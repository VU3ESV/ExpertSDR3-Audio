#include "app/Engine.h"

#include "app/RecordNaming.h"
#include "audio/PortAudioSink.h"
#include "dsp/NrProcessor.h"

#include <QCoreApplication>
#include <QDateTime>
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
// With no audio for this long, the stage meters fall to silence rather than freeze.
constexpr int kMeterIdleMs = 250;
// PLAY's pump. Each tick hands the output what is due by then, so the tick only sets how
// lumpy the delivery is, not its rate.
constexpr int kPlayTickMs = 10;
// An output opened for a playback alone holds about its buffer setting when the file ends,
// and its drift control lets it run up to 150 ms over that (PlaybackBuffer::kOverrunSlackMs).
// It closes once that much more time has passed, so the end of the file is heard.
constexpr int kPlayOutSlackMs = 200;

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

// Never prepared and never processed, so its meters read as silence: -120 dB and no
// gain reduction. Copied over a stage's meters whenever that stage did not run.
const StageSet& idleStages()
{
    static const StageSet idle{};
    return idle;
}

// The audio a recording's part holds: what was pushed into it, less what its ring had no
// room for.
double secondsIn(const RecordSession& session, quint64 pushed)
{
    const quint64 dropped = std::min(session.droppedFrames(), pushed);
    return double(pushed - dropped) / double(std::max(1, session.sampleRate()));
}

} // namespace

Engine::Engine(const QString& settingsPath, SinkFactory sinkFactory, QObject* parent,
               Recorder::Options recorderOptions)
    : QObject(parent),
      m_sinkFactory(sinkFactory ? std::move(sinkFactory) : SinkFactory(openRealSink)),
      m_settings(std::make_unique<Settings>(settingsPath)),
      m_tci(new TciClient(this)),
      m_taps(std::make_shared<LiveTaps>()),
      m_housekeeping(this),  // parented so moveToThread() carries them along
      m_meterRest(this),
      m_closeTimer(this),
      m_nrPool(this),
      m_recorderInbox(this),
      m_recorder(std::move(recorderOptions), this),
      m_playTimer(this),
      m_playDrain(this)
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

    m_meterRest.setSingleShot(true);
    m_meterRest.setTimerType(Qt::PreciseTimer);
    connect(&m_meterRest, &QTimer::timeout, this, [this] {
        if (m_lastBlockMs < 0 || nowMs() - m_lastBlockMs > kMeterIdleMs) idleStageMeters();
    });

    // Emitted on the disk thread, so these arrive queued, in the order they happened there.
    connect(&m_recorder, &Recorder::fileOpened, &m_recorderInbox,
            [this](quint64 id, const QString& path) { onRecorderFileOpened(id, path); });
    connect(&m_recorder, &Recorder::fileClosed, &m_recorderInbox,
            [this](quint64 id, const QString& path, quint64 frames, int rate) {
                onRecorderFileClosed(id, path, frames, rate);
            });
    connect(&m_recorder, &Recorder::failed, &m_recorderInbox,
            [this](quint64 id, const QString& reason) { onRecorderFailed(id, reason); });
    connect(&m_recorder, &Recorder::finished, &m_recorderInbox, [this](quint64 id) { onRecorderFinished(id); });

    m_playTimer.setTimerType(Qt::PreciseTimer);
    m_playTimer.setInterval(kPlayTickMs);
    connect(&m_playTimer, &QTimer::timeout, this, &Engine::pumpPlayback);

    m_playDrain.setSingleShot(true);
    connect(&m_playDrain, &QTimer::timeout, this, [this] {
        if (m_play) return;  // a new playback has the output now
        releasePlaybackSink();
        emitStatus();
    });
}

