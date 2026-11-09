#include "audio/PlaybackBuffer.h"

#include "audio/OutputShape.h"

#include "Resampler.h"

#include <algorithm>
#include <cmath>

namespace esa {

namespace {

// Catmull-Rom through four points; x in [0, 1) between p1 and p2. At a ratio this
// close to 1 it is transparent and needs no filter design.
inline float cubic(float p0, float p1, float p2, float p3, float x)
{
    const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
    const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    const float c = -0.5f * p0 + 0.5f * p2;
    return ((a * x + b) * x + c) * x + p1;
}

} // namespace

PlaybackBuffer::PlaybackBuffer(int chainRate, int deviceRate, int deviceChannels)
    : m_deviceRate(deviceRate),
      m_deviceChannels(deviceChannels),
      m_ring(size_t(deviceRate)),  // one second, comfortably above kMaxLatencyMs
      m_drift(deviceRate, deviceRate * 0.06),
      m_scratch(size_t(deviceRate) * 2, 0.0f)
{
    setTargetLatencyMs(60);
    if (chainRate != deviceRate) {
        for (auto& resampler : m_resamplers) {
            resampler = std::make_unique<AetherSDR::Resampler>(chainRate, deviceRate);
        }
    }
}

PlaybackBuffer::~PlaybackBuffer() = default;

void PlaybackBuffer::setGainDb(double db)
{
    // A NaN target would turn the gain ramp NaN for as long as this output lives, and NaN
    // passes the clamp to full scale on its way to the device.
    if (!std::isfinite(db)) return;
    m_targetGain.store(outputGain(db), std::memory_order_relaxed);
}

void PlaybackBuffer::setTargetLatencyMs(int ms)
{
    const int clamped = std::clamp(ms, 5, kMaxLatencyMs);
    m_targetFrames.store(m_deviceRate * clamped / 1000, std::memory_order_relaxed);
}

SinkStats PlaybackBuffer::stats()
{
    SinkStats s;
    s.deviceRate = m_deviceRate;
    s.bufferedMs = 1000.0 * double(m_ring.size()) / m_deviceRate;
    s.targetMs = 1000.0 * m_targetFrames.load(std::memory_order_relaxed) / m_deviceRate;
    s.driftPpm = m_driftPpm.load(std::memory_order_relaxed);
    s.callbacks = m_callbacks.load(std::memory_order_relaxed);
    s.framesPlayed = m_framesPlayed.load(std::memory_order_relaxed);
    s.underruns = m_underruns.load(std::memory_order_relaxed);
    s.overruns = m_overruns.load(std::memory_order_relaxed);
    s.peak = m_peak.exchange(0.0f, std::memory_order_relaxed);
    return s;
}

void PlaybackBuffer::write(const float* stereo, int frames)
{
    if (frames <= 0) return;

    if (!m_resamplers[0]) {
        m_ring.write(stereo, size_t(frames));
        return;
    }

    for (size_t channel = 0; channel < 2; ++channel) {
        m_split[channel].resize(size_t(frames));
        for (int i = 0; i < frames; ++i) m_split[channel][size_t(i)] = stereo[i * 2 + int(channel)];
        m_resamplers[channel]->process(m_split[channel].data(), frames, m_converted[channel]);
    }
    const size_t converted = size_t(std::min(m_converted[0].size(), m_converted[1].size())) / sizeof(float);
    m_interleaved.resize(converted * 2);
    const auto* left = reinterpret_cast<const float*>(m_converted[0].constData());
    const auto* right = reinterpret_cast<const float*>(m_converted[1].constData());
    for (size_t i = 0; i < converted; ++i) {
        m_interleaved[i * 2] = left[i];
        m_interleaved[i * 2 + 1] = right[i];
    }
    m_ring.write(m_interleaved.data(), converted);
}

void PlaybackBuffer::pullInterpolated(float* stereo, unsigned long frames, double ratio)
{
    for (unsigned long i = 0; i < frames; ++i) {
        while (m_phase >= 1.0) {
            float next[2];
            if (m_ring.read(next, 1) != 1) {
                // Starved. Silence the rest of this buffer and rebuild the cushion
                // before resuming, rather than chattering on and off.
                std::fill(stereo + i * 2, stereo + frames * 2, 0.0f);
                m_starved = true;
                return;
            }
            std::copy(m_history.begin() + 2, m_history.end(), m_history.begin());
            m_history[6] = next[0];
            m_history[7] = next[1];
            m_phase -= 1.0;
        }
        const float x = float(m_phase);
        stereo[i * 2] = cubic(m_history[0], m_history[2], m_history[4], m_history[6], x);
        stereo[i * 2 + 1] = cubic(m_history[1], m_history[3], m_history[5], m_history[7], x);
        m_phase += ratio;
    }
}

void PlaybackBuffer::render(float* out, unsigned long frames)
{
    m_callbacks.fetch_add(1, std::memory_order_relaxed);

    const auto silence = [&] { std::fill(out, out + frames * unsigned(m_deviceChannels), 0.0f); };
    if (frames > m_scratch.size() / 2) {  // a device buffer larger than a second
        silence();
        return;
    }

    const int target = m_targetFrames.load(std::memory_order_relaxed);
    m_drift.setTarget(target);
    size_t fill = m_ring.size();

    if (m_prebuffering) {
        if (fill < size_t(target)) {
            silence();
            return;
        }
        m_prebuffering = false;
        m_fade = 0.0f;
        m_drift.reset(double(fill));
    }

    // Far behind (a stall, then a flood): jump back rather than stay late forever.
    const auto overrunSlack = size_t(m_deviceRate * kOverrunSlackMs / 1000.0);
    if (fill > size_t(target) + overrunSlack) {
        m_ring.skip(fill - size_t(target));
        m_overruns.fetch_add(1, std::memory_order_relaxed);
        fill = m_ring.size();
        m_drift.reset(double(fill));
    }

    const double ratio = m_drift.update(double(fill), int(frames));
    m_driftPpm.store(m_drift.correctionPpm(), std::memory_order_relaxed);

    float* stereo = m_scratch.data();
    m_starved = false;
    pullInterpolated(stereo, frames, ratio);
    if (m_starved) {
        m_prebuffering = true;
        m_underruns.fetch_add(1, std::memory_order_relaxed);
        // The target was too small for this link: grow it so the next one is less
        // likely. Capped, and reset whenever the operator moves the buffer control.
        const int bumped = target + m_deviceRate * kUnderrunBumpMs / 1000;
        m_targetFrames.store(std::min(bumped, m_deviceRate * kMaxLatencyMs / 1000),
                             std::memory_order_relaxed);
    }

    // Gain is ramped across the buffer to avoid zipper noise on slider moves. The
    // fade softens the edge when playback resumes after a starve.
    const bool muted = m_muted.load(std::memory_order_relaxed);
    const float gainTarget = muted ? 0.0f : m_targetGain.load(std::memory_order_relaxed);
    const float gainStep = (gainTarget - m_currentGain) / float(frames);
    const float fadeStep = 1.0f / (float(m_deviceRate) * float(kFadeMs) / 1000.0f);
    const bool boost = m_boost.load(std::memory_order_relaxed);

    float peak = m_peak.load(std::memory_order_relaxed);
    for (unsigned long i = 0; i < frames; ++i) {
        m_currentGain += gainStep;
        m_fade = std::min(m_fade + fadeStep, 1.0f);
        const float gain = m_currentGain * m_fade;
        const float left = shapeOutput(stereo[i * 2], gain, boost);
        const float right = shapeOutput(stereo[i * 2 + 1], gain, boost);
        peak = std::max({peak, std::fabs(left), std::fabs(right)});
        if (m_deviceChannels == 1) {
            out[i] = 0.5f * (left + right);
        } else {
            out[i * 2] = left;
            out[i * 2 + 1] = right;
        }
    }
    m_peak.store(peak, std::memory_order_relaxed);
    m_framesPlayed.fetch_add(frames, std::memory_order_relaxed);
}

} // namespace esa
