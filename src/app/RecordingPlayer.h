#pragma once

#include "app/WavFile.h"
#include "audio/SpscRing.h"

#include <QFile>
#include <QString>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

namespace esa {

// One PLAY of a finished recording, shared by the recorder thread, which fills
// the ring from the file, and the engine thread, which empties it at the pace of the output.
// Made by Recorder::play(). The engine never touches the file.
//
// The engine's pump, every 10 ms on a Qt::PreciseTimer:
//   if (s->failed())   stop, show s->failureReason();
//   if (!s->ready())   wait (the file is still being opened);
//   n = pacer.take(now, s->ring().size()); s->ring().read(buf, n); sink->write(buf, n);
//   if (s->atEnd() && s->ring().size() == 0)   playback is over.
class PlaySession {
public:
    PlaySession(quint64 id, QString path, size_t ringFrames)
        : m_id(id), m_path(std::move(path)), m_ring(ringFrames)
    {
    }

    quint64 id() const { return m_id; }
    QString path() const { return m_path; }

    // Engine thread reads; the recorder thread writes.
    SpscStereoRing& ring() { return m_ring; }

    // The header was read and the first frames are in the ring. sampleRate() and totalFrames()
    // are valid from then on.
    bool ready() const { return m_ready.load(std::memory_order_acquire); }
    int sampleRate() const { return m_sampleRate; }
    quint64 totalFrames() const { return m_totalFrames; }
    // Frames put in the ring so far.
    quint64 framesQueued() const { return m_queued.load(std::memory_order_acquire); }
    // Every frame of the file is in the ring: once it drains, playback is over.
    bool atEnd() const { return m_atEnd.load(std::memory_order_acquire); }

    bool failed() const { return m_failed.load(std::memory_order_acquire); }
    // Valid once failed() is true.
    QString failureReason() const { return m_failureReason; }

    // Any thread: stop reading. The recorder lets go of the file at once (Recorder::stopPlaying)
    // or at its next refill.
    void cancel() { m_cancelled.store(true, std::memory_order_release); }
    bool cancelled() const { return m_cancelled.load(std::memory_order_acquire); }

private:
    friend class RecordingPlayer;

    const quint64 m_id;
    const QString m_path;
    SpscStereoRing m_ring;
    // Written once by the recorder thread before m_ready or m_failed is released.
    int m_sampleRate{0};
    quint64 m_totalFrames{0};
    QString m_failureReason;
    std::atomic<quint64> m_queued{0};
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_atEnd{false};
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_cancelled{false};
};

// Streams a finished recording into a PlaySession's ring. It parses the file with WavFile and
// plays exactly the frames the header declares, bit for bit. Lives on the recorder thread,
// which calls fill() on a timer; tests may drive it from any one thread.
class RecordingPlayer {
public:
    // `expectedRate` is the rate the output runs at; a file at another rate is refused.
    // 0 accepts any rate.
    RecordingPlayer(std::shared_ptr<PlaySession> session, int expectedRate);

    // Opens and parses the file and queues the first frames. False, with the session marked
    // failed and its reason set, if the file cannot be played.
    bool open();

    // Tops the ring up from the file. Returns true while there is more to do, false once every
    // frame is queued, the session was cancelled, or reading failed (the session says which).
    bool fill();

    const std::shared_ptr<PlaySession>& session() const { return m_session; }

private:
    bool fail(const QString& reason);

    std::shared_ptr<PlaySession> m_session;
    const int m_expectedRate;
    QFile m_file;
    quint64 m_total{0};
    quint64 m_queued{0};
    std::vector<float> m_scratch;
};

// The pacing of the engine's PLAY pump: the frames due by elapsed time, which is how a
// prototype fed PlaybackBuffer for 44 s with no underruns across ±300 ppm of clock drift.
// Engine thread only; times are QElapsedTimer::nsecsElapsed() values.
class PlayPacer {
public:
    void start(int sampleRate, qint64 nowNs)
    {
        m_rate = std::max(sampleRate, 1);
        m_startNs = nowNs;
        m_accounted = 0;
        m_taken = 0;
    }

    // Frames to take now: those due since the last call, at most `available` and at most one
    // second. What could not be taken is not owed later. A ring that ran dry, or a stalled
    // engine thread, then costs a gap rather than a rush that would overrun the output buffer.
    qint64 take(qint64 nowNs, qint64 available)
    {
        const qint64 due = framesAt(nowNs);
        const qint64 owed = due - m_accounted;
        m_accounted = due;
        const qint64 n = std::clamp<qint64>(std::min(owed, available), 0, m_rate);
        m_taken += n;
        return n;
    }

    qint64 taken() const { return m_taken; }

private:
    // Split so that no product can overflow, however long the playback.
    qint64 framesAt(qint64 nowNs) const
    {
        const qint64 ns = std::max<qint64>(nowNs - m_startNs, 0);
        constexpr qint64 kNsPerSecond = 1000000000;
        return ns / kNsPerSecond * m_rate + ns % kNsPerSecond * m_rate / kNsPerSecond;
    }

    qint64 m_rate{1};
    qint64 m_startNs{0};
    qint64 m_accounted{0};
    qint64 m_taken{0};
};

} // namespace esa
