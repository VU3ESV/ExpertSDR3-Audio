#pragma once

#include "audio/AudioSink.h"
#include "audio/DriftController.h"
#include "audio/SpscRing.h"

#include <QByteArray>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace AetherSDR { class Resampler; }

namespace esa {

// Everything between the chain and the sound card that is not the sound card:
// rate conversion, the jitter buffer, clock-drift correction, volume, and recovery
// from underruns and overruns. It knows nothing about PortAudio, so tests can drive
// it directly with no audio hardware.
//
// One thread calls write(), another calls render(). render() is real-time safe.
class PlaybackBuffer {
public:
    PlaybackBuffer(int chainRate, int deviceRate, int deviceChannels);
    ~PlaybackBuffer();

    // Interleaved stereo float32 at the chain's rate.
    void write(const float* stereo, int frames);

    // Fills `frames` frames of `deviceChannels` channels. Always fills all of it,
    // with silence where there is nothing to play.
    void render(float* out, unsigned long frames);

    void setGainDb(double db);
    void setBoost(bool on) { m_boost.store(on, std::memory_order_relaxed); }
    void setMuted(bool muted) { m_muted.store(muted, std::memory_order_relaxed); }
    void setTargetLatencyMs(int ms);

    SinkStats stats();

    static constexpr int kMaxLatencyMs = 500;
    static constexpr int kUnderrunBumpMs = 10;      // how far the buffer grows after an underrun
    static constexpr double kOverrunSlackMs = 150;  // past target + this, jump back
    static constexpr int kFadeMs = 5;

private:
    void pullInterpolated(float* stereo, unsigned long frames, double ratio);

    const int m_deviceRate;
    const int m_deviceChannels;

    SpscStereoRing m_ring;
    DriftController m_drift;

    // Fixed-ratio conversion on the writer's thread, when the device rejects the
    // chain's rate. Null otherwise.
    std::array<std::unique_ptr<AetherSDR::Resampler>, 2> m_resamplers;
    std::array<std::vector<float>, 2> m_split;
    std::array<QByteArray, 2> m_converted;
    std::vector<float> m_interleaved;

    // render() state.
    std::array<float, 8> m_history{};  // four stereo frames for cubic interpolation
    double m_phase{0.0};
    bool m_prebuffering{true};
    bool m_starved{false};
    float m_fade{0.0f};
    float m_currentGain{1.0f};
    std::vector<float> m_scratch;

    // Shared between threads.
    std::atomic<int> m_targetFrames{0};
    std::atomic<float> m_targetGain{1.0f};
    std::atomic<bool> m_boost{false};
    std::atomic<bool> m_muted{false};
    std::atomic<quint64> m_callbacks{0};
    std::atomic<quint64> m_framesPlayed{0};
    std::atomic<quint64> m_underruns{0};
    std::atomic<quint64> m_overruns{0};
    std::atomic<float> m_peak{0.0f};
    std::atomic<double> m_driftPpm{0.0};
};

} // namespace esa
