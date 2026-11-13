#include "app/Recorder.h"

#include "app/RecordNaming.h"
#include "app/WavFile.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <utility>
#include <vector>

Q_LOGGING_CATEGORY(lcRecorder, "esa.recorder")

namespace esa {

namespace {

// Frames moved from a ring to the disk per write: 128 KB, a fraction of a second.
constexpr size_t kChunkFrames = 16384;
constexpr int kMaxRate = 48000;  // the play ring is sized for the fastest chain rate

QString shown(const QString& path)
{
    return QDir::toNativeSeparators(path);
}

} // namespace

RecordRequest RecordRequest::forFile(const QString& path, int sampleRate)
{
    const QFileInfo info(path);
    QString name = info.fileName();
    if (name.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive)) name.chop(4);
    return {info.path(), name, sampleRate};
}

RecordSession::RecordSession(quint64 id, RecordRequest request, size_t ringFrames)
    : m_id(id), m_request(std::move(request)), m_ring(ringFrames)
{
}

// Everything the disk thread does. Lives on Recorder::m_thread; every function runs there.
class RecorderWorker : public QObject {
public:
    RecorderWorker(Recorder* owner, const Recorder::Options& options)
        : m_owner(owner),
          m_options(options),
          m_rollBytes(std::clamp<quint64>(options.rollDataBytes, WavFile::kBytesPerFrame, WavFile::kMaxDataBytes)
                      / WavFile::kBytesPerFrame * WavFile::kBytesPerFrame)
    {
    }

    void init()
    {
        m_scratch.assign(kChunkFrames * WavFile::kChannels, 0.0f);
        m_drainTimer = new QTimer(this);
        m_drainTimer->setInterval(std::max(1, m_options.drainIntervalMs));
        connect(m_drainTimer, &QTimer::timeout, this, [this] { drainAll(); });
        m_playTimer = new QTimer(this);
        m_playTimer->setInterval(std::max(1, m_options.playRefillMs));
        connect(m_playTimer, &QTimer::timeout, this, [this] { refillAll(); });
    }

    void begin(const std::shared_ptr<RecordSession>& session, const QString& seriesBase = QString(),
               int filesOpened = 0)
    {
        auto a = std::make_unique<Active>();
        a->session = session;
        a->base = seriesBase.isEmpty() ? session->request().baseName : seriesBase;
        a->filesOpened = filesOpened;
        m_active.push_back(std::move(a));
        if (!m_drainTimer->isActive()) m_drainTimer->start();
    }

    void end(const std::shared_ptr<RecordSession>& session)
    {
        // finishAll() ends sessions nobody called finish() on: push() must stop for them too.
        session->m_ended.store(true, std::memory_order_relaxed);
        Active* a = find(session.get());
        if (!a) return;  // it failed earlier and is already gone
        if (drain(*a, true)) closeFile(*a);
        remove(a);
    }

    // A rate change: the old session's file closes and the new one carries on its name as the
    // next part.
    void rollTo(const std::shared_ptr<RecordSession>& previous, const std::shared_ptr<RecordSession>& next)
    {
        Active* a = find(previous.get());
        if (!a) {
            if (previous->failed()) {
                failUnstarted(*next, previous->failureReason());
            } else {
                begin(next);
            }
            return;
        }
        if (drain(*a, true)) closeFile(*a);
        const QString base = a->base;
        const int filesOpened = a->filesOpened;
        const bool failed = previous->failed();
        remove(a);
        if (failed) {
            failUnstarted(*next, previous->failureReason());
        } else {
            begin(next, base, filesOpened);
        }
    }

    void startPlay(const std::shared_ptr<PlaySession>& session, int expectedRate)
    {
        auto player = std::make_unique<RecordingPlayer>(session, expectedRate);
        if (!player->open()) {
            emit m_owner->playbackFailed(session->id(), session->failureReason());
            return;
        }
        if (session->atEnd()) return;  // it fitted in the ring at once
        m_players.push_back(std::move(player));
        if (!m_playTimer->isActive()) m_playTimer->start();
    }

