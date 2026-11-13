#pragma once

#include "app/RecordingPlayer.h"
#include "audio/SpscRing.h"

#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <functional>
#include <memory>

class QFileDevice;

namespace esa {

class RecorderWorker;

// Where a recording goes. The first file is <folder>/<baseName>.wav, or the first free name of
// <baseName>_1.wav .. <baseName>_999.wav if that is taken; later parts add _part2, _part3 and
// so on (RecordNaming::fileName). Nothing is ever overwritten.
struct RecordRequest {
    QString folder;     // created when the first frames arrive, not before
    QString baseName;   // without ".wav"; RecordNaming::baseName() for REC
    int sampleRate{0};  // the rate the frames come at: the chain rate

    // For a path the operator typed (headless --record file.wav, --capture <base>.after.wav):
    // splits off the folder and drops a ".wav" ending.
    static RecordRequest forFile(const QString& path, int sampleRate);
};

// One recording at one sample rate. The engine thread is its producer and calls push() once per
// block; the recorder thread drains it. Everything else may be read from any thread for status.
// Made by Recorder::start() and Recorder::roll().
class RecordSession {
public:
    RecordSession(quint64 id, RecordRequest request, size_t ringFrames);

    quint64 id() const { return m_id; }
    int sampleRate() const { return m_request.sampleRate; }
    const RecordRequest& request() const { return m_request; }
    size_t ringCapacityFrames() const { return m_ring.capacity(); }

    // Engine thread only. Never blocks, locks or allocates: frames go into a ring sized for
    // 10 s that the recorder drains every 200 ms, so a disk stall of up to 10 s loses nothing.
    // Beyond that the excess is dropped and counted. A no-op once the session has failed or
    // been finished.
    void push(const float* stereo, int frames)
    {
        if (frames <= 0 || m_ended.load(std::memory_order_relaxed) || m_failed.load(std::memory_order_relaxed))
            return;
        const size_t written = m_ring.write(stereo, size_t(frames));
        if (written < size_t(frames)) m_dropped.fetch_add(quint64(size_t(frames) - written), std::memory_order_relaxed);
    }

    // Frames push() could not fit: shown in amber.
    quint64 droppedFrames() const { return m_dropped.load(std::memory_order_relaxed); }
    // Frames on disk, across every part of this session.
    quint64 framesWritten() const { return m_written.load(std::memory_order_relaxed); }
    // The recorder gave up (disk full, folder not writable). What reached the disk stays valid.
    bool failed() const { return m_failed.load(std::memory_order_acquire); }
    // Valid once failed() is true.
    QString failureReason() const { return m_failureReason; }

private:
    friend class Recorder;
    friend class RecorderWorker;

    const quint64 m_id;
    const RecordRequest m_request;
    SpscStereoRing m_ring;
    std::atomic<quint64> m_dropped{0};
    std::atomic<quint64> m_written{0};
    std::atomic<bool> m_ended{false};   // finish() or roll() was called
    std::atomic<bool> m_failed{false};
    QString m_failureReason;            // written once, before m_failed is released
};

static_assert(std::atomic<quint64>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
              "push() runs on the engine thread, which must never take a lock");

// The disk thread: it writes recordings and reads them back for PLAY, so the
// engine thread never touches the disk. It runs at QThread::LowPriority.
//
// How the engine uses it (Engine.cpp has the whole of it):
//   Recorder m_recorder;                                    // a member; its thread starts at once
//   m_record = m_recorder.start({folder, RecordNaming::baseName(now, hz, mode, rx), chainRate});
//   publishBlock:     if (m_record) m_record->push(stereo, frames);
//   rate change:      m_record = m_recorder.roll(m_record, newRate);   // the next file is _partN
//   stop, disconnect: m_recorder.finish(m_record); m_record.reset();
//   failed(id, ...):  if id is one of the recording's parts, it has stopped: report it once.
//                     A rate roll whose old part fails also fails its continuation, with the
//                     same reason, so the engine matches every part's id (Engine::onRecorderFailed).
//   fileClosed(...):  remember the path: the last file of a finished recording is what PLAY plays
//   PLAY:             m_play = m_recorder.play(path, sinkRate);  then pump it (see PlaySession)
//   shutdown:         m_recorder.shutdown();                // blocks until every file is closed
//
// Every call returns at once except finishAll() and shutdown(), and may be made from any thread
// other than during shutdown() or destruction; the work is queued to the disk thread in call
// order. Signals are emitted on the disk thread, so they reach receivers on other threads
// queued. A session's push() belongs to one producer thread, the engine's.
class Recorder : public QObject {
    Q_OBJECT

public:
    // Makes the device a file is written through, not yet open; called on the disk thread. The
    // default is a QFile. Tests return a QFile subclass that fails writes: the disk-full path on
    // Windows, where a child process cannot be given a file size limit.
    using DeviceFactory = std::function<std::unique_ptr<QFileDevice>(const QString& path)>;

