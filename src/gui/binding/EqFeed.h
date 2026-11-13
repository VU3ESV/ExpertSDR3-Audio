#pragma once

#include <QElapsedTimer>
#include <QObject>

#include <memory>
#include <vector>

class QWidget;

namespace esa {

struct LiveTaps;

// The EQ analyser's samples: what came out of the EQ stage, from the engine's post-EQ tap
// (LiveTaps::eqTap), for upstream's panel, which asks for the newest 2048 at every tick of
// its own timer through the stand-in engine's copyRecentClientEqRxSamples().
//
// It runs only while the page shows: the engine writes the tap only while eqWanted is set,
// and the panel only asks while it is visible, so a hidden page costs the engine one load
// of a flag per block and the window nothing. The engine thread only ever writes the ring;
// everything here is the window's, on its own thread, and nothing waits on either side.
//
// GUI thread only.
class EqFeed : public QObject {
    Q_OBJECT

public:
    // Mono samples kept for the analyser: twice the 2048 it asks for, so a tick that finds
    // a block or two fewer than usual still has a full window.
    static constexpr int kHistory = 4096;
    // With no samples for this long the history is cleared, so the trace falls away rather
    // than freezing on the last block: the stream stopped, or the EQ is passed by (BYPASS,
    // transmit), where blocks keep coming but nothing goes through it.
    static constexpr int kStaleMs = 200;

    explicit EqFeed(std::shared_ptr<LiveTaps> taps, QObject* parent = nullptr);
    ~EqFeed() override;

    // Runs while `widget` is visible, from its show and hide events on.
    void follow(QWidget* widget);

    // Asks the engine for samples, dropping what waited in the tap from before, so the
    // analyser starts from what is playing now.
    void start();
    // The engine stops writing; what was kept is let go.
    void stop();
    bool isRunning() const { return m_running; }

    // Takes in what the engine has written since the last call, as one channel ((L + R) / 2),
    // and copies out the newest `count` samples, oldest first, with zeros ahead of them when
    // fewer are kept: all zeros before the first block and once the samples go stale. False
    // only when stopped.
    bool copyRecent(float* out, int count);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void drain();
    void clearHistory();

    std::shared_ptr<LiveTaps> m_taps;
    bool m_running{false};
    int m_rate{0};                   // the chain's rate the history was taken at
    std::vector<float> m_history;    // a ring of kHistory mono samples
    int m_write{0};                  // where the next sample goes in it
    std::vector<float> m_scratch;    // stereo frames read from the tap, allocated once
    QElapsedTimer m_lastHeard;       // since the last samples came; invalid before the first
};

} // namespace esa
