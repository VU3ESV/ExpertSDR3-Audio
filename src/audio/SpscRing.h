#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace esa {

// Lock-free single-producer single-consumer ring of interleaved stereo frames,
// carrying audio from the engine thread to the real-time device callback. Neither
// side blocks or allocates after construction. The consumer may also skip frames.
class SpscStereoRing {
public:
    explicit SpscStereoRing(size_t capacityFrames)
        : m_capacity(capacityFrames + 1), m_data(m_capacity * 2, 0.0f) {}

    size_t capacity() const { return m_capacity - 1; }

    size_t size() const
    {
        const size_t head = m_head.load(std::memory_order_acquire);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        return head >= tail ? head - tail : head + m_capacity - tail;
    }

    // Producer. Returns frames written; the excess is dropped when full.
    size_t write(const float* stereo, size_t frames)
    {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        const size_t used = head >= tail ? head - tail : head + m_capacity - tail;
        const size_t count = std::min(frames, m_capacity - 1 - used);
        const size_t first = std::min(count, m_capacity - head);
        std::copy_n(stereo, first * 2, m_data.data() + head * 2);
        std::copy_n(stereo + first * 2, (count - first) * 2, m_data.data());
        m_head.store((head + count) % m_capacity, std::memory_order_release);
        return count;
    }

    // Consumer. Returns frames read.
    size_t read(float* stereo, size_t frames)
    {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t head = m_head.load(std::memory_order_acquire);
        const size_t avail = head >= tail ? head - tail : head + m_capacity - tail;
        const size_t count = std::min(frames, avail);
        const size_t first = std::min(count, m_capacity - tail);
        std::copy_n(m_data.data() + tail * 2, first * 2, stereo);
        std::copy_n(m_data.data(), (count - first) * 2, stereo + first * 2);
        m_tail.store((tail + count) % m_capacity, std::memory_order_release);
        return count;
    }

    // Consumer. Discards the oldest frames.
    size_t skip(size_t frames)
    {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t head = m_head.load(std::memory_order_acquire);
        const size_t avail = head >= tail ? head - tail : head + m_capacity - tail;
        const size_t count = std::min(frames, avail);
        m_tail.store((tail + count) % m_capacity, std::memory_order_release);
        return count;
    }

private:
    const size_t m_capacity;
    std::vector<float> m_data;
    std::atomic<size_t> m_head{0};
    std::atomic<size_t> m_tail{0};
};

} // namespace esa
