#pragma once

#include <algorithm>
#include <cmath>

namespace esa {

// The radio's clock and the sound card's clock are never quite the same, so left
// alone the buffer between them slowly empties or overflows and glitches every
// few minutes. This steers playback by a fraction of a percent, far too little to
// hear, to hold the buffer at its target.
//
// A PI controller on the buffer level. The level is low-pass filtered first
// because TCI delivers audio in bursts, which makes the raw value a sawtooth.
class DriftController {
public:
    static constexpr double kMaxCorrection = 0.002;  // +/-2000 ppm: real clock error is far below this

    DriftController(double sampleRate, double targetFrames)
        : m_sampleRate(sampleRate), m_target(targetFrames), m_filtered(targetFrames) {}

    void setTarget(double frames) { m_target = frames; }
    double target() const { return m_target; }

    // After a discontinuity (restart, underrun, or a skip back to target), so the
    // filter and integrator do not act on a level that no longer applies. The
    // integrator holds the learned clock offset and is kept unless told otherwise.
    void reset(double fillFrames, bool keepClockEstimate = true)
    {
        m_filtered = fillFrames;
        if (!keepClockEstimate) m_integral = 0.0;
    }

    // Call once per device callback. Returns source frames to consume per output
    // frame, 1.0 meaning no correction.
    double update(double fillFrames, int framesElapsed)
    {
        const double dt = framesElapsed / m_sampleRate;
        m_filtered += (fillFrames - m_filtered) * (1.0 - std::exp(-dt / kFilterSeconds));

        const double errorSeconds = (m_filtered - m_target) / m_sampleRate;
        m_integral = std::clamp(m_integral + kIntegralGain * errorSeconds * dt,
                                -kMaxCorrection, kMaxCorrection);
        m_correction = std::clamp(kProportionalGain * errorSeconds + m_integral,
                                  -kMaxCorrection, kMaxCorrection);
        return 1.0 + m_correction;
    }

    double correctionPpm() const { return m_correction * 1e6; }
    double filteredFill() const { return m_filtered; }

private:
    static constexpr double kFilterSeconds = 1.0;
    static constexpr double kProportionalGain = 0.03;  // 10 ms of error steers by 300 ppm
    static constexpr double kIntegralGain = 0.001;

    double m_sampleRate;
    double m_target;
    double m_filtered;
    double m_integral{0.0};
    double m_correction{0.0};
};

} // namespace esa
