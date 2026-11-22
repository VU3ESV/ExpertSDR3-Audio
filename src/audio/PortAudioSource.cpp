#include "audio/PortAudioSource.h"

#include "app/TestSandbox.h"
#include "audio/PortAudioCommon.h"

#include <QLoggingCategory>

#include <algorithm>

Q_LOGGING_CATEGORY(lcSource, "esa.audio.input")

namespace esa {

namespace {

using audio::pa::deviceKeyFor;

// Two seconds: enough for the engine thread's longest stall (a chain rebuild, a settings sync)
// without a gap.
constexpr int kRingSeconds = 2;
// The most frames a callback is expected to bring at once; a mono device's are widened in
// chunks of this.
constexpr size_t kScratchFrames = 4096;

PaDeviceIndex findInput(const QString& key)
{
    const PaDeviceIndex count = Pa_GetDeviceCount();
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info && info->maxInputChannels > 0 && deviceKeyFor(info) == key) return i;
    }
    return paNoDevice;
}

int paCallback(const void* input, void*, unsigned long frames, const PaStreamCallbackTimeInfo*,
               PaStreamCallbackFlags flags, void* user)
{
    static_cast<PortAudioSource*>(user)->capture(static_cast<const float*>(input), frames,
                                                 (flags & paInputOverflow) != 0);
    return paContinue;
}

} // namespace

namespace audio {

QList<AudioDeviceInfo> listInputDevices()
{
    QList<AudioDeviceInfo> devices;
    pa::Library library;
    if (!library.ok()) {
        qCWarning(lcSource) << "PortAudio init failed:" << library.errorText();
        return devices;
    }
    const PaDeviceIndex defaultDevice = Pa_GetDefaultInputDevice();
    const PaDeviceIndex count = Pa_GetDeviceCount();
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxInputChannels <= 0) continue;
        const PaHostApiInfo* host = Pa_GetHostApiInfo(info->hostApi);
        AudioDeviceInfo device;
        device.key = deviceKeyFor(info);
        device.name = QString::fromUtf8(info->name);
        device.hostApi = host ? QString::fromUtf8(host->name) : QString();
        device.maxOutputChannels = info->maxOutputChannels;
        device.maxInputChannels = info->maxInputChannels;
        device.defaultSampleRate = info->defaultSampleRate;
        device.isDefault = i == defaultDevice;
        devices.append(device);
    }
    return devices;
}

QString realInputRefusal()
{
    if (!testSandbox()) return QString();
    return QStringLiteral("Real audio inputs are off in tests (ESA_TEST_SANDBOX): use a test source");
}

} // namespace audio

std::unique_ptr<PortAudioSource> PortAudioSource::open(const QString& deviceKey, int preferredRate, QString* error)
{
    const auto fail = [error](const QString& message) -> std::unique_ptr<PortAudioSource> {
        if (error) *error = message;
        return nullptr;
    };

    if (const QString refusal = audio::realInputRefusal(); !refusal.isEmpty()) {
        qCWarning(lcSource) << "Test sandbox: refused to open a real input" << deviceKey;
        return fail(refusal);
    }
    if (deviceKey.isEmpty()) return fail(QStringLiteral("Choose the radio's audio input first"));

    const PaError initError = Pa_Initialize();
    if (initError != paNoError) {
        return fail(QStringLiteral("PortAudio: %1").arg(QString::fromUtf8(Pa_GetErrorText(initError))));
    }
    const PaDeviceIndex index = findInput(deviceKey);
    const PaDeviceInfo* info = index == paNoDevice ? nullptr : Pa_GetDeviceInfo(index);
    if (!info) {
        Pa_Terminate();
        return fail(QStringLiteral("%1 is not connected").arg(deviceKey.section(QLatin1Char('|'), 1)));
    }

    PaStreamParameters params{};
    params.device = index;
    params.channelCount = std::min(info->maxInputChannels, 2);
    params.sampleFormat = paFloat32;
    params.suggestedLatency = info->defaultLowInputLatency;

    int rate = preferredRate;
    if (Pa_IsFormatSupported(&params, nullptr, rate) != paFormatIsSupported) {
        rate = int(info->defaultSampleRate);
        const PaError rateError = Pa_IsFormatSupported(&params, nullptr, rate);
        if (rateError != paFormatIsSupported) {
            const QString name = QString::fromUtf8(info->name);
            Pa_Terminate();
            return fail(QStringLiteral("%1 rejected %2 and %3 Hz: %4")
                            .arg(name).arg(preferredRate).arg(rate)
                            .arg(QString::fromUtf8(Pa_GetErrorText(rateError))));
        }
    }

    std::unique_ptr<PortAudioSource> source(new PortAudioSource(rate, params.channelCount));
    source->m_deviceName = QString::fromUtf8(info->name);
    // At most 1024 frames a callback, so the engine sees audio at least every 21 ms at 48 kHz.
    PaError result = Pa_OpenStream(&source->m_stream, &params, nullptr, rate, 1024, paClipOff, paCallback,
                                   source.get());
    if (result == paNoError) result = Pa_StartStream(source->m_stream);
    if (result != paNoError) {
        // The destructor closes any half-open stream and releases our Pa_Initialize.
        const QString message = QString::fromUtf8(Pa_GetErrorText(result));
        const QString name = source->m_deviceName;
        source.reset();
        return fail(QStringLiteral("%1: %2").arg(name, message));
    }
    qCInfo(lcSource) << "Input open:" << source->m_deviceName << rate << "Hz," << params.channelCount
                     << "channel(s)";
    return source;
}

PortAudioSource::PortAudioSource(int rate, int channels)
    : m_rate(rate), m_channels(channels), m_ring(size_t(rate) * kRingSeconds),
      m_monoScratch(channels == 1 ? kScratchFrames * 2 : 0)
{
}

PortAudioSource::~PortAudioSource()
{
    if (m_stream) {
        Pa_StopStream(m_stream);
        Pa_CloseStream(m_stream);
    }
    Pa_Terminate();
}

bool PortAudioSource::alive() const
{
    return m_stream && Pa_IsStreamActive(m_stream) == 1;
}

SourceStats PortAudioSource::stats() const
{
    SourceStats s;
    s.deviceRate = m_rate;
    s.deviceChannels = m_channels;
    s.callbacks = m_callbacks.load(std::memory_order_relaxed);
    s.framesCaptured = m_frames.load(std::memory_order_relaxed);
    s.overflows = m_overflows.load(std::memory_order_relaxed);
    return s;
}

void PortAudioSource::capture(const float* input, unsigned long frames, bool overflowed)
{
    m_callbacks.fetch_add(1, std::memory_order_relaxed);
    if (overflowed) m_overflows.fetch_add(1, std::memory_order_relaxed);
    if (!input || frames == 0) return;
    size_t lost = 0;
    if (m_channels >= 2) {
        lost = frames - m_ring.write(input, frames);
    } else {
        // Nothing here may allocate: the mono frames are widened through scratch sized at open.
        for (size_t done = 0; done < frames;) {
            const size_t chunk = std::min<size_t>(frames - done, kScratchFrames);
            for (size_t i = 0; i < chunk; ++i) {
                m_monoScratch[i * 2] = input[done + i];
                m_monoScratch[i * 2 + 1] = input[done + i];
            }
            lost += chunk - m_ring.write(m_monoScratch.data(), chunk);
            done += chunk;
        }
    }
    m_frames.fetch_add(quint64(frames - lost), std::memory_order_relaxed);
    if (lost > 0) m_overflows.fetch_add(1, std::memory_order_relaxed);
}

} // namespace esa