Engine::~Engine()
{
    m_recorder.shutdown();  // a file still open is finished, whole, before anything else goes
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
    // A new chain knows nothing of the taps or of BYPASS, so hand both on.
    m_chain->setPostEqTap(&m_taps->eqTap, &m_taps->eqWanted);
    m_chain->setOperatorBypass(m_rxBypassed);
    m_taps->chainRate.store(sampleRate, std::memory_order_relaxed);
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

void Engine::setRxBypassed(bool on)
{
    m_rxBypassed = on;
    if (m_chain) m_chain->setOperatorBypass(on);
    emitStatus();  // at once, so the button can follow the engine rather than the click
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

// ── Recording ───────────────────────────────────────────────────────────────

void Engine::startRecording(const QString& folder)
{
    beginRecording(folder, QString());
}

void Engine::startRecordingToFile(const QString& path)
{
    beginRecording(QString(), path);
}

void Engine::beginRecording(const QString& folder, const QString& file)
{
    if (m_recording) return;  // already on, as the status says
    // A session the operator started counts, even while its link is still coming up: the file
    // is only named once there is a radio to name it from, and opened once there is audio.
    if (!m_sessionActive) {
        refuseRecording(QStringLiteral("Connect to a radio first"));
        return;
    }
    if (folder.isEmpty() && file.isEmpty()) {
        refuseRecording(QStringLiteral("No folder to record into"));
        return;
    }
    m_recording = true;
    m_recordFolder = folder;
    m_recordFile = file;
    m_recordParts.clear();
    m_recordFramesPushed = 0;
    m_recordSecondsBefore = 0.0;
    m_recordDroppedBefore = 0;
    m_recordPath.clear();
    m_recordError.clear();
    m_recordRefused = false;
    if (m_tci->state() == TciClient::State::Ready) openRecordingSession();  // else when it is
    emitStatus();  // at once, so the button follows the engine
}

void Engine::refuseRecording(const QString& reason)
{
    m_recordError = reason;
    m_recordRefused = true;
    emit recordingFailed(reason);
    emitStatus();  // at once, so the button flips back
}

void Engine::openRecordingSession()
{
    const int rate = m_chain ? m_chain->sampleRate() : m_config.chainRate;
    RecordRequest request;
    if (!m_recordFile.isEmpty()) {
        request = RecordRequest::forFile(m_recordFile, rate);
    } else {
        const RadioState::Receiver rx = m_tci->radio().receivers.value(m_config.receiver);
        request = {m_recordFolder,
                   RecordNaming::baseName(QDateTime::currentDateTimeUtc(), rx.frequencyHz, rx.modulation,
                                          m_config.receiver),
                   rate};
    }
    m_record = m_recorder.start(request);
    if (!m_record) {
        endRecording(QStringLiteral("Recording is not available while the app is closing"));
        return;
    }
    m_recordParts.insert(m_record->id());
    m_recordSessions.insert(m_record->id());
    // m_recordPath waits for the disk thread: it names the file once there is one, and only it
    // knows whether that took a _1.
}

// Frames at two rates never share a file: the recording goes on in the next _partN.
void Engine::rollRecording(int sampleRate)
{
    std::shared_ptr<RecordSession> next = m_recorder.roll(m_record, sampleRate);
    if (!next) {
        // It had already failed, and roll() has ended it.
        endRecording(m_record->failed() ? m_record->failureReason()
                                        : QStringLiteral("Could not go on recording at %1 Hz").arg(sampleRate));
        return;
    }
    m_recordSecondsBefore += secondsIn(*m_record, m_recordFramesPushed);
    m_recordDroppedBefore += m_record->droppedFrames();  // final: push() has stopped for it
    m_recordFramesPushed = 0;
    m_record = std::move(next);
    m_recordParts.insert(m_record->id());
    m_recordSessions.insert(m_record->id());
}

void Engine::endRecording(const QString& failure)
{
    if (!m_recording) return;
    m_recording = false;
    if (m_record) {
        m_recordSecondsBefore += secondsIn(*m_record, m_recordFramesPushed);
        m_recordDroppedBefore += m_record->droppedFrames();
        m_recordFramesPushed = 0;
        // Its last file is what PLAY will play, once the disk thread has closed it.
        m_endingRecordings.insert(m_record->id(), !failure.isEmpty());
        m_recorder.finish(m_record);
        m_record.reset();
    }
    if (!failure.isEmpty()) {
        m_recordError = failure;
        m_recordRefused = false;
        emit recordingFailed(failure);
    }
}

void Engine::stopRecording()
{
    if (!m_recording) return;
    endRecording(QString());
    emitStatus();
}

void Engine::onRecorderFileOpened(quint64 id, const QString& path)
{
    if (m_captureSessions.contains(id)) {
        m_captureFiles.append(path);
    } else if (m_recordParts.contains(id)) {
        m_recordPath = path;  // also after a stop: the last drain may be what opened it
    } else {
        return;
    }
    emitStatus();
}

void Engine::onRecorderFileClosed(quint64 id, const QString& path, quint64, int sampleRate)
{
    // The disk thread finishes one recording before it starts the next, so the last REC file
    // closed before a recording's end is reported is that recording's last file.
    if (!m_recordSessions.contains(id)) return;
    m_closedRecordPath = path;
    m_closedRecordRate = sampleRate;
}

void Engine::onRecorderFailed(quint64 id, const QString& reason)
{
    if (m_captureSessions.contains(id)) {
        if (m_captureError.isEmpty()) m_captureError = reason;
        if (m_captureBefore && m_captureBefore->id() == id) m_captureBefore.reset();
        if (m_captureAfter && m_captureAfter->id() == id) m_captureAfter.reset();
        emitStatus();
        return;
    }
    if (m_recording && m_recordParts.contains(id)) {
        endRecording(reason);
        emitStatus();
        return;
    }
    // A recording already stopped lost its end in the last write. Worth saying, but once: a
    // part that fails takes the part after it down too, with the same reason.
    const auto ending = m_endingRecordings.find(id);
    if (ending != m_endingRecordings.end() && !ending.value()) {
        ending.value() = true;
        // A newer recording may be under way by now. Its status is its own: a red "stopped"
        // over a healthy recording, naming another file, would only mislead.
        if (!m_recording) {
            m_recordError = reason;
            m_recordRefused = false;
        }
        emit recordingFailed(reason);
        emitStatus();
    }
}

void Engine::onRecorderFinished(quint64 id)
{
    m_captureSessions.remove(id);
    m_recordSessions.remove(id);
    if (!m_endingRecordings.remove(id)) return;
    // Unless a newer recording has the status by now, it names what this one kept: its last
    // file, or none at all when no audio ever reached the disk.
    if (!m_recording) m_recordPath = m_closedRecordPath;
    // The disk thread is done with this recording, so its last file is whole: PLAY can have it.
    if (!m_closedRecordPath.isEmpty()) {
        m_lastRecordingPath = m_closedRecordPath;
        m_lastRecordingRate = m_closedRecordRate;
        m_closedRecordPath.clear();
    }
    emitStatus();
}

void Engine::setCapture(const QString& basePath)
{
    finishCapture();
    m_captureBase = basePath;
    m_captureFiles.clear();
    m_captureError.clear();
    if (basePath.isEmpty()) return;
    const int rate = m_chain ? m_chain->sampleRate() : m_config.chainRate;
    m_captureBefore = m_recorder.start(RecordRequest::forFile(basePath + QStringLiteral(".before.wav"), rate));
    m_captureAfter = m_recorder.start(RecordRequest::forFile(basePath + QStringLiteral(".after.wav"), rate));
    for (const auto* session : {&m_captureBefore, &m_captureAfter}) {
        if (*session) m_captureSessions.insert((*session)->id());
    }
}

void Engine::pushCapture(std::shared_ptr<RecordSession>& session, const float* stereo, int frames, int sampleRate)
{
    if (!session) return;
    if (session->sampleRate() != sampleRate) {
        const std::shared_ptr<RecordSession> next = m_recorder.roll(session, sampleRate);
        if (!next) {
            if (m_captureError.isEmpty()) {
                m_captureError = session->failed() ? session->failureReason()
                                                   : QStringLiteral("Could not go on capturing at %1 Hz").arg(sampleRate);
            }
            session.reset();
            return;
        }
        m_captureSessions.insert(next->id());
        session = next;
    }
    session->push(stereo, frames);
}

void Engine::finishCapture()
{
    m_recorder.finish(m_captureBefore);
    m_recorder.finish(m_captureAfter);
    m_captureBefore.reset();
    m_captureAfter.reset();
}

void Engine::finishRecordings()
{
    endRecording(QString());
    finishCapture();
    // Waits for the disk thread, so every file is whole by the time the engine says it is
    // disconnected or gone.
    m_recorder.finishAll();
}

// ── Playback ────────────────────────────────────────────────────────────────

void Engine::startPlayback()
{
    if (m_play) return;  // already playing, as the status says
    if (m_lastRecordingPath.isEmpty()) {
        refusePlayback(QStringLiteral("Nothing has been recorded yet"));
        return;
    }
    if (m_closing) {
        refusePlayback(QStringLiteral("Still disconnecting from the radio"));
        return;
    }
    if (m_sessionActive && !m_sink) {
        refusePlayback(m_outputError.isEmpty() ? QStringLiteral("No output device")
                                               : QStringLiteral("Output: %1").arg(m_outputError));
        return;
    }
    const int rate = m_sink ? m_sinkRate : m_config.chainRate;
    if (m_lastRecordingRate != rate) {
        refusePlayback(QStringLiteral("This recording is at %1 Hz, but the output runs at %2 Hz")
                           .arg(m_lastRecordingRate)
                           .arg(rate));
        return;
    }
    // Still playing out the last playback's end: this one takes the output over as it is.
    m_playDrain.stop();
    if (!m_sink) {
        // No radio: the output opens for this playback alone. With no session the mute policy
        // wants nothing (updateMuteWanted), so ExpertSDR3 is left exactly as it is.
        openSink();
        if (!m_sink) {
            refusePlayback(QStringLiteral("Output: %1").arg(m_outputError));
            return;
        }
        m_playOwnsSink = true;
    }
    m_play = m_recorder.play(m_lastRecordingPath, rate);
    if (!m_play) {
        if (m_playOwnsSink) {
            m_playOwnsSink = false;
            closeSink();
        }
        refusePlayback(QStringLiteral("Playback is not available while the app is closing"));
        return;
    }
    m_playRate = rate;
    m_playStarted = false;
    // Sized here, not per tick: the pacer hands over at most a second at a time.
    const size_t needed = size_t(rate) * 2;
    if (m_playScratch.size() < needed) m_playScratch.assign(needed, 0.0f);
    m_playTimer.start();
    emitStatus();
}

void Engine::stopPlayback()
{
    endPlayback(QString());
}

void Engine::refusePlayback(const QString& reason)
{
    emit playbackFailed(reason);
    emitStatus();  // at once, so the button flips back
}

void Engine::pumpPlayback()
{
    if (!m_play) {
        m_playTimer.stop();
        return;
    }
    if (m_play->failed()) {
        endPlayback(m_play->failureReason());
        return;
    }
    if (!m_sink || !m_sink->alive()) {
        endPlayback(QStringLiteral("Output device stopped"));
        return;
    }
    if (!m_play->ready()) return;  // the disk thread is still opening the file

    const qint64 now = m_clock.nsecsElapsed();
    if (!m_playStarted) {
        m_playPacer.start(m_play->sampleRate(), now);
        m_playStarted = true;
    }
    SpscStereoRing& ring = m_play->ring();
    const qint64 due = std::min(m_playPacer.take(now, qint64(ring.size())), qint64(m_playScratch.size() / 2));
    if (due > 0) {
        const size_t frames = ring.read(m_playScratch.data(), size_t(due));
        const float* stereo = m_playScratch.data();
        if (m_taps->outWanted.load(std::memory_order_relaxed)) m_taps->outTap.write(stereo, frames);
        m_sink->write(stereo, int(frames));
    }
    // The whole file has been handed over. What the output still buffers plays on.
    if (m_play->atEnd() && ring.size() == 0) endPlayback(QString(), true);
}

void Engine::endPlayback(const QString& failure, bool playOut)
{
    if (!m_play) return;
    m_playTimer.stop();
    m_recorder.stopPlaying(m_play);
    m_play.reset();
    m_playStarted = false;
    if (m_playOwnsSink && playOut && m_sink && !m_sessionActive) {
        // Closing now would cut off the last part of the file, which is still in the output's
        // buffer. The status says the playback is over meanwhile.
        m_playDrain.start(std::max(0, m_config.bufferMs) + kPlayOutSlackMs);
    } else {
        releasePlaybackSink();
    }
    if (!failure.isEmpty()) emit playbackFailed(failure);
    emitStatus();
}

void Engine::releasePlaybackSink()
{
    m_playDrain.stop();
    if (!m_playOwnsSink) return;
    m_playOwnsSink = false;
    if (!m_sessionActive) closeSink();
}

// ── Radio ───────────────────────────────────────────────────────────────────

void Engine::connectRadio()
{
    endPlayback(QString());  // PLAY gives way to the radio
    releasePlaybackSink();   // and so does the end of one, still playing out
    if (m_recordRefused) {   // the refusal is answered
        m_recordError.clear();
        m_recordRefused = false;
    }
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
    endPlayback(QString());
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
    // After the mute restore, which must never wait on a disk.
    finishRecordings();
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
        // A REC pressed while the link came up: the radio's frequency and mode are known now,
        // and its audio follows.
        if (m_recording && !m_record) openRecordingSession();
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
    m_sinkRate = m_sink ? m_config.chainRate : 0;
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
    // An output reopened at another rate, after the stream's changed, cannot go on playing a
    // file at the old one.
    if (m_play && m_sink && m_sinkRate != m_playRate) {
        endPlayback(QStringLiteral("The output now runs at %1 Hz; this recording is at %2 Hz")
                        .arg(m_sinkRate)
                        .arg(m_playRate));
    }
}

void Engine::closeSink()
{
    m_sink.reset();
    m_sinkRate = 0;
    updateMuteWanted();
}

void Engine::setOutputDevice(const QString& deviceKey)
{
    m_config.deviceKey = deviceKey;
    if (!m_play) releasePlaybackSink();  // only the end of a playback, playing out: not worth moving
    if (m_sessionActive || m_playOwnsSink) openSink();  // a PLAY with no radio moves with it
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
    if (!m_play) releasePlaybackSink();  // as setOutputDevice()
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
        // A recording or capture in progress rolls to its next part when the first block at
        // the new rate reaches it (publishBlock, pushCapture).
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
    const float inputPeak = peakOf(m_decoded.data(), qsizetype(frames) * 2);
    m_inputPeak = std::max(m_inputPeak, inputPeak);
    // The only writer; the window takes it with exchange(0) and may lose one block's
    // peak to the race, which a meter cannot show anyway.
    if (inputPeak > m_taps->inputPeak.load(std::memory_order_relaxed)) {
        m_taps->inputPeak.store(inputPeak, std::memory_order_relaxed);
    }
    pushCapture(m_captureBefore, m_decoded.data(), frames, m_chain->sampleRate());

    QElapsedTimer dsp;
    dsp.start();
    const QByteArray& output = m_chain->process(m_input);
    m_dspNanos += dsp.nsecsElapsed();

    // NR can change the block's length, and while it fills can return nothing.
    const int outFrames = int(output.size() / (2 * sizeof(float)));
    if (outFrames > 0) publishBlock(reinterpret_cast<const float*>(output.constData()), outFrames);

    ++m_blocks;
    m_lastBlockMs = nowMs();
    if (!m_audioConfirmed) {
        m_audioConfirmed = true;
        updateMuteWanted();
    }
}

void Engine::publishBlock(const float* stereo, int frames)
{
    copyStageMeters();
    m_taps->nrGain.store(m_chain->nrGain(), std::memory_order_relaxed);
    m_taps->nrActive.store(m_chain->nrActive(), std::memory_order_relaxed);
    m_taps->blocks.fetch_add(1, std::memory_order_relaxed);

    m_outputPeak = std::max(m_outputPeak, peakOf(stereo, qsizetype(frames) * 2));

    // Recorded whatever happens to the output: a failed device, BYPASS, PLAY. The ring takes
    // it without a lock; the disk thread writes it out.
    const int rate = m_chain->sampleRate();
    if (m_record) {
        if (m_record->sampleRate() != rate) rollRecording(rate);
        if (m_record && !m_record->failed()) {
            m_record->push(stereo, frames);
            m_recordFramesPushed += quint64(frames);
        }
    }
    pushCapture(m_captureAfter, stereo, frames, rate);

    // PLAY has the speakers and the scope meanwhile (pumpPlayback).
    if (m_play) return;
    // Outside the sink's check, so the meter and scope keep running with no device. A
    // full ring drops the rest: the window has fallen behind.
    if (m_taps->outWanted.load(std::memory_order_relaxed)) m_taps->outTap.write(stereo, size_t(frames));
    if (m_sink) m_sink->write(stereo, frames);
}

void Engine::copyStageMeters()
{
    const unsigned ran = m_chain->lastRun();
    const StageSet& live = m_chain->stages();
    const StageSet& idle = idleStages();
    StageSet& view = m_taps->view;
    view.gate.copyMeteringFrom((ran & RxChain::GateBit) ? live.gate : idle.gate);
    view.comp.copyMeteringFrom((ran & RxChain::CompBit) ? live.comp : idle.comp);
    view.tube.copyMeteringFrom((ran & RxChain::TubeBit) ? live.tube : idle.tube);
    view.pudu.copyMeteringFrom((ran & RxChain::PuduBit) ? live.pudu : idle.pudu);
    // The EQ has no meters.
}

void Engine::idleStageMeters()
{
    const StageSet& idle = idleStages();
    StageSet& view = m_taps->view;
    view.gate.copyMeteringFrom(idle.gate);
    view.comp.copyMeteringFrom(idle.comp);
    view.tube.copyMeteringFrom(idle.tube);
    view.pudu.copyMeteringFrom(idle.pudu);
    m_taps->nrActive.store(false, std::memory_order_relaxed);
}

void Engine::onHousekeeping()
{
    const qint64 now = nowMs();
    m_mute->tick(now);

    // Unchecked stages, BYPASS and transmitting are seen block by block. This catches
    // the stream itself stopping, or a disconnect.
    const qint64 sinceBlock = m_lastBlockMs < 0 ? -1 : now - m_lastBlockMs;
    if (sinceBlock < 0 || sinceBlock > kMeterIdleMs) {
        idleStageMeters();
    } else if (sinceBlock + kHousekeepingMs > kMeterIdleMs) {
        m_meterRest.start(int(kMeterIdleMs - sinceBlock) + 2);  // due before the next tick
    }

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
    s.chainRate = m_chain ? m_chain->sampleRate() : m_config.chainRate;
    s.rxBypassed = m_rxBypassed;

    s.recording.active = m_recording;
    s.recording.path = m_recordPath;
    s.recording.seconds = m_recordSecondsBefore;
    s.recording.droppedFrames = m_recordDroppedBefore;
    if (m_record) {
        s.recording.seconds += secondsIn(*m_record, m_recordFramesPushed);
        s.recording.droppedFrames += m_record->droppedFrames();
    }
    s.recording.error = m_recordError;
    s.recording.refused = m_recordRefused && !m_recordError.isEmpty();
    s.playback.active = m_play != nullptr;
    if (m_play && m_playStarted) {  // the file has been read, so its rate is known
        const double rate = m_play->sampleRate();
        s.playback.seconds = double(m_playPacer.taken()) / rate;
        s.playback.totalSeconds = double(m_play->totalFrames()) / rate;
    }
    s.lastRecordingAvailable = !m_lastRecordingPath.isEmpty();
    s.lastRecordingPath = m_lastRecordingPath;
    s.capture.files = m_captureFiles;
    s.capture.error = m_captureError;
    emit statusChanged(s);
}

void Engine::shutdown()
{
    m_connectAfterClosing = false;
    m_housekeeping.stop();
    m_meterRest.stop();
    endPlayback(QString());
    releasePlaybackSink();  // quitting: the end of a playback need not be heard
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

    // After the mute restore: every file is finished and the disk thread stopped, before the
    // engine goes. Quitting asks nothing.
    finishRecordings();
    m_recorder.shutdown();
    // Its last reports (a file's final name, a failure in the last write) are queued to this
    // thread. Taken now, they are in the final status, which headless runs print.
    QCoreApplication::sendPostedEvents(&m_recorderInbox, QEvent::MetaCall);
    emitStatus();
    m_nrPool.waitForDone();
}

} // namespace esa
