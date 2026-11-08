#include "audio/PortAudioSink.h"

#include "app/TestSandbox.h"

#include <portaudio.h>

#include <QLoggingCategory>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(lcSink, "esa.audio")

namespace esa {

namespace {

// PortAudio's library state is process-wide and reference counted, and it snapshots
// the device list when first initialised, so a rescan needs every holder to let go.
class PaLibrary {
public:
    PaLibrary() : m_error(Pa_Initialize()) {}
    ~PaLibrary()
    {
        if (m_error == paNoError) Pa_Terminate();
    }
    PaLibrary(const PaLibrary&) = delete;
    PaLibrary& operator=(const PaLibrary&) = delete;
    bool ok() const { return m_error == paNoError; }
    QString errorText() const { return QString::fromUtf8(Pa_GetErrorText(m_error)); }

private:
    PaError m_error;
};

QString deviceKeyFor(const PaDeviceInfo* info)
{
    const PaHostApiInfo* host = Pa_GetHostApiInfo(info->hostApi);
    return QStringLiteral("%1|%2").arg(host ? QString::fromUtf8(host->name) : QString(),
                                       QString::fromUtf8(info->name));
}

PaDeviceIndex findDevice(const QString& key)
{
    if (key.isEmpty()) return Pa_GetDefaultOutputDevice();
    const PaDeviceIndex count = Pa_GetDeviceCount();
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info && info->maxOutputChannels > 0 && deviceKeyFor(info) == key) return i;
    }
    qCWarning(lcSink) << "Device not found, using the default:" << key;
    return Pa_GetDefaultOutputDevice();
}

int paCallback(const void*, void* output, unsigned long frames, const PaStreamCallbackTimeInfo*,
               PaStreamCallbackFlags, void* user)
{
    static_cast<PortAudioSink*>(user)->render(static_cast<float*>(output), frames);
    return paContinue;
}

} // namespace

namespace audio {

QList<AudioDeviceInfo> listOutputDevices()
{
    QList<AudioDeviceInfo> devices;
    PaLibrary library;
    if (!library.ok()) {
        qCWarning(lcSink) << "PortAudio init failed:" << library.errorText();
        return devices;
    }
    const PaDeviceIndex defaultDevice = Pa_GetDefaultOutputDevice();
    const PaDeviceIndex count = Pa_GetDeviceCount();
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0) continue;
        const PaHostApiInfo* host = Pa_GetHostApiInfo(info->hostApi);
        AudioDeviceInfo device;
        device.key = deviceKeyFor(info);
        device.name = QString::fromUtf8(info->name);
        device.hostApi = host ? QString::fromUtf8(host->name) : QString();
        device.maxOutputChannels = info->maxOutputChannels;
        device.defaultSampleRate = info->defaultSampleRate;
        device.isDefault = i == defaultDevice;
        devices.append(device);
    }
    return devices;
}

QString realOutputRefusal()
{
    if (!testSandbox()) return QString();
    return QStringLiteral("Real audio outputs are off in tests (ESA_TEST_SANDBOX): "
                          "use the null device or a test sink");
}

} // namespace audio

std::unique_ptr<PortAudioSink> PortAudioSink::open(const QString& deviceKey, int chainRate,
                                                   int targetLatencyMs, int forcedDeviceRate, QString* error)
{
    const auto fail = [error](const QString& message) -> std::unique_ptr<PortAudioSink> {
        if (error) *error = message;
        return nullptr;
    };

    if (const QString refusal = audio::realOutputRefusal(); !refusal.isEmpty()) {
        qCWarning(lcSink) << "Test sandbox: refused to open a real output" << deviceKey;
        return fail(refusal);
    }

    const PaError initError = Pa_Initialize();
    if (initError != paNoError) {
        return fail(QStringLiteral("PortAudio: %1").arg(QString::fromUtf8(Pa_GetErrorText(initError))));
    }

    const PaDeviceIndex index = findDevice(deviceKey);
    const PaDeviceInfo* info = index == paNoDevice ? nullptr : Pa_GetDeviceInfo(index);
    if (!info) {
        Pa_Terminate();
        return fail(QStringLiteral("No audio output device is available"));
    }

    PaStreamParameters params{};
    params.device = index;
    params.channelCount = std::min(info->maxOutputChannels, 2);
    params.sampleFormat = paFloat32;
    params.suggestedLatency = info->defaultLowOutputLatency;

    // Prefer the chain's rate so nothing needs converting.
    int deviceRate = forcedDeviceRate > 0 ? forcedDeviceRate : chainRate;
    if (Pa_IsFormatSupported(nullptr, &params, deviceRate) != paFormatIsSupported) {
        deviceRate = int(info->defaultSampleRate);
        const PaError rateError = Pa_IsFormatSupported(nullptr, &params, deviceRate);
        if (rateError != paFormatIsSupported) {
            const QString name = QString::fromUtf8(info->name);
            Pa_Terminate();
            return fail(QStringLiteral("%1 rejected %2 and %3 Hz: %4")
                            .arg(name).arg(chainRate).arg(deviceRate)
                            .arg(QString::fromUtf8(Pa_GetErrorText(rateError))));
        }
    }

    std::unique_ptr<PortAudioSink> sink(new PortAudioSink(chainRate, deviceRate, params.channelCount));
    sink->m_deviceName = QString::fromUtf8(info->name);
    sink->m_buffer.setTargetLatencyMs(targetLatencyMs);

    PaError result = Pa_OpenStream(&sink->m_stream, nullptr, &params, deviceRate,
                                   paFramesPerBufferUnspecified, paClipOff, paCallback, sink.get());
    if (result == paNoError) result = Pa_StartStream(sink->m_stream);
    if (result != paNoError) {
        // The destructor closes any half-open stream and releases our Pa_Initialize.
        const QString message = QString::fromUtf8(Pa_GetErrorText(result));
        const QString name = sink->m_deviceName;
        sink.reset();
        return fail(QStringLiteral("%1: %2").arg(name, message));
    }
    qCInfo(lcSink) << "Output open:" << sink->m_deviceName << deviceRate << "Hz,"
                   << params.channelCount << "channel(s)";
    return sink;
}

PortAudioSink::PortAudioSink(int chainRate, int deviceRate, int deviceChannels)
    : m_buffer(chainRate, deviceRate, deviceChannels)
{
}

PortAudioSink::~PortAudioSink()
{
    if (m_stream) {
        Pa_StopStream(m_stream);
        Pa_CloseStream(m_stream);
    }
    Pa_Terminate();
}

bool PortAudioSink::alive() const
{
    return m_stream && Pa_IsStreamActive(m_stream) == 1;
}

SinkStats PortAudioSink::stats()
{
    SinkStats s = m_buffer.stats();
    if (m_stream) {
        if (const PaStreamInfo* info = Pa_GetStreamInfo(m_stream)) {
            s.reportedLatencyMs = info->outputLatency * 1000.0;
        }
    }
    return s;
}

} // namespace esa
