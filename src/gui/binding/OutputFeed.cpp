#include "gui/binding/OutputFeed.h"

#include "app/LiveTaps.h"
#include "audio/OutputShape.h"

#include "core/AudioEngine.h"

#include <QByteArray>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <utility>

namespace esa {

OutputFeed::OutputFeed(std::shared_ptr<LiveTaps> taps, AetherSDR::AudioEngine* engine, std::function<Shape()> shape,
                       QObject* parent)
    : QObject(parent), m_taps(std::move(taps)), m_engine(engine), m_shape(std::move(shape)), m_timer(new QTimer(this))
{
    if (m_taps) m_stereo.resize(m_taps->outTap.capacity() * 2);
    m_timer->setInterval(kTickMs);
    connect(m_timer, &QTimer::timeout, this, &OutputFeed::tick);
}

OutputFeed::~OutputFeed()
{
    stop();
}

void OutputFeed::start()
{
    if (!m_taps || isRunning()) return;
    // What queued before now is old news; the engine adds to it only while it is wanted.
    m_taps->outTap.skip(m_taps->outTap.size());
    m_taps->outWanted.store(true, std::memory_order_relaxed);

    m_clock.restart();
    const quint64 blocks = m_taps->outBlocks.load(std::memory_order_relaxed);
    // Something reached the output while the page was away, or was reaching it when the page
    // went: taken as flowing, until it is seen to have stopped. Otherwise still silent.
    const bool flowing = blocks != m_lastBlocks || !m_silent;
    m_lastBlocks = blocks;
    m_lastAdvanceMs = flowing ? 0 : -(kStaleMs + 1);
    // The scope shows "Enable PC Audio" once its last samples are a second old, which they
    // are after a spell off screen; a piece of silence now keeps that from flashing up
    // before the first tick.
    const int rate = std::max(1, m_taps->chainRate.load(std::memory_order_relaxed));
    const size_t samples = size_t(rate) * kSilenceEveryMs / 1000;
    if (m_mono.size() < samples) m_mono.resize(samples);
    std::fill_n(m_mono.begin(), samples, 0.0f);
    publish(samples, rate);
    m_lastSilenceMs = 0;
    m_timer->start();
}

void OutputFeed::stop()
{
    m_timer->stop();
    if (m_taps) m_taps->outWanted.store(false, std::memory_order_relaxed);
}

bool OutputFeed::isRunning() const
{
    return m_timer->isActive();
}

void OutputFeed::tick()
{
    if (!m_taps) return;
    const qint64 now = m_clock.isValid() ? m_clock.elapsed() : 0;
    const quint64 blocks = m_taps->outBlocks.load(std::memory_order_relaxed);
    if (blocks != m_lastBlocks) {
        m_lastBlocks = blocks;
        m_lastAdvanceMs = now;
    }
    const int rate = std::max(1, m_taps->chainRate.load(std::memory_order_relaxed));

    const size_t frames = m_taps->outTap.read(m_stereo.data(), m_stereo.size() / 2);
    if (frames > 0) {
        if (m_mono.size() < frames) m_mono.resize(frames);
        toMono(m_stereo.data(), frames, m_shape ? m_shape() : Shape(), m_mono.data());
        publish(frames, rate);
    }

    const bool stale = now - m_lastAdvanceMs > kStaleMs;
    if (stale && frames == 0 && now - m_lastSilenceMs >= kSilenceEveryMs) {
        const size_t samples = size_t(rate) * kSilenceEveryMs / 1000;
        if (m_mono.size() < samples) m_mono.resize(samples);
        std::fill_n(m_mono.begin(), samples, 0.0f);
        publish(samples, rate);
        m_lastSilenceMs = now;
    }
    setSilent(stale);
}

void OutputFeed::toMono(const float* stereo, size_t frames, const Shape& shape, float* mono)
{
    // Before MUTE, as upstream's meter reads (design 6.5).
    const float gain = outputGain(shape.volumeDb);
    for (size_t i = 0; i < frames; ++i) {
        const float left = shapeOutput(stereo[2 * i], gain, shape.boost);
        const float right = shapeOutput(stereo[2 * i + 1], gain, shape.boost);
        mono[i] = std::clamp(0.5f * (left + right), -1.0f, 1.0f);
    }
}

void OutputFeed::publish(size_t samples, int rate)
{
    if (!m_engine || samples == 0) return;
    const QByteArray pcm(reinterpret_cast<const char*>(m_mono.data()), qsizetype(samples * sizeof(float)));
    // The meter listens to the first, the scope to the second, as upstream's do.
    m_engine->publishRxScope(pcm, rate);
    m_engine->publishRxPostChainScope(pcm, rate);
}

void OutputFeed::setSilent(bool silent)
{
    if (silent == m_silent) return;
    m_silent = silent;
    emit silenceChanged(silent);
}

} // namespace esa
