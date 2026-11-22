#include "radio/UsbBackend.h"

#include "app/Settings.h"
#include "audio/MicrophonePermission.h"
#include "audio/PortAudioSource.h"
#include "core/Resampler.h"

#include <QLoggingCategory>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(lcUsb, "esa.usb")

namespace esa {

namespace {

// A missing codec is looked for again this often: the radio may be switched on later.
constexpr int kRetryMs = 2000;
// The device is drained this often. Its callbacks bring at most 1024 frames, 21 ms at 48 kHz.
constexpr int kDrainMs = 5;
// With no callback for this long, the device has stopped without saying so (unplugged).
constexpr int kStalledMs = 1000;

std::unique_ptr<AudioSource> openRealSource(const QString& key, int preferredRate, QString* error)
{
    // The sandbox first, so no test sets off the permission prompt.
    if (const QString refusal = audio::realInputRefusal(); !refusal.isEmpty()) {
        if (error) *error = refusal;
        return nullptr;
    }
    switch (audio::inputAccess()) {
    case audio::InputAccess::Undetermined: {
        static bool asked = false;  // macOS shows its question once; asking again only queues
        if (!asked) audio::requestInputAccess();
        asked = true;
        if (error) *error = QStringLiteral("Waiting for you to allow this app to hear the radio's audio");
        return nullptr;
    }
    case audio::InputAccess::Denied:
        if (error) {
            *error = QStringLiteral("macOS does not let this app hear the radio's audio. Allow it in System "
                                    "Settings, Privacy & Security, Microphone.");
        }
        return nullptr;
    case audio::InputAccess::Granted:
        break;
    }
    return PortAudioSource::open(key, preferredRate, error);
}

} // namespace

UsbBackend::UsbBackend(SourceFactory sourceFactory, QObject* parent, Ts990Cat::TransportFactory catTransport)
    : RadioBackend(parent),
      m_sourceFactory(sourceFactory ? std::move(sourceFactory) : SourceFactory(openRealSource)),
      m_catTransport(std::move(catTransport)),
      m_retry(this),
      m_drain(this)
{
    m_clock.start();
    m_retry.setInterval(kRetryMs);
    connect(&m_retry, &QTimer::timeout, this, &UsbBackend::tryOpen);
    m_drain.setTimerType(Qt::PreciseTimer);
    m_drain.setInterval(kDrainMs);
    connect(&m_drain, &QTimer::timeout, this, &UsbBackend::drain);
}

UsbBackend::~UsbBackend() = default;

RadioBackend::Capabilities UsbBackend::capabilities() const
{
    Capabilities c;
    // The main receiver's AF, where the model has it and its CAT is in use.
    c.speaker = m_model.afMute && m_cat && m_cat->active() ? SpeakerControl::Level : SpeakerControl::None;
    c.audioProvesRadio = false;
    const bool catReady = m_cat && m_cat->state() == Ts990Cat::State::Ready;
    c.reportsMode = catReady;
    c.reportsFrequency = catReady;
    return c;
}

void UsbBackend::open(const AppConfig& config)
{
    m_wantOpen = true;
    m_inputKey = config.usbInput;
    m_model = usb::model(config.usbModel);
    setInputTrimDb(config.usbTrimDb);

    m_radio = RadioState();
    m_radio.serverName = m_model.make;
    m_radio.device = m_model.name;
    m_radio.receiveOnly = true;  // as far as this app goes: it never transmits
    m_radio.trxCount = int(m_model.receivers.size());
    for (int i = 0; i < m_radio.trxCount; ++i) {
        RadioState::Receiver rx;
        rx.name = m_model.receivers.at(i);
        m_radio.receivers.insert(i, rx);
    }
    emit radioChanged();

    m_lastError.clear();
    setState(Link::Connecting);
    tryOpen();

    // CAT, where the model has it and a port is chosen. Only the TS-990S's for now.
    if (m_model.hasCat() && !config.usbCatPort.isEmpty()) {
        if (!m_cat) {
            m_cat = new Ts990Cat(m_catTransport, this);
            connect(m_cat, &Ts990Cat::bandChanged, this, &UsbBackend::onCatBand);
            connect(m_cat, &Ts990Cat::stateChanged, this, &RadioBackend::radioChanged);
            connect(m_cat, &Ts990Cat::stateChanged, this, &UsbBackend::updateSpeakerLink);
            connect(m_cat, &Ts990Cat::afLevelRead, this, &RadioBackend::speakerLevelReported);
        }
        m_cat->setReadsAfLevel(m_model.afMute);
        m_cat->open(config.usbCatPort, config.usbCatBaud);
    } else if (m_cat) {
        m_cat->close();
    }
}

RadioBackend::Cat UsbBackend::cat() const
{
    Cat c;
    if (!m_cat || !m_cat->active()) return c;
    c.used = true;
    c.ready = m_cat->state() == Ts990Cat::State::Ready;
    c.port = m_cat->port();
    c.error = c.ready ? QString() : m_cat->lastError();
    return c;
}

bool UsbBackend::speakerLinkUp() const
{
    return m_model.afMute && m_cat && m_cat->state() == Ts990Cat::State::Ready;
}

void UsbBackend::updateSpeakerLink()
{
    const bool up = speakerLinkUp();
    if (up == m_speakerUp) return;
    m_speakerUp = up;
    emit speakerLinkChanged(up);
}

void UsbBackend::setSpeakerLevel(int level)
{
    // Never before the level has been read on this link: what it would overwrite is unknown.
    if (!speakerLinkUp() || !m_cat->afKnown()) {
        qCWarning(lcUsb) << "AF level" << level << "not sent: the radio's own level has not been read on this link";
        return;
    }
    m_cat->setAfLevel(level);
}

void UsbBackend::onCatBand(int receiver)
{
    if (!m_radio.receivers.contains(receiver)) return;
    const Ts990Cat::Band& band = m_cat->band(receiver);
    RadioState::Receiver& rx = m_radio.receivers[receiver];
    rx.frequencyHz = band.frequencyHz;
    const bool modeChanged = rx.modulation != band.mode;
    rx.modulation = band.mode;
    emit radioChanged();
    if (modeChanged) emit modulationChanged(receiver, band.mode);
}

void UsbBackend::flush()
{
    if (m_cat) m_cat->flushPendingLevel();
}

void UsbBackend::close()
{
    if (m_cat) m_cat->close();
    m_wantOpen = false;
    m_retry.stop();
    m_drain.stop();
    m_source.reset();
    m_pending.clear();
    setState(Link::Disconnected);
}

void UsbBackend::requestAudio(int receiver, int chainRate)
{
    const int chosen = std::clamp(receiver, 0, std::max(0, int(m_model.receivers.size()) - 1));
    // A part block of the other receiver's audio must not go out as this one's.
    if (chosen != m_receiver) m_pending.clear();
    m_receiver = chosen;
    if (chainRate != m_chainRate) {
        m_chainRate = chainRate;
        m_resampler.reset();  // rebuilt for the new ratio when the next audio comes
        m_pending.clear();
    }
}

void UsbBackend::setInputTrimDb(double db)
{
    if (!std::isfinite(db)) return;
    m_trim = float(std::pow(10.0, std::clamp(db, kMinUsbTrimDb, kMaxUsbTrimDb) / 20.0));
}

RadioBackend::Input UsbBackend::input() const
{
    Input in;
    if (m_source) {
        const SourceStats stats = m_source->stats();
        in.device = m_source->deviceName();
        in.sampleRate = stats.deviceRate;
        in.overflows = stats.overflows;
    }
    return in;
}

void UsbBackend::releaseAudioDevices()
{
    if (!m_source) return;
    m_drain.stop();
    m_source.reset();
}

void UsbBackend::reacquireAudioDevices()
{
    if (m_wantOpen && !m_source) tryOpen();
}

void UsbBackend::setState(Link state)
{
    if (state == m_state) return;
    m_state = state;
    emit stateChanged(state);
}

void UsbBackend::tryOpen()
{
    if (!m_wantOpen || m_source) return;
    QString error;
    m_source = m_sourceFactory(m_inputKey, m_chainRate, &error);
    if (!m_source) {
        if (error != m_lastError) {
            m_lastError = error;
            qCInfo(lcUsb) << "Radio audio input:" << error;
        }
        setState(Link::Connecting);
        m_retry.start();
        return;
    }
    m_retry.stop();
    m_lastError.clear();
    m_resampler.reset();
    m_pending.clear();
    m_lastCallbacks = m_source->stats().callbacks;
    m_lastProgressMs = m_clock.elapsed();
    m_drain.start();
    setState(Link::Ready);
}

void UsbBackend::lose(const QString& why)
{
    qCWarning(lcUsb) << "Radio audio input lost:" << why;
    m_drain.stop();
    m_source.reset();
    m_pending.clear();
    m_lastError = why;
    if (!m_wantOpen) return;
    setState(Link::Connecting);
    m_retry.start();
}

void UsbBackend::drain()
{
    if (!m_source) return;
    const qint64 now = m_clock.elapsed();
    const quint64 callbacks = m_source->stats().callbacks;
    if (callbacks != m_lastCallbacks) {
        m_lastCallbacks = callbacks;
        m_lastProgressMs = now;
    }
    if (!m_source->alive()) {
        lose(QStringLiteral("%1 stopped").arg(m_source->deviceName()));
        return;
    }
    if (now - m_lastProgressMs > kStalledMs) {
        lose(QStringLiteral("%1 stopped delivering audio (unplugged?)").arg(m_source->deviceName()));
        return;
    }

    const size_t available = m_source->available();
    if (available == 0) return;
    m_captured.resize(available * 2);
    const size_t frames = m_source->read(m_captured.data(), available);
    const int channel = m_model.channels.value(m_receiver, 0);
    m_mono.resize(frames);
    for (size_t i = 0; i < frames; ++i) m_mono[i] = m_captured[i * 2 + size_t(channel)] * m_trim;

    if (m_source->sampleRate() == m_chainRate) {
        deliver(m_mono.data(), frames);
        return;
    }
    if (!m_resampler) {
        m_resampler = std::make_unique<AetherSDR::Resampler>(m_source->sampleRate(), m_chainRate);
    }
    // In pieces no larger than the converter was sized for.
    for (size_t done = 0; done < frames;) {
        const int chunk = int(std::min<size_t>(frames - done, 4096));
        const int converted = m_resampler->process(m_mono.data() + done, chunk, m_converted);
        deliver(reinterpret_cast<const float*>(m_converted.constData()), size_t(converted));
        done += size_t(chunk);
    }
}

void UsbBackend::deliver(const float* mono, size_t frames)
{
    m_pending.insert(m_pending.end(), mono, mono + frames);
    const size_t block = size_t(blockFrames(m_chainRate));
    size_t offset = 0;
    while (m_pending.size() - offset >= block) {
        m_block.resize(block * 2);
        for (size_t i = 0; i < block; ++i) {
            const float sample = m_pending[offset + i];
            m_block[i * 2] = sample;
            m_block[i * 2 + 1] = sample;
        }
        offset += block;
        emit audioBlock(m_receiver, m_block.data(), int(block), m_chainRate);
        if (!m_source) return;  // a handler closed us
    }
    m_pending.erase(m_pending.begin(), m_pending.begin() + std::ptrdiff_t(offset));
}

} // namespace esa
