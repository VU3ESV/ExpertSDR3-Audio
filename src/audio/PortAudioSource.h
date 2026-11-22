#pragma once

#include "audio/AudioSink.h"
#include "audio/AudioSource.h"
#include "audio/SpscRing.h"

#include <QList>
#include <QString>

#include <atomic>
#include <memory>
#include <vector>

using PaStream = void;

namespace esa {

namespace audio {
// Every input-capable device PortAudio can see. Rescans the system, so it must not be called
// while a PortAudioSink or PortAudioSource is open.
QList<AudioDeviceInfo> listInputDevices();

// Why no real input may open here, or empty if one may. Under the test sandbox none may, so
// no test can open a microphone or a radio's codec, or set off the macOS permission prompt.
QString realInputRefusal();
} // namespace audio

// Captures from a PortAudio input device, in stereo, at the device's rate. Only the device
// asked for: an unknown key fails rather than fall back to the default input, which would be
// this computer's microphone taken for the radio.
class PortAudioSource final : public AudioSource {
public:
    // Null with `*error` set on failure. Prefers `preferredRate`, else the device's own rate.
    static std::unique_ptr<PortAudioSource> open(const QString& deviceKey, int preferredRate, QString* error);
    ~PortAudioSource() override;

    QString deviceName() const override { return m_deviceName; }
    int sampleRate() const override { return m_rate; }
    size_t read(float* stereo, size_t frames) override { return m_ring.read(stereo, frames); }
    size_t available() const override { return m_ring.size(); }
    bool alive() const override;
    SourceStats stats() const override;

    // Public only for PortAudio's C callback: the device's real-time thread.
    void capture(const float* input, unsigned long frames, bool overflowed);

private:
    PortAudioSource(int rate, int channels);

    QString m_deviceName;
    int m_rate;
    int m_channels;
    PaStream* m_stream{nullptr};
    SpscStereoRing m_ring;
    std::vector<float> m_monoScratch;  // a mono device's frames, as stereo; sized at open
    std::atomic<quint64> m_callbacks{0};
    std::atomic<quint64> m_frames{0};
    std::atomic<quint64> m_overflows{0};
};

} // namespace esa