    void stopPlay(const std::shared_ptr<PlaySession>& session)
    {
        session->cancel();
        std::erase_if(m_players, [&](const auto& p) { return p->session() == session; });
        if (m_players.empty()) m_playTimer->stop();
    }

    void finishAll()
    {
        while (!m_active.empty()) end(m_active.front()->session);
        for (const auto& p : m_players) p->session()->cancel();
        m_players.clear();
        m_playTimer->stop();
    }

private:
    struct Active {
        std::shared_ptr<RecordSession> session;
        QString base;         // the name parts are made from, with any collision suffix
        int filesOpened{0};   // across the whole recording, so parts number on after a rate roll
        std::unique_ptr<WavFile> file;
        QElapsedTimer sincePatch;
        quint64 patchedFrames{0};
    };

    Active* find(const RecordSession* session)
    {
        for (const auto& a : m_active) {
            if (a->session.get() == session) return a.get();
        }
        return nullptr;
    }

    void remove(Active* a)
    {
        const quint64 id = a->session->id();
        std::erase_if(m_active, [a](const auto& p) { return p.get() == a; });
        if (m_active.empty()) m_drainTimer->stop();
        emit m_owner->finished(id);
    }

    void drainAll()
    {
        // Copied first: a failure removes its session from m_active.
        std::vector<Active*> current;
        for (const auto& a : m_active) current.push_back(a.get());
        for (Active* a : current) {
            if (!drain(*a, false)) remove(a);
        }
    }

    // False if the session failed.
    bool drain(Active& a, bool final)
    {
        RecordSession& s = *a.session;
        for (;;) {
            const size_t n = s.m_ring.read(m_scratch.data(), kChunkFrames);
            if (n == 0) break;
            if (!write(a, m_scratch.data(), n)) return false;
        }
        if (!final && a.file && a.file->frames() != a.patchedFrames
            && a.sincePatch.elapsed() >= m_options.headerIntervalMs) {
            if (!a.file->patchHeader()) {
                fail(a, QStringLiteral("Could not update %1: %2")
                            .arg(shown(a.file->path()), a.file->errorString()));
                return false;
            }
            a.patchedFrames = a.file->frames();
            a.sincePatch.restart();
        }
        return true;
    }

    bool write(Active& a, float* stereo, size_t frames)
    {
        RecordSession& s = *a.session;
        while (frames > 0) {
            if (!a.file && !openNext(a)) return false;
            const quint64 room = (m_rollBytes - a.file->dataBytes()) / WavFile::kBytesPerFrame;
            if (room == 0) {
                // Full: close this part, and the next pass opens _part<n+1>.
                if (!closeFile(a)) return false;
                continue;
            }
            const size_t take = size_t(std::min<quint64>(frames, room));
            const quint64 before = a.file->frames();
            const bool ok = a.file->append(stereo, take);
            s.m_written.fetch_add(a.file->frames() - before, std::memory_order_relaxed);
            if (!ok) {
                fail(a, QStringLiteral("Could not write %1: %2")
                            .arg(shown(a.file->path()), a.file->errorString()));
                return false;
            }
            stereo += take * WavFile::kChannels;
            frames -= take;
        }
        return true;
    }

