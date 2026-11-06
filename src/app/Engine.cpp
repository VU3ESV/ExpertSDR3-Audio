#include "app/Engine.h"

#include "audio/PortAudioSink.h"
#include "dsp/NrProcessor.h"

#include <QEventLoop>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPair>

#include <algorithm>
#include <cmath>
#include <memory>

Q_LOGGING_CATEGORY(lcEngine, "esa.engine")

namespace esa {

namespace {

constexpr int kHousekeepingMs = 100;
// With no audio for this long, ExpertSDR3 should not stay muted: the operator
// would otherwise hear nothing from either program.
constexpr int kStreamDeadMs = 3000;
constexpr int kSinkRetryMs = 3000;
constexpr int kShutdownWaitMs = 1500;

float toDb(float linear)
{
    return linear > 1e-6f ? 20.0f * std::log10(linear) : -120.0f;
}

float peakOf(const float* samples, qsizetype count)
{
    float peak = 0.0f;
    for (qsizetype i = 0; i < count; ++i) peak = std::max(peak, std::fabs(samples[i]));
    return peak;
}

std::unique_ptr<AudioSink> openRealSink(const AppConfig& c, QString* error)
{
    if (c.deviceKey == kNullDeviceKey) return std::make_unique<NullSink>();
    return PortAudioSink::open(c.deviceKey, c.chainRate, c.bufferMs, c.deviceRate, error);
}

} // namespace

Engine::Engine(const QString& settingsPath, SinkFactory sinkFactory, QObject* parent)
    : QObject(parent),
      m_sinkFactory(sinkFactory ? std::move(sinkFactory) : SinkFactory(openRealSink)),
      m_settings(std::make_unique<Settings>(settingsPath)),
      m_tci(new TciClient(this)),
      m_housekeeping(this),  // parented so moveToThread() carries them along
      m_closeTimer(this),
      m_nrPool(this)
{
    qRegisterMetaType<EngineStatus>();
    qRegisterMetaType<AppConfig>();
    qRegisterMetaType<ChainSettings>();
    qRegisterMetaType<QList<AudioDeviceInfo>>();
    qRegisterMetaType<tci::StreamHeader>();

    m_clock.start();

    prepareMuteFor(QString());

    connect(m_tci, &TciClient::stateChanged, this, &Engine::onConnectionState);
    connect(m_tci, &TciClient::muteReported, this, &Engine::onMuteReported);
    connect(m_tci, &TciClient::modulationChanged, this, &Engine::onModulationChanged);
    connect(m_tci, &TciClient::transmitChanged, this, &Engine::onTransmitChanged);
    connect(m_tci, &TciClient::audioBlock, this, &Engine::onAudioBlock);
    connect(m_tci, &TciClient::malformedBlock, this, [this] { ++m_malformedBlocks; });

    m_nrPool.setMaxThreadCount(1);

    m_closeTimer.setInterval(20);
    connect(&m_closeTimer, &QTimer::timeout, this, &Engine::continueClosing);

    m_housekeeping.setInterval(kHousekeepingMs);
    connect(&m_housekeeping, &QTimer::timeout, this, &Engine::onHousekeeping);
    m_housekeeping.start();
}

Engine::~Engine()
{
    m_nrPool.waitForDone();
}

// ── Configuration ───────────────────────────────────────────────────────────

void Engine::prepareMuteFor(const QString& host)
{
    if (m_mute && host == m_muteHost) return;  // keep what it knows, such as an operator override

    const Settings::MuteOwnership stored = m_settings->loadMuteOwnership(host);
    MuteController::Actions actions;
    actions.sendMute = [this](bool muted) { m_tci->sendMute(muted); };
    actions.persist = [this, host](const MuteController::Ownership& o) {
        m_settings->saveMuteOwnership(host, {o.owned, o.priorMuted});
    };
    m_mute = std::make_unique<MuteController>(std::move(actions),
                                              MuteController::Ownership{stored.owned, stored.priorMuted});
    m_muteHost = host;
}

void Engine::configure(const AppConfig& config)
{
    m_config = config;
    rebuildChain(config.chainRate);
    if (m_sink) {
        m_sink->setGainDb(config.outputGainDb);
        m_sink->setBoost(config.boost);
    }
    updateMuteWanted();
}

void Engine::rebuildChain(int sampleRate)
{
    m_chain = std::make_unique<RxChain>(sampleRate);
    m_chain->setStageOrder(m_config.stageOrder);
    // In table order, because the gate's mode must land before ratio and floor.
    for (const ParamSpec& spec : RxChain::stageParams()) {
        m_chain->setStageParam(spec.key, paramValue(m_config, spec));
    }
    updateNrBypass();
    startNrBuild();
}

void Engine::startNrBuild()
{
    const quint64 generation = ++m_nrGeneration;
    m_chain->setNrProcessor(nullptr);
    m_nrFailed = false;

    const NrMethod* method = findNrMethod(m_config.nrMethod);
    if (!method) {
        m_nrPreparing = false;
        return;
    }

    m_nrPreparing = true;
    const int rate = m_chain->sampleRate();
    // Everything the job needs is captured by value: it runs on the pool, and this
    // object is only touched again via the queued adoption back on our own thread.
    QList<QPair<QString, double>> values;
    for (const ParamSpec& spec : method->params) {
        values.append({spec.key, paramValue(m_config, spec)});
    }
    const auto create = method->create;
    m_nrPool.start([this, generation, rate, values, create] {
        std::shared_ptr<NrProcessor> built(create(rate).release());
        if (built) {
            for (const auto& value : values) built->setParam(value.first, value.second);
        }
        QMetaObject::invokeMethod(this, [this, generation, built] {
            if (generation != m_nrGeneration) return;  // superseded
            m_nrPreparing = false;
            if (!built) {
                m_nrFailed = true;
                qCWarning(lcEngine) << "Could not start NR method" << m_config.nrMethod;
                return;
            }
            m_chain->setNrProcessor(built);
        }, Qt::QueuedConnection);
    });
}

void Engine::setNrMethod(const QString& id)
{
    m_config.nrMethod = id;
    startNrBuild();
}

void Engine::setParam(const QString& key, double value)
{
    m_config.params.insert(key, value);
    if (!m_chain) return;
    if (m_chain->setStageParam(key, value)) return;
    // Values for the other methods wait in m_config until those are selected.
    // Adapters match on the part after the dot, so a key must only reach the method
    // it names: "mnr.strength" would otherwise set NNR's "strength" too.
    auto* processor = m_chain->nrProcessor();
    if (processor && key.startsWith(m_config.nrMethod + QLatin1Char('.'))) {
        processor->setParam(key, value);
    }
}

void Engine::applyChainSettings(const ChainSettings& chain)
{
    const QString previousMethod = m_config.nrMethod;
    applyChain(m_config, chain);
    if (!m_chain) return;

    // In table order, because the gate's mode must land before ratio and floor.
    for (const ParamSpec& spec : RxChain::stageParams()) {
        m_chain->setStageParam(spec.key, paramValue(m_config, spec));
    }
    m_chain->setStageOrder(m_config.stageOrder);
    updateNrBypass();

    if (m_config.nrMethod != previousMethod || m_nrFailed) {
        startNrBuild();  // reads the new values, so they need setting first
    } else if (const NrMethod* method = findNrMethod(m_config.nrMethod)) {
        if (auto* processor = m_chain->nrProcessor()) {
            for (const ParamSpec& spec : method->params) processor->setParam(spec.key, paramValue(m_config, spec));
        }
    }
}

void Engine::setStageOrder(const QStringList& order)
{
    m_config.stageOrder = order;
    if (m_chain) m_chain->setStageOrder(order);
}

void Engine::setNrAutoBypass(bool on)
{
    m_config.nrAutoBypass = on;
    updateNrBypass();
}

void Engine::updateNrBypass()
{
    if (m_chain) {
        m_chain->setNrBypassed(m_config.nrAutoBypass && modeBypassesNr(m_modulation));
    }
}

void Engine::setCapture(const QString& basePath)
{
    m_captureBase = basePath;
    m_captureBefore.close();
    m_captureAfter.close();
    if (basePath.isEmpty() || !m_chain) return;
    if (!m_captureBefore.open(basePath + QStringLiteral(".before.wav"), m_chain->sampleRate())
        || !m_captureAfter.open(basePath + QStringLiteral(".after.wav"), m_chain->sampleRate())) {
        qCWarning(lcEngine) << "Could not open capture files at" << basePath;
    }
}

// ── Radio ───────────────────────────────────────────────────────────────────

void Engine::connectRadio()
{
    if (m_closing) {
        m_connectAfterClosing = true;  // the previous radio is still getting its mute back
        return;
    }
    m_sessionActive = true;
    m_audioConfirmed = false;
    m_lastBlockMs = -1;
    m_counterValid = false;
    prepareMuteFor(m_config.host);
    if (!m_chain) rebuildChain(m_config.chainRate);
    openSink();
    requestAudio();
    m_tci->open(m_config.host, m_config.port, m_config.fallbackPorts);
    emitStatus();
}

void Engine::disconnectRadio()
{
    m_connectAfterClosing = false;
    if (m_closing) return;

    m_sessionActive = false;
    m_audioConfirmed = false;
    updateMuteWanted();

    // Give the restore a chance to land before the socket closes.
    if (!m_mute->settled() && m_tci->state() == TciClient::State::Ready) {
        m_closing = true;
        m_closeDeadlineMs = nowMs() + kShutdownWaitMs;
        m_closeTimer.start();
        return;
    }
    finishClosing();
}

void Engine::continueClosing()
{
    m_mute->tick(nowMs());
    if (m_mute->settled() || nowMs() >= m_closeDeadlineMs
        || m_tci->state() != TciClient::State::Ready) {
        finishClosing();
    }
}

void Engine::finishClosing()
{
    m_closeTimer.stop();
    m_closing = false;
    m_tci->stopAudio();
    m_tci->flush();
    m_tci->close();
    closeSink();
    emitStatus();
    if (m_connectAfterClosing) {
        m_connectAfterClosing = false;
        connectRadio();
    }
}

void Engine::setReceiver(int receiver)
{
    if (receiver == m_config.receiver) return;
    m_config.receiver = receiver;
    if (m_chain) m_chain->reset();  // a new source: old noise profiles do not apply
    m_counterValid = false;
    // Pick up the new receiver's mode now; the server only pushes it on change.
    const auto& receivers = m_tci->radio().receivers;
    onModulationChanged(receiver, receivers.value(receiver).modulation);
    if (m_sessionActive) requestAudio();
}

void Engine::requestAudio()
{
    TciClient::AudioRequest request;
    request.receiver = m_config.receiver;
    request.sampleRate = m_config.chainRate;
    // Real samples per block, targeting about 10.7 ms in stereo at either rate.
    request.samplesPerBlock = m_config.chainRate == 24000 ? 512 : 1024;
    m_tci->requestAudio(request);
}

void Engine::onConnectionState(TciClient::State state)
{
    if (state == TciClient::State::Initializing) {
        m_mute->onConnected();
    } else if (state == TciClient::State::Connecting || state == TciClient::State::Disconnected) {
        m_mute->onDisconnected();
        m_counterValid = false;
        m_modulation.clear();
    } else if (state == TciClient::State::Ready) {
        emit connectedPortChanged(m_tci->connectedPort());
        // Whatever arrives on this new connection is a fresh stream.
        if (m_chain) m_chain->reset();
    }
    emitStatus();
}

void Engine::onMuteReported(bool muted)
{
    m_mute->onServerMute(muted, nowMs());
}

void Engine::onModulationChanged(int receiver, const QString& modulation)
{
    if (receiver != m_config.receiver) return;
    m_modulation = modulation;
    m_complexStereo = modeIsComplexStereo(modulation);
    updateNrBypass();
}

void Engine::onTransmitChanged(int receiver, bool transmitting)
{
    if (receiver == m_config.receiver && m_chain) m_chain->setTransmitBypass(transmitting);
}

// ── Output ──────────────────────────────────────────────────────────────────

void Engine::openSink()
{
    // Not closeSink(): re-evaluating the mute here, with no output open, would unmute
    // ExpertSDR3 and re-mute it a moment later on every device change.
    m_sink.reset();
    QString error;
    m_sink = m_sinkFactory(m_config, &error);
    m_outputError = m_sink ? QString() : error;
    if (m_sink) {
        m_sink->setGainDb(m_config.outputGainDb);
        m_sink->setBoost(m_config.boost);
        m_sink->setMuted(m_outputMuted);
        m_outputDeviceName = m_sink->deviceName();
    } else {
        m_outputDeviceName.clear();
        qCWarning(lcEngine) << "Output failed:" << error;
    }
    updateMuteWanted();
}

void Engine::closeSink()
{
    m_sink.reset();
    updateMuteWanted();
}

void Engine::setOutputDevice(const QString& deviceKey)
{
    m_config.deviceKey = deviceKey;
    if (m_sessionActive) openSink();
}

void Engine::setBufferMs(int ms)
{
    m_config.bufferMs = ms;
    if (m_sink) m_sink->setTargetLatencyMs(ms);
}

void Engine::setOutputGainDb(double db)
{
    m_config.outputGainDb = db;
    if (m_sink) m_sink->setGainDb(db);
}

void Engine::setBoost(bool on)
{
    m_config.boost = on;
    if (m_sink) m_sink->setBoost(on);
}

void Engine::setOutputMuted(bool muted)
{
    m_outputMuted = muted;
    if (m_sink) m_sink->setMuted(muted);
}

void Engine::refreshDevices()
{
    const bool hadSink = m_sink != nullptr;
    // Close first: PortAudio only rescans once every holder has let go. The mute is
    // not re-evaluated until openSink() below, so ExpertSDR3 stays as it is throughout.
    m_sink.reset();
    emit devicesChanged(audio::listOutputDevices());
    if (hadSink) openSink();
}

// ── Mute policy ─────────────────────────────────────────────────────────────

void Engine::setMuteExpert(bool on)
{
    m_config.muteExpert = on;
    updateMuteWanted();
}

void Engine::updateMuteWanted()
{
    // Only silence ExpertSDR3 once this app is demonstrably producing sound.
    // m_audioConfirmed latches across a reconnect, so a network blip does not
    // briefly unmute ExpertSDR3 and let both programs play at once.
    const bool wanted = m_config.muteExpert && m_sessionActive && m_sink && m_sink->alive()
                        && m_audioConfirmed;
    m_mute->setWanted(wanted, nowMs());
}

// ── Audio path ──────────────────────────────────────────────────────────────

void Engine::onAudioBlock(const tci::StreamHeader& header, const QByteArray& frame)
{
    if (int(header.receiver) != m_config.receiver || !m_chain) return;

    const int frames = tci::decodeToStereo(header, frame.constData() + tci::kHeaderBytes,
                                           frame.size() - tci::kHeaderBytes, m_decoded);
    if (frames < 0) {
        ++m_malformedBlocks;
        return;
    }
    if (frames == 0) return;

    // The server could ignore our rate request. Follow what it actually sends if the
    // chain supports that rate, and drop it otherwise rather than play it at the
    // wrong speed.
    if (int(header.sampleRate) != m_chain->sampleRate()) {
        if (header.sampleRate != 24000 && header.sampleRate != 48000) {
            ++m_malformedBlocks;
            return;
        }
        qCInfo(lcEngine) << "Stream rate is" << header.sampleRate << "Hz; rebuilding the chain";
        m_config.chainRate = int(header.sampleRate);
        rebuildChain(m_config.chainRate);
        openSink();
        setCapture(m_captureBase);
    }

    // The packet counter restarts on TX/RX switches, so a backwards jump is not a
    // loss; only a forward gap is.
    if (m_counterValid && header.count > m_lastCounter + 1) {
        m_droppedBlocks += header.count - m_lastCounter - 1;
    }
    m_lastCounter = header.count;
    m_counterValid = true;

    if (m_complexStereo) {
        // In DIGL/DIGU the pair is I/Q. The I component alone is the real audio.
        for (int i = 0; i < frames; ++i) m_decoded[size_t(i) * 2 + 1] = m_decoded[size_t(i) * 2];
    }

    m_input = QByteArray::fromRawData(reinterpret_cast<const char*>(m_decoded.data()),
                                      qsizetype(frames) * 2 * qsizetype(sizeof(float)));
    m_inputPeak = std::max(m_inputPeak, peakOf(m_decoded.data(), qsizetype(frames) * 2));
    m_captureBefore.write(m_decoded.data(), frames);

    QElapsedTimer dsp;
    dsp.start();
    const QByteArray& output = m_chain->process(m_input);
    m_dspNanos += dsp.nsecsElapsed();

    const int outFrames = int(output.size() / (2 * sizeof(float)));
    if (outFrames > 0) {
        const auto* samples = reinterpret_cast<const float*>(output.constData());
        m_outputPeak = std::max(m_outputPeak, peakOf(samples, qsizetype(outFrames) * 2));
        m_captureAfter.write(samples, outFrames);
        if (m_sink) m_sink->write(samples, outFrames);
    }

    ++m_blocks;
    m_lastBlockMs = nowMs();
    if (!m_audioConfirmed) {
        m_audioConfirmed = true;
        updateMuteWanted();
    }
}

void Engine::onHousekeeping()
{
    const qint64 now = nowMs();
    m_mute->tick(now);

    // The device can vanish underneath us (unplugged). Hand the audio back to
    // ExpertSDR3 at once, then keep trying to reopen.
    if (m_sink && !m_sink->alive()) {
        m_outputError = QStringLiteral("Output device stopped");
        qCWarning(lcEngine) << "Output device stopped";
        closeSink();
        m_nextSinkRetryMs = now + kSinkRetryMs;
    }
    if (!m_sink && m_sessionActive && now >= m_nextSinkRetryMs) {
        m_nextSinkRetryMs = now + kSinkRetryMs;
        openSink();
    }

    if (m_audioConfirmed && m_tci->state() == TciClient::State::Ready && m_lastBlockMs >= 0
        && now - m_lastBlockMs > kStreamDeadMs) {
        m_audioConfirmed = false;
    }
    updateMuteWanted();

    const qint64 windowMs = now - m_dspWindowMs;
    if (windowMs >= 1000) {
        m_dspLoad = double(m_dspNanos) / (double(windowMs) * 1e6);
        m_dspNanos = 0;
        m_dspWindowMs = now;
        m_blocksPerSecond = double(m_blocks - m_blocksAtLastRate) * 1000.0 / double(windowMs);
        m_blocksAtLastRate = m_blocks;
    }

    emitStatus();
    m_inputPeak = 0.0f;
    m_outputPeak = 0.0f;
}

void Engine::emitStatus()
{
    EngineStatus s;
    const RadioState& radio = m_tci->radio();
    s.connection = m_tci->state();
    s.connectionError = m_tci->lastError();
    s.serverName = radio.serverName;
    s.serverVersion = radio.serverVersion;
    s.device = radio.device;
    s.receiveOnly = radio.receiveOnly;
    s.trxCount = radio.trxCount;
    s.connectedPort = m_tci->connectedPort();
    s.receiver = m_config.receiver;
    const RadioState::Receiver rx = radio.receivers.value(m_config.receiver);
    s.frequencyHz = rx.frequencyHz;
    s.modulation = rx.modulation;
    s.receiverEnabled = rx.enabled;
    s.transmitting = rx.transmitting;

    s.mutePhase = m_mute->phase();

    s.nrMethod = m_config.nrMethod;
    s.nrPreparing = m_nrPreparing;
    s.nrFailed = m_nrFailed;
    if (m_chain) {
        s.nrBypassedByMode = !m_config.nrMethod.isEmpty() && m_chain->nrBypassed();
        s.nrGainDb = toDb(m_chain->nrGain());
    }

    s.outputDevice = m_outputDeviceName;
    s.outputError = m_outputError;
    s.outputOpen = m_sink != nullptr;
    if (m_sink) s.sink = m_sink->stats();
    s.inputPeakDb = toDb(m_inputPeak);
    s.outputPeakDb = toDb(m_outputPeak);
    s.audioFlowing = m_audioConfirmed && m_lastBlockMs >= 0 && nowMs() - m_lastBlockMs < 1000;
    s.blocksPerSecond = m_blocksPerSecond;
    s.blocks = m_blocks;
    s.droppedBlocks = m_droppedBlocks;
    s.malformedBlocks = m_malformedBlocks;
    s.dspLoad = m_dspLoad;
    emit statusChanged(s);
}

void Engine::shutdown()
{
    m_connectAfterClosing = false;
    m_housekeeping.stop();
    if (m_sessionActive) disconnectRadio();

    // Waiting here is safe where it was not in disconnectRadio(): the caller is blocked until this
    // returns, so nothing can be queued that would pull the engine out from under it.
    if (m_closing) {
        QEventLoop loop;
        QTimer poll;
        connect(&poll, &QTimer::timeout, &loop, [this, &loop] {
            if (!m_closing) loop.quit();
        });
        poll.start(10);
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    m_connectAfterClosing = false;

    m_captureBefore.close();
    m_captureAfter.close();
    m_nrPool.waitForDone();
}

} // namespace esa
