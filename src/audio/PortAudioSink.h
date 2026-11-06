#pragma once

#include "audio/AudioSink.h"
#include "audio/PlaybackBuffer.h"

#include <QList>
#include <QString>

#include <memory>

using PaStream = void;

namespace esa {

namespace audio {
// Every output-capable device PortAudio can see. Rescans the system, so it must
// not be called while a PortAudioSink is open.
QList<AudioDeviceInfo> listOutputDevices();
} // namespace audio

// Plays through a PortAudio device. Keeps the chain's rate if the device accepts it.
class PortAudioSink final : public AudioSink {
public:
    // Null with `*error` set on failure. An empty or unknown key uses the system
    // default device. A non-zero `forcedDeviceRate` overrides rate negotiation, for
    // hardware that claims to support rates it handles badly.
    static std::unique_ptr<PortAudioSink> open(const QString& deviceKey, int chainRate,
                                               int targetLatencyMs, int forcedDeviceRate, QString* error);
    ~PortAudioSink() override;

    QString deviceName() const override { return m_deviceName; }
    void write(const float* stereo, int frames) override { m_buffer.write(stereo, frames); }
    bool alive() const override;
    SinkStats stats() override;
    void setGainDb(double db) override { m_buffer.setGainDb(db); }
    void setBoost(bool on) override { m_buffer.setBoost(on); }
    void setMuted(bool muted) override { m_buffer.setMuted(muted); }
    void setTargetLatencyMs(int ms) override { m_buffer.setTargetLatencyMs(ms); }

    // Public only for PortAudio's C callback.
    void render(float* out, unsigned long frames) { m_buffer.render(out, frames); }

private:
    PortAudioSink(int chainRate, int deviceRate, int deviceChannels);

    PlaybackBuffer m_buffer;
    QString m_deviceName;
    PaStream* m_stream{nullptr};
};

} // namespace esa