    bool openNext(Active& a)
    {
        RecordSession& s = *a.session;
        const QString folder = s.request().folder.isEmpty() ? QStringLiteral(".") : s.request().folder;
        if (a.base.isEmpty()) {
            fail(a, QStringLiteral("No file name for the recording"));
            return false;
        }
        if (!QDir().mkpath(folder)) {
            fail(a, QStringLiteral("Could not create the folder %1").arg(shown(folder)));
            return false;
        }
        const QDir dir(folder);
        const int part = a.filesOpened + 1;
        for (int collision = 0; collision <= RecordNaming::kMaxCollisionSuffix; ++collision) {
            const QString path = dir.filePath(RecordNaming::fileName(a.base, part, collision));
            std::unique_ptr<QFileDevice> device;
            if (m_options.openDevice) {
                device = m_options.openDevice(path);
            } else {
                device = std::make_unique<QFile>(path);
            }
            auto file = std::make_unique<WavFile>(std::move(device), s.sampleRate());
            switch (file->create()) {
            case WavFile::CreateResult::NameTaken:
                continue;
            case WavFile::CreateResult::Failed:
                fail(a, QStringLiteral("Could not create %1: %2").arg(shown(path), file->errorString()));
                return false;
            case WavFile::CreateResult::Created:
                // Later parts follow the name the first one actually got.
                if (part == 1 && collision > 0) a.base += QStringLiteral("_%1").arg(collision);
                a.filesOpened = part;
                a.file = std::move(file);
                a.patchedFrames = 0;
                a.sincePatch.start();
                qCInfo(lcRecorder) << "Recording to" << path;
                emit m_owner->fileOpened(s.id(), path);
                return true;
            }
        }
        fail(a, QStringLiteral("Could not create a recording in %1: every name from %2 to %3 is taken")
                    .arg(shown(folder), RecordNaming::fileName(a.base, part, 0),
                         RecordNaming::fileName(a.base, part, RecordNaming::kMaxCollisionSuffix)));
        return false;
    }

    // Closes the current file, keeping it if it has frames. False if its header could not be
    // brought up to date, which fails the session.
    bool closeFile(Active& a)
    {
        if (!a.file) return true;
        std::unique_ptr<WavFile> file = std::move(a.file);
        const quint64 frames = file->frames();
        if (frames == 0) {
            file->discard();
            return true;
        }
        const bool finished = file->finish();
        emit m_owner->fileClosed(a.session->id(), file->path(), frames, file->sampleRate());
        if (!finished) {
            fail(a, QStringLiteral("Could not finish %1: %2")
                        .arg(shown(file->path()), file->errorString()));
            return false;
        }
        return true;
    }

    // What reached the disk is kept, valid; the caller removes the session.
    void fail(Active& a, const QString& reason)
    {
        RecordSession& s = *a.session;
        if (a.file) {
            std::unique_ptr<WavFile> file = std::move(a.file);
            const quint64 frames = file->frames();
            if (frames == 0) {
                file->discard();
            } else {
                file->finish();  // a short write has closed it already, header and all
                emit m_owner->fileClosed(s.id(), file->path(), frames, file->sampleRate());
            }
        }
        qCWarning(lcRecorder) << reason;
        markFailed(s, reason);
    }

    void failUnstarted(RecordSession& s, const QString& reason)
    {
        markFailed(s, reason);
        emit m_owner->finished(s.id());
    }

    // Once only: other threads may be reading the reason as soon as the flag is set.
    void markFailed(RecordSession& s, const QString& reason)
    {
        if (s.m_failed.load(std::memory_order_relaxed)) return;
        s.m_failureReason = reason;
        s.m_failed.store(true, std::memory_order_release);
        emit m_owner->failed(s.id(), reason);
    }

    void refillAll()
    {
        std::vector<std::unique_ptr<RecordingPlayer>> kept;
        for (auto& p : m_players) {
            if (p->fill()) {
                kept.push_back(std::move(p));
            } else if (p->session()->failed()) {
                emit m_owner->playbackFailed(p->session()->id(), p->session()->failureReason());
            }
        }
        m_players = std::move(kept);
        if (m_players.empty()) m_playTimer->stop();
    }

    Recorder* const m_owner;
    const Recorder::Options m_options;
    const quint64 m_rollBytes;
    std::vector<std::unique_ptr<Active>> m_active;
    std::vector<std::unique_ptr<RecordingPlayer>> m_players;
    std::vector<float> m_scratch;
    QTimer* m_drainTimer{nullptr};
    QTimer* m_playTimer{nullptr};
};