    static constexpr quint64 kRollDataBytes = 3584ull * 1024 * 1024;  // 3.5 GiB, about 2.7 h at 48 kHz

    struct Options {
        int ringSeconds{10};             // per session, at its sample rate
        int drainIntervalMs{200};
        int headerIntervalMs{1000};      // how often the header is brought up to date
        quint64 rollDataBytes{kRollDataBytes};
        int playRingSeconds{2};          // per PLAY, at 48 kHz or the expected rate if higher
        int playRefillMs{100};
        DeviceFactory openDevice;        // empty: QFile
    };

    explicit Recorder(QObject* parent = nullptr);
    explicit Recorder(Options options, QObject* parent = nullptr);
    ~Recorder() override;  // shutdown()

    // Allocates the session and its ring here, on the calling thread, so that push() never has
    // to, and queues its start. The file opens when the first frames are drained, so a
    // recording that never gets audio leaves nothing behind. Null after shutdown() or for a
    // sample rate of 0 or less.
    std::shared_ptr<RecordSession> start(const RecordRequest& request);

    // Ends a recording: push() becomes a no-op, and the disk thread drains what is left, brings
    // the header up to date and closes the file. A recording with no frames leaves no file.
    void finish(const std::shared_ptr<RecordSession>& session);

    // For a stream-rate change: ends `session` and returns its continuation at `sampleRate`,
    // whose file is the next _partN, so frames at two rates never share a file. Null if `session`
    // has already failed.
    std::shared_ptr<RecordSession> roll(const std::shared_ptr<RecordSession>& session, int sampleRate);

    // Starts playing a finished recording into the returned session's ring. A file at a rate other
    // than `expectedRate` (0: any) is refused, through the session and playbackFailed().
    std::shared_ptr<PlaySession> play(const QString& path, int expectedRate = 0);
    // Cancels a PLAY and closes its file.
    void stopPlaying(const std::shared_ptr<PlaySession>& session);

    // Blocks until every recording is finished and every PLAY stopped. For disconnect and quit.
    // It waits for as long as the disk takes, so call it after anything that must not wait on a
    // disk, such as restoring ExpertSDR3's mute.
    void finishAll();
    // finishAll(), then stops the disk thread. Further calls do nothing. Safe to call twice.
    void shutdown();

    QThread* diskThread() { return &m_thread; }

signals:
    // A file was created: the first one of a session, or the next part.
    void fileOpened(quint64 sessionId, const QString& path);
    // A file was closed and kept, with `frames` frames at `sampleRate`: after a roll, a finish
    // or a failure. The last one of a finished session is what PLAY offers. Only when the
    // failure was in rewriting the header itself does the file declare fewer frames than this.
    void fileClosed(quint64 sessionId, const QString& path, quint64 frames, int sampleRate);
    // The session stopped writing: a short write (disk full), or a file that could not be
    // created. Whatever reached the disk stays valid. Then finished() follows. `reason` is the
    // bare cause ("Could not write <path>: No space left on device"), with no "Recording
    // stopped:" in front: whoever shows it says that.
    void failed(quint64 sessionId, const QString& reason);
    // The disk thread is done with the session, whether it was finished or failed.
    void finished(quint64 sessionId);
    void playbackFailed(quint64 playId, const QString& reason);

private:
    // Queues work to the disk thread. Qt's event queue orders everything the caller did before
    // with the work, but ThreadSanitizer cannot see inside a Qt it did not build. A release
    // here and an acquire there state the same ordering where it can see it, so a session made
    // on the engine thread and first touched on the disk thread is not reported as a race.
    template <typename Work>
    void post(Work&& work);
    void runBlocking(const std::function<void()>& work);

    QThread m_thread;
    RecorderWorker* m_worker{nullptr};
    std::atomic<quint64> m_nextId{1};
    std::atomic<quint64> m_handoffs{0};  // see post()
    const Options m_options;
};

} // namespace esa
