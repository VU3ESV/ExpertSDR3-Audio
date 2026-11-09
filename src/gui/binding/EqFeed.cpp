#include "gui/binding/EqFeed.h"

#include "app/LiveTaps.h"

#include <QEvent>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <utility>

namespace esa {

namespace {

// Stereo frames taken from the tap per read: a block or so, so a tick drains in a few reads.
constexpr int kChunkFrames = 1024;

} // namespace

EqFeed::EqFeed(std::shared_ptr<LiveTaps> taps, QObject* parent)
    : QObject(parent), m_taps(std::move(taps)), m_history(size_t(kHistory), 0.0f),
      m_scratch(size_t(2 * kChunkFrames), 0.0f)
{
}

EqFeed::~EqFeed()
{
    stop();
}

void EqFeed::follow(QWidget* widget)
{
    widget->installEventFilter(this);
    if (widget->isVisible()) start();
}

bool EqFeed::eventFilter(QObject* watched, QEvent* event)
{
    // Ahead of the widget's own show and hide, which start and stop the panel's analyser.
    if (event->type() == QEvent::Show) {
        start();
    } else if (event->type() == QEvent::Hide) {
        stop();
    }
    return QObject::eventFilter(watched, event);
}

void EqFeed::start()
{
    if (!m_taps) return;
    // Only this side reads the tap, so it may drop what is queued: blocks from the last time
    // the page showed, or written while the engine had not yet seen the flag go down.
    m_taps->eqTap.skip(m_taps->eqTap.size());
    clearHistory();
    m_lastHeard.invalidate();
    m_running = true;
    m_taps->eqWanted.store(true, std::memory_order_relaxed);
}

void EqFeed::stop()
{
    if (m_taps) m_taps->eqWanted.store(false, std::memory_order_relaxed);
    m_running = false;
    m_lastHeard.invalidate();
    clearHistory();
}

void EqFeed::clearHistory()
{
    std::fill(m_history.begin(), m_history.end(), 0.0f);
    m_write = 0;
}

void EqFeed::drain()
{
    // At most what the tap holds, so an engine writing as fast as this reads cannot keep the
    // window here.
    size_t budget = m_taps->eqTap.capacity();
    while (budget > 0) {
        const size_t want = std::min(budget, size_t(kChunkFrames));
        const size_t got = m_taps->eqTap.read(m_scratch.data(), want);
        if (got == 0) break;
        for (size_t i = 0; i < got; ++i) {
            m_history[size_t(m_write)] = 0.5f * (m_scratch[2 * i] + m_scratch[2 * i + 1]);
            m_write = (m_write + 1) % kHistory;
        }
        m_lastHeard.start();
        budget -= got;
        if (got < want) break;
    }
}

bool EqFeed::copyRecent(float* out, int count)
{
    if (!m_running || !m_taps || !out || count <= 0) return false;

    // The panel places the analyser's bins by the rate of the EQ it draws, the mirror's, which
    // nothing processes: it follows the chain's, so a tone shows at its own frequency at any
    // rate. What was kept at another rate no longer fits.
    const int rate = m_taps->chainRate.load(std::memory_order_relaxed);
    if (rate > 0 && rate != m_rate) {
        m_taps->view.eq.prepare(rate);
        m_rate = rate;
        clearHistory();
    }

    drain();
    // Silence until the first block, as upstream's tap reads before any audio: the panel then
    // draws its analyser resting on the floor rather than not at all.
    if (!m_lastHeard.isValid() || m_lastHeard.elapsed() > kStaleMs) clearHistory();

    const int kept = std::min(count, kHistory);
    const int lead = count - kept;
    std::fill(out, out + lead, 0.0f);
    int from = (m_write - kept + kHistory) % kHistory;
    for (int i = 0; i < kept; ++i) {
        out[lead + i] = m_history[size_t(from)];
        from = (from + 1) % kHistory;
    }
    return true;
}

} // namespace esa