template <typename Work>
void Recorder::post(Work&& work)
{
    m_handoffs.fetch_add(1, std::memory_order_release);
    QMetaObject::invokeMethod(
        m_worker,
        [this, work = std::forward<Work>(work)]() mutable {
            (void)m_handoffs.load(std::memory_order_acquire);
            work();
        },
        Qt::QueuedConnection);
}

Recorder::Recorder(QObject* parent) : Recorder(Options(), parent) {}

Recorder::Recorder(Options options, QObject* parent) : QObject(parent), m_options(std::move(options))
{
    m_worker = new RecorderWorker(this, m_options);
    m_worker->moveToThread(&m_thread);
    m_thread.setObjectName(QStringLiteral("recorder"));
    // Below the engine and the GUI: the rings absorb any delay, the audio path must not wait.
    m_thread.start(QThread::LowPriority);
    post([w = m_worker] { w->init(); });
}

Recorder::~Recorder()
{
    shutdown();
}

std::shared_ptr<RecordSession> Recorder::start(const RecordRequest& request)
{
    if (!m_worker || request.sampleRate <= 0) return nullptr;
    const size_t ringFrames = size_t(request.sampleRate) * size_t(std::max(1, m_options.ringSeconds));
    auto session = std::make_shared<RecordSession>(m_nextId.fetch_add(1), request, ringFrames);
    post([w = m_worker, session] { w->begin(session); });
    return session;
}

void Recorder::finish(const std::shared_ptr<RecordSession>& session)
{
    if (!session) return;
    session->m_ended.store(true, std::memory_order_relaxed);
    if (!m_worker) return;
    post([w = m_worker, session] { w->end(session); });
}

std::shared_ptr<RecordSession> Recorder::roll(const std::shared_ptr<RecordSession>& session, int sampleRate)
{
    if (!session) return nullptr;
    if (session->failed() || sampleRate <= 0 || !m_worker) {
        finish(session);
        return nullptr;
    }
    session->m_ended.store(true, std::memory_order_relaxed);
    RecordRequest request = session->request();
    request.sampleRate = sampleRate;
    const size_t ringFrames = size_t(sampleRate) * size_t(std::max(1, m_options.ringSeconds));
    auto next = std::make_shared<RecordSession>(m_nextId.fetch_add(1), request, ringFrames);
    post([w = m_worker, session, next] { w->rollTo(session, next); });
    return next;
}

std::shared_ptr<PlaySession> Recorder::play(const QString& path, int expectedRate)
{
    if (!m_worker) return nullptr;
    const size_t ringFrames =
        size_t(std::max(1, m_options.playRingSeconds)) * size_t(std::max(kMaxRate, expectedRate));
    auto session = std::make_shared<PlaySession>(m_nextId.fetch_add(1), path, ringFrames);
    post([w = m_worker, session, expectedRate] { w->startPlay(session, expectedRate); });
    return session;
}

void Recorder::stopPlaying(const std::shared_ptr<PlaySession>& session)
{
    if (!session) return;
    session->cancel();
    if (!m_worker) return;
    post([w = m_worker, session] { w->stopPlay(session); });
}

void Recorder::runBlocking(const std::function<void()>& work)
{
    if (!m_worker) return;
    if (QThread::currentThread() == &m_thread) {
        work();
    } else {
        // As in post(), in both directions: the caller goes on to read what the work did.
        m_handoffs.fetch_add(1, std::memory_order_release);
        QMetaObject::invokeMethod(
            m_worker,
            [this, &work] {
                (void)m_handoffs.load(std::memory_order_acquire);
                work();
                m_handoffs.fetch_add(1, std::memory_order_release);
            },
            Qt::BlockingQueuedConnection);
        (void)m_handoffs.load(std::memory_order_acquire);
    }
}

void Recorder::finishAll()
{
    runBlocking([w = m_worker] { w->finishAll(); });
}

void Recorder::shutdown()
{
    if (!m_worker) return;
    finishAll();
    // Deleted on its own thread, because it owns timers and open files.
    runBlocking([w = m_worker] { delete w; });
    m_worker = nullptr;
    m_thread.quit();
    m_thread.wait();
}

} // namespace esa
