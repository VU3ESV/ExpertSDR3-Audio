#pragma once

#include <QList>
#include <QString>

#include <atomic>
#include <memory>

namespace esa {

struct AudioDeviceInfo {
    QString key;      // stable across runs and reorderings: "<host api>|<name>"
    QString name;
    QString hostApi;
    int maxOutputChannels{0};
    double defaultSampleRate{0.0};
    bool isDefault{false};
};

struct SinkStats {
    int deviceRate{0};
    double bufferedMs{0.0};
    double targetMs{0.0};
    double driftPpm{0.0};
    double reportedLatencyMs{0.0};
    quint64 callbacks{0};
    quint64 framesPlayed{0};
    quint64 underruns{0};
    quint64 overruns{0};
    float peak{0.0f};  // output peak since the previous stats() call
};

// Where processed audio goes. Implementations own their threading: write() is
// called from the engine thread and must not block.
class AudioSink {
public:
    virtual ~AudioSink() = default;
    virtual QString deviceName() const = 0;
    // Interleaved stereo float32 at the chain's rate.
    virtual void write(const float* stereo, int frames) = 0;
    // False once the device has gone away (unplugged, stream error).
    virtual bool alive() const = 0;
    virtual SinkStats stats() = 0;
    virtual void setGainDb(double db) = 0;
    virtual void setBoost(bool on) = 0;
    virtual void setMuted(bool muted) = 0;
    virtual void setTargetLatencyMs(int ms) = 0;
};

// The device key that selects NullSink.
inline const QLatin1String kNullDeviceKey("null");

// Discards audio. Backs `--device null`, for running on a machine with no sound
// hardware, and the end-to-end tests.
class NullSink final : public AudioSink {
public:
    QString deviceName() const override { return QStringLiteral("(null output)"); }
    void write(const float*, int frames) override
    {
        m_frames.fetch_add(quint64(frames), std::memory_order_relaxed);
    }
    bool alive() const override { return m_alive.load(); }
    SinkStats stats() override
    {
        SinkStats s;
        s.framesPlayed = m_frames.load(std::memory_order_relaxed);
        return s;
    }
    void setGainDb(double) override {}
    void setBoost(bool) override {}
    void setMuted(bool) override {}
    void setTargetLatencyMs(int) override {}

    // Test hook: behave like an unplugged device.
    void kill() { m_alive.store(false); }

private:
    std::atomic<quint64> m_frames{0};
    std::atomic<bool> m_alive{true};
};

} // namespace esa
