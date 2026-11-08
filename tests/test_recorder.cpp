// The recorder core: WavFile, RecordNaming, Recorder and
// RecordingPlayer, with synthetic frames. No engine, no network, no audio device. The
// disk-full and crash cases run this binary again as a child process (kChildFlag).
//
// At the end, the app itself: headless --record and --capture, and a recording killed
// mid-write, each run against a MockExpert in this process. The engine's own REC and PLAY
// tests are in test_engine_e2e.

#include "MockExpert.h"

#include "app/RecordNaming.h"
#include "app/Recorder.h"
#include "app/RecordingPlayer.h"
#include "app/WavFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QTimer>
#include <QtEndian>
#include <QtTest>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <vector>

#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/resource.h>
#endif

using namespace esa;

namespace {

constexpr int kTimeout = 10000;
constexpr char kChildFlag[] = "--recorder-child";

// Frame i of every synthetic recording: exact in float, and different in each channel, so a
// lost, doubled, swapped or shifted frame shows.
std::vector<float> pattern(quint64 first, size_t frames)
{
    std::vector<float> out(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const auto n = float((first + i) % 16777215);
        out[i * 2] = n;
        out[i * 2 + 1] = -(n + 1.0f);
    }
    return out;
}

std::vector<float> randomFrames(size_t frames, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-4.0f, 4.0f);
    std::vector<float> out(frames * 2);
    for (float& v : out) v = dist(rng);
    // Values a conversion would be most likely to disturb.
    if (out.size() >= 8) {
        out[0] = -0.0f;
        out[1] = std::numeric_limits<float>::denorm_min();
        out[2] = std::numeric_limits<float>::max();
        out[3] = std::numeric_limits<float>::lowest();
        out[4] = 1e-30f;
    }
    return out;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

quint32 bitsOf(float v)
{
    quint32 b;
    std::memcpy(&b, &v, 4);
    return b;
}

float floatOf(quint32 b)
{
    float v;
    std::memcpy(&v, &b, 4);
    return v;
}

QByteArray le16(quint16 v)
{
    QByteArray b(2, '\0');
    qToLittleEndian(v, b.data());
    return b;
}

QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    qToLittleEndian(v, b.data());
    return b;
}

quint32 at32(const QByteArray& b, int offset) { return qFromLittleEndian<quint32>(b.constData() + offset); }
quint16 at16(const QByteArray& b, int offset) { return qFromLittleEndian<quint16>(b.constData() + offset); }

QByteArray chunk(const char* id, const QByteArray& body)
{
    QByteArray c(id, 4);
    c += le32(quint32(body.size()));
    c += body;
    if (body.size() & 1) c += '\0';
    return c;
}

QByteArray fmtBody(quint16 format, quint16 channels, quint32 rate, quint16 bits)
{
    const quint16 align = quint16(channels * bits / 8);
    return le16(format) + le16(channels) + le32(rate) + le32(rate * align) + le16(align) + le16(bits);
}

QByteArray riff(const QByteArray& chunks, const char* form = "WAVE")
{
    return QByteArray("RIFF", 4) + le32(quint32(4 + chunks.size())) + QByteArray(form, 4) + chunks;
}

QByteArray bytesOf(const std::vector<float>& stereo)
{
    return QByteArray(reinterpret_cast<const char*>(stereo.data()), qsizetype(stereo.size() * sizeof(float)));
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// The frames a file holds, up to `frames` (all it declares by default).
std::vector<float> readFrames(const QString& path, quint64 frames = std::numeric_limits<quint64>::max())
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const std::optional<WavFile::Info> info = WavFile::parse(f);
    if (!info) return {};
    const quint64 n = std::min(frames, info->frames);
    std::vector<float> out(size_t(n) * 2);
    f.read(reinterpret_cast<char*>(out.data()), qint64(n) * 8);
    return out;
}

// Every whole frame after the header, whatever the header declares.
std::vector<float> framesOnDisk(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const std::optional<WavFile::Info> info = WavFile::parse(f);
    if (!info) return {};
    std::vector<float> out(size_t(info->framesInFile) * 2);
    f.read(reinterpret_cast<char*>(out.data()), qint64(out.size() * sizeof(float)));
    return out;
}

// A disk that fills up at `capacity` bytes: rewriting bytes the file already has succeeds, as
// it does on a full disk, and the first write past the end stops there. Injected through
// Recorder::Options::openDevice, it is how the disk-full path is covered on Windows.
class FullDiskFile : public QFile {
public:
    FullDiskFile(const QString& path, qint64 capacity) : QFile(path), m_capacity(capacity) {}

protected:
    qint64 writeData(const char* data, qint64 len) override
    {
        const qint64 allowed = std::clamp<qint64>(m_capacity - pos(), 0, len);
        if (allowed == 0) {
            setErrorString(QStringLiteral("There is not enough space on the disk."));
            return -1;
        }
        return QFile::writeData(data, allowed);
    }

private:
    const qint64 m_capacity;
};

Recorder::Options fastOptions()
{
    Recorder::Options o;
    o.drainIntervalMs = 10;
    o.headerIntervalMs = 30;
    o.playRefillMs = 5;
    return o;
}

// Nothing reaches the disk until finish(), so a test sees exactly what finishing does.
Recorder::Options idleOptions()
{
    Recorder::Options o;
    o.drainIntervalMs = 600000;
    o.headerIntervalMs = 600000;
    return o;
}

// The recorder's signals, collected on the test thread. QSignalSpy would record them on the
// disk thread, and before Qt 6.6 it has no lock for that.
struct Events : QObject {
    struct Closed {
        quint64 id;
        QString path;
        quint64 frames;
        int rate;
    };

    explicit Events(Recorder& r)
    {
        connect(&r, &Recorder::fileOpened, this, [this](quint64, const QString& p) { opened << p; });
        connect(&r, &Recorder::fileClosed, this, [this](quint64 id, const QString& p, quint64 frames, int rate) {
            closed.push_back({id, p, frames, rate});
        });
        connect(&r, &Recorder::failed, this, [this](quint64 id, const QString& why) {
            failedIds << id;
            reasons << why;
        });
        connect(&r, &Recorder::finished, this, [this](quint64 id) { finished << id; });
        connect(&r, &Recorder::playbackFailed, this, [this](quint64 id, const QString& why) {
            playFailedIds << id;
            reasons << why;
        });
    }

    QStringList opened;
    std::vector<Closed> closed;
    QList<quint64> failedIds;
    QStringList reasons;
    QList<quint64> finished;
    QList<quint64> playFailedIds;
};

void pushInBlocks(RecordSession& s, const std::vector<float>& stereo, int block = 480)
{
    const int frames = int(stereo.size() / 2);
    for (int i = 0; i < frames; i += block) s.push(stereo.data() + size_t(i) * 2, std::min(block, frames - i));
}

// Reads a PLAY to its end on this thread, as the engine's pump would.
std::vector<float> drainPlay(PlaySession& p, int timeoutMs = kTimeout)
{
    std::vector<float> got;
    std::vector<float> buf(4096 * 2);
    QElapsedTimer t;
    t.start();
    while (!p.failed() && t.elapsed() < timeoutMs) {
        if (!p.ready()) {
            QTest::qWait(2);
            continue;
        }
        const size_t n = p.ring().read(buf.data(), 4096);
        got.insert(got.end(), buf.begin(), buf.begin() + qsizetype(n) * 2);
        if (n == 0) {
            if (p.atEnd() && p.ring().size() == 0) break;
            QTest::qWait(1);
        }
    }
    return got;
}

QString waitForLine(QProcess& p, const QByteArray& prefix, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (!p.canReadLine()) {
            if (p.state() != QProcess::Running) break;
            p.waitForReadyRead(50);
            continue;
        }
        const QByteArray line = p.readLine().trimmed();
        if (line.startsWith(prefix)) return QString::fromUtf8(line.mid(prefix.size()));
    }
    return {};
}

// ── Child processes ─────────────────────────────────────────────────────────

void say(const QByteArray& line)
{
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Records for ever, as fast as real time, until killed. Capped at 30 s in case nobody does.
int childRecordUntilKilled(const QString& folder, int drainMs, int headerMs)
{
    Recorder::Options o;
    o.drainIntervalMs = drainMs;
    o.headerIntervalMs = headerMs;
    Recorder recorder(o);
    QObject::connect(&recorder, &Recorder::fileOpened, qApp,
                     [](quint64, const QString& path) { say("opened=" + path.toUtf8()); });
    QObject::connect(&recorder, &Recorder::failed, qApp, [](quint64, const QString& why) {
        say("failed=" + why.toUtf8());
        QCoreApplication::exit(3);
    });
    const std::shared_ptr<RecordSession> session = recorder.start({folder, QStringLiteral("crash"), 48000});

    quint64 next = 0;
    QElapsedTimer clock;
    clock.start();
    QTimer engine;
    engine.setTimerType(Qt::PreciseTimer);
    engine.setInterval(10);
    QObject::connect(&engine, &QTimer::timeout, qApp, [&] {
        const auto due = quint64(clock.elapsed()) * 48;
        if (due <= next) return;
        const std::vector<float> block = pattern(next, size_t(due - next));
        session->push(block.data(), int(due - next));
        next = due;
    });
    engine.start();
    QTimer::singleShot(30000, qApp, [] { QCoreApplication::exit(4); });
    return QCoreApplication::exec();
}

#ifdef Q_OS_UNIX
// Records with the file size limited to `limit` bytes, which is how a full disk looks to a
// process: write() stops short and then fails (EFBIG; SIGXFSZ is ignored so it does not kill).
// It plays on after the failure, as the engine must.
int childDiskFull(const QString& folder, qint64 limit)
{
    std::signal(SIGXFSZ, SIG_IGN);
    rlimit r{};
    if (getrlimit(RLIMIT_FSIZE, &r) != 0) return 10;
    r.rlim_cur = rlim_t(limit);
    if (setrlimit(RLIMIT_FSIZE, &r) != 0) return 11;

    Recorder::Options o;
    o.drainIntervalMs = 20;
    o.headerIntervalMs = 50;
    Recorder recorder(o);
    QString path;
    QString reason;
    QObject::connect(&recorder, &Recorder::fileOpened, qApp, [&](quint64, const QString& p) { path = p; });
    QObject::connect(&recorder, &Recorder::failed, qApp, [&](quint64, const QString& why) { reason = why; });
    const std::shared_ptr<RecordSession> session = recorder.start({folder, QStringLiteral("full"), 48000});

    // The engine thread: a block every 5 ms, recorded and played. 960 frames per 5 ms is four
    // times real time, to be done sooner.
    constexpr int kBlock = 960;
    quint64 next = 0;
    quint64 playedAfterFailure = 0;
    QTimer engine;
    engine.setInterval(5);
    QObject::connect(&engine, &QTimer::timeout, qApp, [&] {
        const std::vector<float> block = pattern(next, kBlock);
        session->push(block.data(), kBlock);
        next += kBlock;
        if (session->failed()) playedAfterFailure += kBlock;  // what the sink went on receiving
        if (!reason.isEmpty() && playedAfterFailure >= 48000) QCoreApplication::exit(0);
    });
    engine.start();
    QTimer::singleShot(20000, qApp, [] { QCoreApplication::exit(4); });
    const int rc = QCoreApplication::exec();
    engine.stop();
    recorder.finish(session);
    recorder.shutdown();

    say("path=" + path.toUtf8());
    say("reason=" + reason.toUtf8());
    say("failedFlag=" + QByteArray::number(session->failed()));
    say("written=" + QByteArray::number(session->framesWritten()));
    say("dropped=" + QByteArray::number(session->droppedFrames()));
    say("pushed=" + QByteArray::number(next));
    say("playedAfterFailure=" + QByteArray::number(playedAfterFailure));
    return rc;
}
#endif

int runChild(const QStringList& args)
{
    const QString mode = args.value(0);
    if (mode == QLatin1String("forever"))
        return childRecordUntilKilled(args.value(1), args.value(2).toInt(), args.value(3).toInt());
#ifdef Q_OS_UNIX
    if (mode == QLatin1String("diskfull")) return childDiskFull(args.value(1), args.value(2).toLongLong());
#endif
    return 2;
}

// ── The app, headless ───────────────────────────────────────────────────────

// Always an explicit --port, which turns the fallback ports off: one of those is where a real
// ExpertSDR3 may be listening. Always a settings file of its own, and the null output.
QStringList headlessArgs(const MockExpert& mock, const QTemporaryDir& dir)
{
    return {QStringLiteral("--headless"), QStringLiteral("--host"), QStringLiteral("127.0.0.1"),
            QStringLiteral("--port"), QString::number(mock.port()), QStringLiteral("--no-mute"),
            QStringLiteral("--device"), QStringLiteral("null"),
            QStringLiteral("--settings"), dir.filePath(QStringLiteral("settings.ini"))};
}

// Runs this thread's event loop, which the mock needs, until the child has gone.
bool waitForExit(QProcess& child, int timeoutMs)
{
    return QTest::qWaitFor([&] { return child.state() == QProcess::NotRunning; }, timeoutMs);
}

QJsonObject summaryOf(const QByteArray& output)
{
    for (const QByteArray& line : output.split('\n')) {
        if (line.startsWith("SUMMARY ")) return QJsonDocument::fromJson(line.mid(8)).object();
    }
    return {};
}

// A sanitizer build's child reports there rather than failing on its own. Its reports, that
// is: a summary of matched suppressions is not one.
bool sanitizerSpoke(const QByteArray& stderrText)
{
    return stderrText.contains("WARNING: ThreadSanitizer") || stderrText.contains("ERROR: ThreadSanitizer")
           || stderrText.contains("ERROR: AddressSanitizer") || stderrText.contains("runtime error:");
}

QStringList radioChangingCommands(const QStringList& commands)
{
    QStringList found;
    for (const QString& c : commands) {
        if (c.startsWith(QLatin1String("trx")) || c.startsWith(QLatin1String("volume"))
            || c.startsWith(QLatin1String("mute"))) {
            found << c;
        }
    }
    return found;
}

#ifdef Q_OS_UNIX
QMap<QString, QString> keyValues(const QByteArray& output)
{
    QMap<QString, QString> out;
    for (const QByteArray& line : output.split('\n')) {
        const qsizetype eq = line.indexOf('=');
        if (eq > 0) out.insert(QString::fromUtf8(line.left(eq)), QString::fromUtf8(line.mid(eq + 1)).trimmed());
    }
    return out;
}
#endif

} // namespace

class TestRecorder : public QObject {
    Q_OBJECT

private slots:
    // ── WavFile ─────────────────────────────────────────────────────────────

    void headerLayout()
    {
        const QByteArray h = WavFile::header(48000, 1000);
        QCOMPARE(h.size(), 58);
        QCOMPARE(h.mid(0, 4), QByteArray("RIFF"));
        QCOMPARE(at32(h, 4), 50u + 8000u);
        QCOMPARE(h.mid(8, 8), QByteArray("WAVEfmt "));
        QCOMPARE(at32(h, 16), 18u);       // fmt chunk size
        QCOMPARE(at16(h, 20), quint16(3));  // IEEE float
        QCOMPARE(at16(h, 22), quint16(2));
        QCOMPARE(at32(h, 24), 48000u);
        QCOMPARE(at32(h, 28), 48000u * 8);
        QCOMPARE(at16(h, 32), quint16(8));
        QCOMPARE(at16(h, 34), quint16(32));
        QCOMPARE(at16(h, 36), quint16(0));  // cbSize
        QCOMPARE(h.mid(38, 4), QByteArray("fact"));
        QCOMPARE(at32(h, 42), 4u);
        QCOMPARE(at32(h, 46), 1000u);
        QCOMPARE(h.mid(50, 4), QByteArray("data"));
        QCOMPARE(at32(h, 54), 8000u);

        // Past what RIFF can describe, the header says the most it can, in whole frames.
        const QByteArray big = WavFile::header(48000, quint64(1) << 40);
        QCOMPARE(quint64(at32(big, 54)), WavFile::kMaxDataBytes);
        QCOMPARE(quint64(at32(big, 4)), WavFile::kMaxDataBytes + 50);
        QCOMPARE(WavFile::kMaxDataBytes % 8, 0u);
    }

    void headerRoundTrip_data()
    {
        QTest::addColumn<int>("rate");
        QTest::addColumn<quint64>("frames");
        QTest::newRow("48 kHz, empty") << 48000 << quint64(0);
        QTest::newRow("48 kHz, one frame") << 48000 << quint64(1);
        QTest::newRow("24 kHz") << 24000 << quint64(12345);
        QTest::newRow("48 kHz, 2 s") << 48000 << quint64(96000);
    }

    void headerRoundTrip()
    {
        QFETCH(int, rate);
        QFETCH(quint64, frames);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("a.wav"));
        const std::vector<float> data = pattern(0, size_t(frames));
        QVERIFY(writeBytes(path, WavFile::header(rate, frames) + bytesOf(data)));

        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QString error;
        const std::optional<WavFile::Info> info = WavFile::parse(f, &error);
        QVERIFY2(info, qPrintable(error));
        QCOMPARE(info->sampleRate, rate);
        QCOMPARE(info->dataOffset, qint64(58));
        QCOMPARE(info->frames, frames);
        QCOMPARE(info->declaredFrames, frames);
        QCOMPARE(info->framesInFile, frames);
        QCOMPARE(f.pos(), qint64(58));  // left at the first frame
        QVERIFY(sameBits(readFrames(path), data));
    }

    // The writer was killed after writing frames it had not yet put in the header.
    void parseHeaderBehindData()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("a.wav"));
        QVERIFY(writeBytes(path, WavFile::header(48000, 1000) + bytesOf(pattern(0, 1500)) + QByteArray(3, 'x')));
        QString error;
        const std::optional<WavFile::Info> info = WavFile::parseFile(path, &error);
        QVERIFY2(info, qPrintable(error));
        QCOMPARE(info->frames, quint64(1000));
        QCOMPARE(info->declaredFrames, quint64(1000));
        QCOMPARE(info->framesInFile, quint64(1500));
        QVERIFY(sameBits(readFrames(path), pattern(0, 1000)));

        // Killed before the first patch: the header still says 0, and the frames are all there.
        QVERIFY(writeBytes(path, WavFile::header(48000, 0) + bytesOf(pattern(0, 700))));
        const std::optional<WavFile::Info> early = WavFile::parseFile(path, &error);
        QVERIFY2(early, qPrintable(error));
        QCOMPARE(early->frames, quint64(0));
        QCOMPARE(early->framesInFile, quint64(700));
    }

    // The file was cut short: what is there plays.
    void parseHeaderAheadOfData()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("a.wav"));
        QVERIFY(writeBytes(path, WavFile::header(24000, 1000) + bytesOf(pattern(0, 600)) + QByteArray(5, 'x')));
        const std::optional<WavFile::Info> info = WavFile::parseFile(path);
        QVERIFY(info);
        QCOMPARE(info->sampleRate, 24000);
        QCOMPARE(info->frames, quint64(600));
        QCOMPARE(info->declaredFrames, quint64(1000));
        QCOMPARE(info->framesInFile, quint64(600));
    }

    void parseOtherWriters()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("a.wav"));
        const QByteArray frames = bytesOf(pattern(0, 10));

        // The 44-byte layout of the old --capture writer: 16-byte fmt, no fact chunk.
        QVERIFY(writeBytes(path, riff(chunk("fmt ", fmtBody(3, 2, 48000, 32)) + chunk("data", frames))));
        std::optional<WavFile::Info> info = WavFile::parseFile(path);
        QVERIFY(info);
        QCOMPARE(info->dataOffset, qint64(44));
        QCOMPARE(info->frames, quint64(10));

        // An odd-sized chunk before the data is padded to an even size.
        QVERIFY(writeBytes(path, riff(chunk("fmt ", fmtBody(3, 2, 48000, 32)) + chunk("LIST", "abcde")
                                      + chunk("data", frames))));
        info = WavFile::parseFile(path);
        QVERIFY(info);
        QCOMPARE(info->dataOffset, qint64(12 + 24 + 14 + 8));
        QCOMPARE(info->frames, quint64(10));
        QVERIFY(sameBits(readFrames(path), pattern(0, 10)));

        // WAVE_FORMAT_EXTENSIBLE with the float sub-format.
        const QByteArray ext = fmtBody(0xFFFE, 2, 24000, 32) + le16(22) + le16(32) + le32(3)
                               + le16(3) + QByteArray::fromHex("000000001000800000aa00389b71");
        QCOMPARE(ext.size(), 40);
        QVERIFY(writeBytes(path, riff(chunk("fmt ", ext) + chunk("data", frames))));
        info = WavFile::parseFile(path);
        QVERIFY(info);
        QCOMPARE(info->sampleRate, 24000);
        QCOMPARE(info->frames, quint64(10));

        // A writer that could not seek leaves the size at its maximum: read to the end.
        QVERIFY(writeBytes(path, riff(chunk("fmt ", fmtBody(3, 2, 48000, 32))) + "data" + le32(0xFFFFFFFFu)
                                     + frames));
        info = WavFile::parseFile(path);
        QVERIFY(info);
        QCOMPARE(info->frames, quint64(10));
    }

    void parseRefuses_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        const QByteArray floatFmt = chunk("fmt ", fmtBody(3, 2, 48000, 32));
        const QByteArray data = chunk("data", QByteArray(16, '\0'));
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("too short") << QByteArray("RIFF\0\0", 6);
        QTest::newRow("not RIFF") << QByteArray("RIFX") + riff(floatFmt + data).mid(4);
        QTest::newRow("not WAVE") << riff(floatFmt + data, "AVI ");
        QTest::newRow("PCM16") << riff(chunk("fmt ", fmtBody(1, 2, 48000, 16)) + data);
        QTest::newRow("mono") << riff(chunk("fmt ", fmtBody(3, 1, 48000, 32)) + data);
        QTest::newRow("64-bit float") << riff(chunk("fmt ", fmtBody(3, 2, 48000, 64)) + data);
        QTest::newRow("rate 0") << riff(chunk("fmt ", fmtBody(3, 2, 0, 32)) + data);
        QTest::newRow("no data chunk") << riff(floatFmt);
        QTest::newRow("no format chunk") << riff(data);
        QTest::newRow("data before format") << riff(data + floatFmt);
        QTest::newRow("format too short") << riff(chunk("fmt ", fmtBody(3, 2, 48000, 32).left(14)) + data);
        QTest::newRow("format cut short") << riff(floatFmt).left(12 + 8 + 10);
    }

    void parseRefuses()
    {
        QFETCH(QByteArray, bytes);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("a.wav"));
        QVERIFY(writeBytes(path, bytes));
        QString error;
        QVERIFY(!WavFile::parseFile(path, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!WavFile::parseFile(dir.filePath(QStringLiteral("missing.wav")), &error));
        QVERIFY(!error.isEmpty());
    }

    void sanitizeZeroesOnlyNonFinite()
    {
        std::vector<float> v = {
            std::numeric_limits<float>::quiet_NaN(),
            floatOf(0xFFC00001u),  // a negative NaN with a payload
            floatOf(0x7FA00000u),  // signalling NaN
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),
            0.0f,
            -0.0f,
            std::numeric_limits<float>::denorm_min(),
            -std::numeric_limits<float>::denorm_min(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::lowest(),
            1.5f,
            -2.25f,
        };
        const std::vector<float> before = v;
        WavFile::sanitize(v.data(), v.size());
        for (size_t i = 0; i < 5; ++i) QCOMPARE(bitsOf(v[i]), 0u);
        for (size_t i = 5; i < v.size(); ++i) QCOMPARE(bitsOf(v[i]), bitsOf(before[i]));
    }

    void writerNeverOverwrites()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("a.wav"));
        QVERIFY(writeBytes(path, "keep"));
        WavFile w(std::make_unique<QFile>(path), 48000);
        QCOMPARE(w.create(), WavFile::CreateResult::NameTaken);
        QCOMPARE(readBytes(path), QByteArray("keep"));
    }

    void writerWritesAndDiscards()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("a.wav"));
        {
            WavFile w(std::make_unique<QFile>(path), 24000);
            QCOMPARE(w.create(), WavFile::CreateResult::Created);
            QCOMPARE(QFileInfo(path).size(), qint64(58));  // valid from the moment it opens
            QVERIFY(WavFile::parseFile(path));
            std::vector<float> a = pattern(0, 300);
            std::vector<float> b = pattern(300, 200);
            QVERIFY(w.append(a.data(), 300));
            QVERIFY(w.append(b.data(), 200));
            QCOMPARE(w.frames(), quint64(500));
            QVERIFY(w.finish());
            QVERIFY(!w.isOpen());
        }
        QCOMPARE(QFileInfo(path).size(), qint64(58 + 500 * 8));
        const std::optional<WavFile::Info> info = WavFile::parseFile(path);
        QVERIFY(info);
        QCOMPARE(info->sampleRate, 24000);
        QCOMPARE(info->frames, quint64(500));
        QVERIFY(sameBits(readFrames(path), pattern(0, 500)));

        const QString other = dir.filePath(QStringLiteral("b.wav"));
        WavFile w(std::make_unique<QFile>(other), 48000);
        QCOMPARE(w.create(), WavFile::CreateResult::Created);
        w.discard();
        QVERIFY(!QFile::exists(other));
    }

    void writerKeepsWholeFramesOnShortWrite()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("a.wav"));
        WavFile w(std::make_unique<FullDiskFile>(path, 58 + 100 * 8 + 3), 48000);
        QCOMPARE(w.create(), WavFile::CreateResult::Created);
        std::vector<float> a = pattern(0, 50);
        std::vector<float> b = pattern(50, 100);
        QVERIFY(w.append(a.data(), 50));
        QVERIFY(!w.append(b.data(), 100));  // room for 50.375 more frames
        QVERIFY(w.errorString().contains(QStringLiteral("not enough space")));
        QVERIFY(!w.isOpen());
        QCOMPARE(w.frames(), quint64(100));
        QCOMPARE(QFileInfo(path).size(), qint64(58 + 100 * 8));  // the part frame is cut off
        const std::optional<WavFile::Info> info = WavFile::parseFile(path);
        QVERIFY(info);
        QCOMPARE(info->declaredFrames, quint64(100));
        QCOMPARE(info->framesInFile, quint64(100));
        QVERIFY(sameBits(readFrames(path), pattern(0, 100)));
    }

    void writerCreateFailsWhenHeaderDoesNotFit()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("a.wav"));
        WavFile w(std::make_unique<FullDiskFile>(path, 20), 48000);
        QCOMPARE(w.create(), WavFile::CreateResult::Failed);
        QVERIFY(w.errorString().contains(QStringLiteral("not enough space")));
        QVERIFY(!QFile::exists(path));
    }

    // ── RecordNaming ────────────────────────────────────────────────────────

    void naming_data()
    {
        QTest::addColumn<qint64>("hz");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("receiver");
        QTest::addColumn<QString>("expected");
        const QString t = QStringLiteral("2026-11-07_141503Z_");
        QTest::newRow("RecordNaming.h's example") << qint64(14200000) << "usb" << 0 << t + "14.200MHz_USB";
        QTest::newRow("second receiver") << qint64(14200000) << "USB" << 1 << t + "14.200MHz_USB_RX1";
        QTest::newRow("no mode") << qint64(7074000) << "" << 0 << t + "7.074MHz_RX0";
        QTest::newRow("no mode, RX1") << qint64(7074000) << "" << 1 << t + "7.074MHz_RX1";
        QTest::newRow("mode reduced") << qint64(3573000) << "dig-u/2" << 0 << t + "3.573MHz_DIGU2";
        QTest::newRow("mode of symbols only") << qint64(3573000) << "--" << 0 << t + "3.573MHz_RX0";
        QTest::newRow("rounds down") << qint64(14200499) << "cw" << 0 << t + "14.200MHz_CW";
        QTest::newRow("rounds up") << qint64(14200500) << "cw" << 0 << t + "14.201MHz_CW";
        QTest::newRow("below 1 MHz") << qint64(475000) << "am" << 0 << t + "0.475MHz_AM";
        QTest::newRow("10 GHz") << qint64(10489550000) << "nfm" << 0 << t + "10489.550MHz_NFM";
        QTest::newRow("unknown frequency") << qint64(0) << "usb" << 0 << t + "0.000MHz_USB";
    }

    void naming()
    {
        QFETCH(qint64, hz);
        QFETCH(QString, mode);
        QFETCH(int, receiver);
        QFETCH(QString, expected);
        const QDateTime start(QDate(2026, 11, 7), QTime(14, 15, 3), QTimeZone::utc());
        QCOMPARE(RecordNaming::baseName(start, hz, mode, receiver), expected);
    }

    void namingConvertsToUtc()
    {
        // 00:30 in India on 8 November is 19:00 UTC on 7 November.
        const QDateTime india(QDate(2026, 11, 8), QTime(0, 30, 0), QTimeZone(5 * 3600 + 1800));
        QCOMPARE(RecordNaming::baseName(india, 14074000, QStringLiteral("digu"), 0),
                 QStringLiteral("2026-11-07_190000Z_14.074MHz_DIGU"));
    }

    void fileNames()
    {
        const QString b = QStringLiteral("2026-11-07_141503Z_14.200MHz_USB");
        QCOMPARE(RecordNaming::fileName(b, 1, 0), b + ".wav");
        QCOMPARE(RecordNaming::fileName(b, 1, 1), b + "_1.wav");
        QCOMPARE(RecordNaming::fileName(b, 1, 999), b + "_999.wav");
        QCOMPARE(RecordNaming::fileName(b, 2, 0), b + "_part2.wav");
        QCOMPARE(RecordNaming::fileName(b, 3, 1), b + "_part3_1.wav");

        RecordRequest r = RecordRequest::forFile(QStringLiteral("/data/rec/take.wav"), 48000);
        QCOMPARE(r.folder, QStringLiteral("/data/rec"));
        QCOMPARE(r.baseName, QStringLiteral("take"));
        QCOMPARE(r.sampleRate, 48000);
        r = RecordRequest::forFile(QStringLiteral("/data/take.before.WAV"), 24000);
        QCOMPARE(r.baseName, QStringLiteral("take.before"));
        r = RecordRequest::forFile(QStringLiteral("raw"), 48000);
        QCOMPARE(r.folder, QStringLiteral("."));
        QCOMPARE(r.baseName, QStringLiteral("raw"));
    }

    // ── Recorder ────────────────────────────────────────────────────────────

    void runsOnLowPriorityThread()
    {
        Recorder recorder;
        QVERIFY(recorder.diskThread()->isRunning());
        QVERIFY(recorder.diskThread() != QThread::currentThread());
        QCOMPARE(recorder.diskThread()->priority(), QThread::LowPriority);
        QCOMPARE(Recorder::kRollDataBytes, 3758096384ull);  // 3.5 GiB
        QVERIFY(Recorder::kRollDataBytes <= WavFile::kMaxDataBytes);
        QCOMPARE(Recorder::kRollDataBytes % WavFile::kBytesPerFrame, 0u);
    }

    // The cadence the crash and disk-stall guarantees rest on: a disk may stall for 10 s without
    // losing audio, and a crash loses at most the last second from the declared length.
    void shippingDefaults()
    {
        const Recorder::Options o;
        QCOMPARE(o.ringSeconds, 10);
        QCOMPARE(o.drainIntervalMs, 200);
        QCOMPARE(o.headerIntervalMs, 1000);
        QCOMPARE(o.rollDataBytes, Recorder::kRollDataBytes);
        QCOMPARE(o.playRingSeconds, 2);
        QVERIFY(!o.openDevice);
    }

    // finishAll() is how disconnect and quit end recordings. A session it ended takes no more
    // frames, as after finish(): nothing piles up in a ring nobody drains, nothing is counted
    // as dropped, and the session does not look alive.
    void finishAllEndsEverySession()
    {
        QTemporaryDir dir;
        Recorder recorder(fastOptions());
        Events ev(recorder);
        const auto s = recorder.start({dir.path(), QStringLiteral("w"), 48000});
        pushInBlocks(*s, pattern(0, 100));
        recorder.finishAll();
        QCOMPARE(s->framesWritten(), quint64(100));
        pushInBlocks(*s, pattern(100, 30 * 48000));
        QCOMPARE(s->droppedFrames(), quint64(0));  // 30 s into a 10 s ring: only a no-op drops nothing
        QTest::qWait(50);
        QCOMPARE(s->framesWritten(), quint64(100));
        QVERIFY(sameBits(readFrames(dir.filePath(QStringLiteral("w.wav"))), pattern(0, 100)));
        QTRY_VERIFY_WITH_TIMEOUT(ev.finished.contains(s->id()), kTimeout);
    }

    void sessionRingHoldsTenSeconds()
    {
        QTemporaryDir dir;
        Recorder recorder;
        const auto a = recorder.start({dir.path(), QStringLiteral("a"), 48000});
        const auto b = recorder.start({dir.path(), QStringLiteral("b"), 24000});
        QVERIFY(a && b);
        QCOMPARE(a->ringCapacityFrames(), size_t(480000));
        QCOMPARE(b->ringCapacityFrames(), size_t(240000));
        QVERIFY(a->id() != b->id());
        QVERIFY(!recorder.start({dir.path(), QStringLiteral("c"), 0}));
        recorder.finish(a);
        recorder.finish(b);
    }

    void recordThenPlayIsBitExact()
    {
        QTemporaryDir dir;
        Recorder recorder(fastOptions());
        Events ev(recorder);
        const std::vector<float> input = randomFrames(96000, 1);  // 2 s at 48 kHz
        const auto s = recorder.start({dir.path(), QStringLiteral("take"), 48000});
        pushInBlocks(*s, input);
        QTRY_VERIFY_WITH_TIMEOUT(s->framesWritten() == 96000, kTimeout);
        recorder.finish(s);
        QTRY_VERIFY_WITH_TIMEOUT(ev.finished.contains(s->id()), kTimeout);

        const QString path = dir.filePath(QStringLiteral("take.wav"));
        QCOMPARE(ev.opened, QStringList{path});
        QCOMPARE(ev.closed.size(), size_t(1));
        QCOMPARE(ev.closed[0].path, path);
        QCOMPARE(ev.closed[0].frames, quint64(96000));
        QCOMPARE(ev.closed[0].rate, 48000);
        QVERIFY(ev.failedIds.isEmpty());
        QCOMPARE(s->droppedFrames(), quint64(0));
        QVERIFY(!s->failed());
        QCOMPARE(QFileInfo(path).size(), qint64(58 + 96000 * 8));
        QVERIFY(sameBits(readFrames(path), input));

        const auto play = recorder.play(path, 48000);
        QVERIFY(play);
        const std::vector<float> played = drainPlay(*play);
        QVERIFY2(!play->failed(), qPrintable(play->failureReason()));
        QCOMPARE(play->sampleRate(), 48000);
        QCOMPARE(play->totalFrames(), quint64(96000));
        QVERIFY(sameBits(played, input));
    }

    void nonFiniteSamplesRecordedAsZero()
    {
        QTemporaryDir dir;
        Recorder recorder(idleOptions());
        std::vector<float> input = pattern(1, 100);
        input[10] = std::numeric_limits<float>::quiet_NaN();
        input[11] = std::numeric_limits<float>::infinity();
        input[51] = -std::numeric_limits<float>::infinity();
        const auto s = recorder.start({dir.path(), QStringLiteral("nan"), 48000});
        s->push(input.data(), 100);
        recorder.finishAll();
        std::vector<float> expected = input;
        expected[10] = expected[11] = expected[51] = 0.0f;
        QVERIFY(sameBits(readFrames(dir.filePath(QStringLiteral("nan.wav"))), expected));
    }

    void zeroFrameStopLeavesNoFile()
    {
        QTemporaryDir dir;
        const QString folder = dir.filePath(QStringLiteral("not/yet"));
        Recorder recorder(fastOptions());
        Events ev(recorder);
        const auto s = recorder.start({folder, QStringLiteral("empty"), 48000});
        QTest::qWait(50);
        recorder.finish(s);
        QTRY_VERIFY_WITH_TIMEOUT(ev.finished.contains(s->id()), kTimeout);
        QVERIFY(ev.opened.isEmpty());
        QVERIFY(ev.closed.empty());
        QVERIFY(ev.failedIds.isEmpty());
        QVERIFY(!QFileInfo::exists(folder));  // not even the folder: it is made at the first frames
    }

    void collisionSuffixes()
    {
        QTemporaryDir dir;
        QVERIFY(writeBytes(dir.filePath(QStringLiteral("take.wav")), "first"));
        QVERIFY(writeBytes(dir.filePath(QStringLiteral("take_1.wav")), "second"));
        Recorder::Options o = idleOptions();
        o.rollDataBytes = 100 * 8;
        Recorder recorder(o);
        Events ev(recorder);
        const auto s = recorder.start({dir.path(), QStringLiteral("take"), 48000});
        pushInBlocks(*s, pattern(0, 150), 50);
        recorder.finishAll();
        QTRY_COMPARE_WITH_TIMEOUT(ev.closed.size(), size_t(2), kTimeout);
        QCOMPARE(ev.opened, QStringList({dir.filePath(QStringLiteral("take_2.wav")),
                                         dir.filePath(QStringLiteral("take_2_part2.wav"))}));
        QCOMPARE(readBytes(dir.filePath(QStringLiteral("take.wav"))), QByteArray("first"));
        QCOMPARE(readBytes(dir.filePath(QStringLiteral("take_1.wav"))), QByteArray("second"));
        QVERIFY(sameBits(readFrames(ev.opened[0]), pattern(0, 100)));
        QVERIFY(sameBits(readFrames(ev.opened[1]), pattern(100, 50)));
    }

    void collisionsRunOut()
    {
        QTemporaryDir dir;
        QVERIFY(writeBytes(dir.filePath(QStringLiteral("busy.wav")), "x"));
        for (int i = 1; i <= RecordNaming::kMaxCollisionSuffix; ++i)
            QVERIFY(writeBytes(dir.filePath(QStringLiteral("busy_%1.wav").arg(i)), "x"));
        Recorder recorder(fastOptions());
        Events ev(recorder);
        const auto s = recorder.start({dir.path(), QStringLiteral("busy"), 48000});
        pushInBlocks(*s, pattern(0, 100));
        QTRY_VERIFY_WITH_TIMEOUT(ev.finished.contains(s->id()), kTimeout);
        QCOMPARE(ev.failedIds, QList<quint64>{s->id()});
        QVERIFY2(ev.reasons.value(0).contains(QStringLiteral("taken")), qPrintable(ev.reasons.value(0)));
        QVERIFY(s->failed());
        QCOMPARE(s->failureReason(), ev.reasons.value(0));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 1000);
    }

    void rollsAtDataLimit()
    {
        QTemporaryDir dir;
        Recorder::Options o = fastOptions();
        o.rollDataBytes = 1000 * 8;
        Recorder recorder(o);
        Events ev(recorder);
        const std::vector<float> input = pattern(0, 2500);
        const auto s = recorder.start({dir.path(), QStringLiteral("long"), 48000});
        pushInBlocks(*s, input, 333);
        recorder.finish(s);
        QTRY_VERIFY_WITH_TIMEOUT(ev.finished.contains(s->id()), kTimeout);
        QCOMPARE(ev.closed.size(), size_t(3));
        const QStringList names = {QStringLiteral("long.wav"), QStringLiteral("long_part2.wav"),
                                   QStringLiteral("long_part3.wav")};
        const quint64 counts[] = {1000, 1000, 500};
        std::vector<float> joined;
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(ev.closed[size_t(i)].path, dir.filePath(names[i]));
            QCOMPARE(ev.closed[size_t(i)].frames, counts[i]);
            const std::optional<WavFile::Info> info = WavFile::parseFile(dir.filePath(names[i]));
            QVERIFY(info);
            QCOMPARE(info->frames, counts[i]);
            QCOMPARE(info->framesInFile, counts[i]);
            const std::vector<float> part = readFrames(dir.filePath(names[i]));
            joined.insert(joined.end(), part.begin(), part.end());
        }
        QVERIFY(sameBits(joined, input));
        QCOMPARE(s->framesWritten(), quint64(2500));

        // Exactly full: no empty part after it.
        QTemporaryDir exact;
        const auto t = recorder.start({exact.path(), QStringLiteral("long"), 48000});
        pushInBlocks(*t, pattern(0, 2000), 500);
        recorder.finish(t);
        QTRY_VERIFY_WITH_TIMEOUT(ev.finished.contains(t->id()), kTimeout);
        QCOMPARE(QDir(exact.path()).entryList(QDir::Files),
                 QStringList({QStringLiteral("long.wav"), QStringLiteral("long_part2.wav")}));
    }

    void rollsOnRateChange()
    {
        QTemporaryDir dir;
        Recorder recorder(fastOptions());
        Events ev(recorder);
        const auto first = recorder.start({dir.path(), QStringLiteral("rx"), 48000});
        pushInBlocks(*first, pattern(0, 1000));
        const auto second = recorder.roll(first, 24000);
        QVERIFY(second);
        QVERIFY(second->id() != first->id());
        QCOMPARE(second->sampleRate(), 24000);
        QCOMPARE(second->ringCapacityFrames(), size_t(240000));
        first->push(pattern(0, 10).data(), 10);  // ended: ignored
        pushInBlocks(*second, pattern(1000, 700));
        recorder.finish(second);
        QTRY_VERIFY_WITH_TIMEOUT(ev.finished.contains(second->id()), kTimeout);
        QVERIFY(ev.finished.contains(first->id()));
        QVERIFY(ev.failedIds.isEmpty());

        const std::optional<WavFile::Info> a = WavFile::parseFile(dir.filePath(QStringLiteral("rx.wav")));
        const std::optional<WavFile::Info> b = WavFile::parseFile(dir.filePath(QStringLiteral("rx_part2.wav")));
        QVERIFY(a && b);
        QCOMPARE(a->sampleRate, 48000);
        QCOMPARE(a->frames, quint64(1000));
        QCOMPARE(b->sampleRate, 24000);
        QCOMPARE(b->frames, quint64(700));
        QVERIFY(sameBits(readFrames(dir.filePath(QStringLiteral("rx.wav"))), pattern(0, 1000)));
        QVERIFY(sameBits(readFrames(dir.filePath(QStringLiteral("rx_part2.wav"))), pattern(1000, 700)));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 2);
    }

    void rollBeforeAnyFramesStartsAtTheBaseName()
    {
        QTemporaryDir dir;
        Recorder recorder(fastOptions());
        Events ev(recorder);
        const auto first = recorder.start({dir.path(), QStringLiteral("rx"), 48000});
        const auto second = recorder.roll(first, 24000);
        pushInBlocks(*second, pattern(0, 100));
        recorder.finish(second);
        QTRY_VERIFY_WITH_TIMEOUT(ev.finished.contains(second->id()), kTimeout);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{QStringLiteral("rx.wav")});
        QCOMPARE(WavFile::parseFile(dir.filePath(QStringLiteral("rx.wav")))->sampleRate, 24000);
    }

    void rollOfFailedSessionIsRefused()
    {
        QTemporaryDir dir;
        Recorder::Options o = fastOptions();
        o.openDevice = [](const QString& path) { return std::make_unique<FullDiskFile>(path, 58 + 80); };
        Recorder recorder(o);
        Events ev(recorder);
        const auto s = recorder.start({dir.path(), QStringLiteral("rx"), 48000});
        pushInBlocks(*s, pattern(0, 100));
        QTRY_VERIFY_WITH_TIMEOUT(s->failed(), kTimeout);
        QVERIFY(!recorder.roll(s, 24000));
        QTRY_VERIFY_WITH_TIMEOUT(ev.finished.contains(s->id()), kTimeout);
        QCOMPARE(ev.finished.size(), 1);
    }

    void headerIsPatchedWhileRecording()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("live.wav"));
        Recorder recorder(fastOptions());
        const auto s = recorder.start({dir.path(), QStringLiteral("live"), 48000});
        pushInBlocks(*s, pattern(0, 1000));
        QTRY_VERIFY_WITH_TIMEOUT(WavFile::parseFile(path) && WavFile::parseFile(path)->declaredFrames == 1000,
                                 kTimeout);
        pushInBlocks(*s, pattern(1000, 500));
        QTRY_VERIFY_WITH_TIMEOUT(WavFile::parseFile(path)->declaredFrames == 1500, kTimeout);
        recorder.finish(s);

        // With patches far apart, the open file's header stays behind its data, and parses.
        QTemporaryDir slow;
        Recorder::Options o = fastOptions();
        o.headerIntervalMs = 600000;
        Recorder lazy(o);
        const auto t = lazy.start({slow.path(), QStringLiteral("live"), 48000});
        pushInBlocks(*t, pattern(0, 1000));
        QTRY_VERIFY_WITH_TIMEOUT(t->framesWritten() == 1000, kTimeout);
        const std::optional<WavFile::Info> behind = WavFile::parseFile(slow.filePath(QStringLiteral("live.wav")));
        QVERIFY(behind);
        QCOMPARE(behind->declaredFrames, quint64(0));
        QCOMPARE(behind->framesInFile, quint64(1000));
        lazy.finishAll();
        QCOMPARE(WavFile::parseFile(slow.filePath(QStringLiteral("live.wav")))->frames, quint64(1000));
    }

    void droppedFramesAreCounted()
    {
        QTemporaryDir dir;
        Recorder::Options o = idleOptions();
        o.ringSeconds = 1;
        Recorder recorder(o);
        const auto s = recorder.start({dir.path(), QStringLiteral("stall"), 1000});  // 1000 frames of ring
        QCOMPARE(s->ringCapacityFrames(), size_t(1000));
        pushInBlocks(*s, pattern(0, 1500), 100);  // a stalled disk: nothing drains
        QCOMPARE(s->droppedFrames(), quint64(500));
        recorder.finishAll();
        QCOMPARE(s->framesWritten(), quint64(1000));
        QVERIFY(sameBits(readFrames(dir.filePath(QStringLiteral("stall.wav"))), pattern(0, 1000)));
    }

    void pushAfterFinishIsIgnored()
    {
        QTemporaryDir dir;
        Recorder recorder(idleOptions());
        const auto s = recorder.start({dir.path(), QStringLiteral("a"), 48000});
        pushInBlocks(*s, pattern(0, 100));
        recorder.finish(s);
        pushInBlocks(*s, pattern(100, 100));
        recorder.finishAll();
        QCOMPARE(s->droppedFrames(), quint64(0));
        QCOMPARE(WavFile::parseFile(dir.filePath(QStringLiteral("a.wav")))->frames, quint64(100));
    }

    void finishAllBlocksUntilFilesAreClosed()
    {
        QTemporaryDir dir;
        Recorder recorder(idleOptions());
        const auto a = recorder.start({dir.path(), QStringLiteral("a"), 48000});
        const auto b = recorder.start({dir.path(), QStringLiteral("b"), 24000});
        pushInBlocks(*a, pattern(0, 1000));
        pushInBlocks(*b, pattern(0, 2000));
        recorder.finishAll();  // no event loop runs between this and the checks
        QCOMPARE(WavFile::parseFile(dir.filePath(QStringLiteral("a.wav")))->frames, quint64(1000));
        QCOMPARE(WavFile::parseFile(dir.filePath(QStringLiteral("b.wav")))->frames, quint64(2000));
        // Still usable afterwards.
        const auto c = recorder.start({dir.path(), QStringLiteral("c"), 48000});
        pushInBlocks(*c, pattern(0, 10));
        recorder.finishAll();
        QCOMPARE(WavFile::parseFile(dir.filePath(QStringLiteral("c.wav")))->frames, quint64(10));
    }

    void destructorFinishesRecordings()
    {
        QTemporaryDir dir;
        {
            Recorder recorder(idleOptions());
            const auto s = recorder.start({dir.path(), QStringLiteral("quit"), 48000});
            pushInBlocks(*s, pattern(0, 1234));
        }
        QCOMPARE(WavFile::parseFile(dir.filePath(QStringLiteral("quit.wav")))->frames, quint64(1234));

        Recorder twice(idleOptions());
        twice.shutdown();
        twice.shutdown();
        QVERIFY(!twice.start({dir.path(), QStringLiteral("late"), 48000}));
        QVERIFY(!twice.play(dir.filePath(QStringLiteral("quit.wav"))));
    }

    void unwritableFolderFails()
    {
        QTemporaryDir dir;
        QVERIFY(writeBytes(dir.filePath(QStringLiteral("file")), "x"));
        Recorder recorder(fastOptions());
        Events ev(recorder);
        const auto s = recorder.start({dir.filePath(QStringLiteral("file/sub")), QStringLiteral("a"), 48000});
        pushInBlocks(*s, pattern(0, 100));
        QTRY_VERIFY_WITH_TIMEOUT(ev.finished.contains(s->id()), kTimeout);
        QCOMPARE(ev.failedIds, QList<quint64>{s->id()});
        QVERIFY2(ev.reasons.value(0).contains(QStringLiteral("folder")), qPrintable(ev.reasons.value(0)));
        QVERIFY(ev.opened.isEmpty());
    }

    // The Windows disk-full seam: a device that stops partway through a write.
    void injectedDeviceFailingMidWrite()
    {
        QTemporaryDir dir;
        Recorder::Options o = fastOptions();
        o.openDevice = [](const QString& path) { return std::make_unique<FullDiskFile>(path, 58 + 1000 * 8 + 5); };
        Recorder recorder(o);
        Events ev(recorder);
        const auto s = recorder.start({dir.path(), QStringLiteral("full"), 48000});
        pushInBlocks(*s, pattern(0, 3000));
        QTRY_VERIFY_WITH_TIMEOUT(ev.finished.contains(s->id()), kTimeout);

        const QString path = dir.filePath(QStringLiteral("full.wav"));
        QCOMPARE(ev.failedIds, QList<quint64>{s->id()});
        QVERIFY2(ev.reasons.value(0).contains(QStringLiteral("not enough space")),
                 qPrintable(ev.reasons.value(0)));
        QVERIFY(ev.reasons.value(0).contains(QStringLiteral("full.wav")));
        QVERIFY(s->failed());
        QCOMPARE(s->framesWritten(), quint64(1000));
        QCOMPARE(ev.closed.size(), size_t(1));
        QCOMPARE(ev.closed[0].frames, quint64(1000));
        QCOMPARE(QFileInfo(path).size(), qint64(58 + 1000 * 8));
        const std::optional<WavFile::Info> info = WavFile::parseFile(path);
        QVERIFY(info);
        QCOMPARE(info->declaredFrames, quint64(1000));
        QCOMPARE(info->framesInFile, quint64(1000));
        QVERIFY(sameBits(readFrames(path), pattern(0, 1000)));

        // The engine keeps pushing until it hears: harmless, and nothing more is counted.
        const quint64 dropped = s->droppedFrames();
        pushInBlocks(*s, pattern(3000, 960000));
        QCOMPARE(s->droppedFrames(), dropped);
    }

    void injectedDeviceFailingFirstWrite()
    {
        QTemporaryDir dir;
        Recorder::Options o = fastOptions();
        qint64 capacity = 58;  // the header fits, no frame does
        o.openDevice = [&capacity](const QString& path) { return std::make_unique<FullDiskFile>(path, capacity); };
        Recorder recorder(o);
        Events ev(recorder);
        const auto s = recorder.start({dir.path(), QStringLiteral("full"), 48000});
        pushInBlocks(*s, pattern(0, 100));
        QTRY_VERIFY_WITH_TIMEOUT(ev.finished.contains(s->id()), kTimeout);
        QCOMPARE(ev.failedIds, QList<quint64>{s->id()});
        QVERIFY(ev.closed.empty());
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());  // nothing worth keeping
        recorder.finishAll();

        capacity = 20;  // not even the header
        const auto t = recorder.start({dir.path(), QStringLiteral("full"), 48000});
        pushInBlocks(*t, pattern(0, 100));
        QTRY_VERIFY_WITH_TIMEOUT(ev.finished.contains(t->id()), kTimeout);
        QVERIFY(ev.failedIds.contains(t->id()));
        QVERIFY2(ev.reasons.last().contains(QStringLiteral("Could not create")), qPrintable(ev.reasons.last()));
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
    }

    // POSIX: a real short write from the kernel, in a child limited to a file size.
    void diskFullInChildProcess()
    {
#ifndef Q_OS_UNIX
        QSKIP("Needs RLIMIT_FSIZE; injectedDeviceFailingMidWrite covers this path on Windows");
#else
        QTemporaryDir dir;
        const qint64 limit = 58 + 48000 * 8 + 5;  // one second and five bytes: it ends mid-frame
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(),
                    {QLatin1String(kChildFlag), QStringLiteral("diskfull"), dir.path(), QString::number(limit)});
        QVERIFY(child.waitForFinished(30000));
        const QByteArray out = child.readAllStandardOutput();
        const QByteArray err = child.readAllStandardError();
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, (out + err).constData());
        const QMap<QString, QString> kv = keyValues(out);

        const QString path = kv.value(QStringLiteral("path"));
        QCOMPARE(path, dir.filePath(QStringLiteral("full.wav")));
        QVERIFY2(!kv.value(QStringLiteral("reason")).isEmpty(), out.constData());
        QCOMPARE(kv.value(QStringLiteral("failedFlag")), QStringLiteral("1"));
        QCOMPARE(kv.value(QStringLiteral("written")), QStringLiteral("48000"));
        QCOMPARE(kv.value(QStringLiteral("dropped")), QStringLiteral("0"));
        QVERIFY(kv.value(QStringLiteral("playedAfterFailure")).toLongLong() >= 48000);  // live audio went on
        qInfo("child reported: %s", qPrintable(kv.value(QStringLiteral("reason"))));

        QCOMPARE(QFileInfo(path).size(), qint64(58 + 48000 * 8));
        const std::optional<WavFile::Info> info = WavFile::parseFile(path);
        QVERIFY(info);
        QCOMPARE(info->declaredFrames, quint64(48000));
        QCOMPARE(info->framesInFile, quint64(48000));
        QVERIFY(sameBits(readFrames(path), pattern(0, 48000)));
#endif
    }

    void killedWriterLeavesParseableFile_data()
    {
        QTest::addColumn<int>("drainMs");
        QTest::addColumn<int>("headerMs");
        QTest::addColumn<int>("killAfterMs");
        QTest::newRow("shipping cadence") << 200 << 1000 << 2600;
        QTest::newRow("patching every 20 ms, a") << 5 << 20 << 600;
        QTest::newRow("patching every 20 ms, b") << 5 << 20 << 777;
        QTest::newRow("patching every 7 ms") << 2 << 7 << 913;
    }

    // SIGKILL on POSIX, TerminateProcess on Windows: the writer gets no chance to tidy up.
    void killedWriterLeavesParseableFile()
    {
        QFETCH(int, drainMs);
        QFETCH(int, headerMs);
        QFETCH(int, killAfterMs);
        QTemporaryDir dir;
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(),
                    {QLatin1String(kChildFlag), QStringLiteral("forever"), dir.path(), QString::number(drainMs),
                     QString::number(headerMs)});
        QVERIFY(child.waitForStarted(kTimeout));
        const QString path = waitForLine(child, "opened=", kTimeout);
        QVERIFY2(!path.isEmpty(), child.readAllStandardError().constData());
        QTest::qWait(killAfterMs);
        QCOMPARE(child.state(), QProcess::Running);
        child.kill();
        QVERIFY(child.waitForFinished(kTimeout));

        QString error;
        const std::optional<WavFile::Info> info = WavFile::parseFile(path, &error);
        QVERIFY2(info, qPrintable(error));
        QCOMPARE(info->sampleRate, 48000);
        QVERIFY(info->declaredFrames > 0);  // patched at least once
        QVERIFY(info->declaredFrames <= info->framesInFile);
        // Everything on disk is the recording, in order, including what the header had not
        // caught up with.
        QVERIFY(sameBits(framesOnDisk(path), pattern(0, size_t(info->framesInFile))));
        // What a crash loses from the declared length: about one header interval of audio.
        const quint64 behind = info->framesInFile - info->declaredFrames;
        qInfo("declared %llu of %llu frames on disk; %llu behind", (unsigned long long)info->declaredFrames,
              (unsigned long long)info->framesInFile, (unsigned long long)behind);
        QVERIFY2(behind <= quint64(48) * quint64(headerMs + 2 * drainMs + 500),
                 qPrintable(QString::number(behind)));
    }

    // ── RecordingPlayer ─────────────────────────────────────────────────────

    void playerStreamsExactlyTheFilesFrames()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.wav"));
        const std::vector<float> input = randomFrames(100003, 7);
        QVERIFY(writeBytes(path, WavFile::header(48000, 100003) + bytesOf(input)));

        // A ring much smaller than the file, so it wraps many times.
        auto session = std::make_shared<PlaySession>(1, path, 4096);
        RecordingPlayer player(session, 48000);
        QVERIFY(player.open());
        QVERIFY(session->ready());
        QCOMPARE(session->sampleRate(), 48000);
        QCOMPARE(session->totalFrames(), quint64(100003));
        QCOMPARE(session->ring().size(), size_t(4096));  // primed before ready
        std::vector<float> got;
        std::vector<float> buf(1000 * 2);
        bool more = true;
        while (more || session->ring().size() > 0) {
            const size_t n = session->ring().read(buf.data(), 1000);
            got.insert(got.end(), buf.begin(), buf.begin() + qsizetype(n) * 2);
            if (more) more = player.fill();
        }
        QVERIFY(session->atEnd());
        QVERIFY(!session->failed());
        QCOMPARE(session->framesQueued(), quint64(100003));
        QVERIFY(sameBits(got, input));
    }

    void playerPlaysOnlyDeclaredFrames()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.wav"));
        QVERIFY(writeBytes(path, WavFile::header(24000, 1000) + bytesOf(pattern(0, 1500))));
        auto session = std::make_shared<PlaySession>(1, path, 48000);
        RecordingPlayer player(session, 0);
        QVERIFY(player.open());
        QVERIFY(session->atEnd());  // it all fitted at once
        QCOMPARE(session->totalFrames(), quint64(1000));
        std::vector<float> got(2000 * 2);
        got.resize(session->ring().read(got.data(), 2000) * 2);
        QVERIFY(sameBits(got, pattern(0, 1000)));

        // An empty recording plays nothing, and ends at once.
        QVERIFY(writeBytes(path, WavFile::header(48000, 0)));
        auto empty = std::make_shared<PlaySession>(2, path, 48000);
        RecordingPlayer none(empty, 48000);
        QVERIFY(none.open());
        QVERIFY(empty->ready() && empty->atEnd());
        QCOMPARE(empty->ring().size(), size_t(0));
    }

    void playerRefusesRateMismatch()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.wav"));
        QVERIFY(writeBytes(path, WavFile::header(24000, 10) + bytesOf(pattern(0, 10))));
        auto session = std::make_shared<PlaySession>(1, path, 48000);
        RecordingPlayer player(session, 48000);
        QVERIFY(!player.open());
        QVERIFY(session->failed());
        QVERIFY(!session->ready());
        QVERIFY2(session->failureReason().contains(QStringLiteral("24000")), qPrintable(session->failureReason()));
        QCOMPARE(session->ring().size(), size_t(0));
    }

    void playerRefusesUnreadableFiles()
    {
        QTemporaryDir dir;
        auto missing = std::make_shared<PlaySession>(1, dir.filePath(QStringLiteral("none.wav")), 48000);
        QVERIFY(!RecordingPlayer(missing, 0).open());
        QVERIFY(missing->failed());
        QVERIFY(!missing->failureReason().isEmpty());

        const QString pcm = dir.filePath(QStringLiteral("pcm.wav"));
        QVERIFY(writeBytes(pcm, riff(chunk("fmt ", fmtBody(1, 2, 48000, 16))
                                     + chunk("data", QByteArray(16, '\0')))));
        auto wrong = std::make_shared<PlaySession>(2, pcm, 48000);
        QVERIFY(!RecordingPlayer(wrong, 0).open());
        QVERIFY(wrong->failed());
    }

    void playerStopsWhenCancelled()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.wav"));
        QVERIFY(writeBytes(path, WavFile::header(48000, 20000) + bytesOf(pattern(0, 20000))));
        auto session = std::make_shared<PlaySession>(1, path, 1000);
        RecordingPlayer player(session, 48000);
        QVERIFY(player.open());
        std::vector<float> buf(1000 * 2);
        session->ring().read(buf.data(), 1000);
        session->cancel();
        QVERIFY(!player.fill());
        QCOMPARE(session->ring().size(), size_t(0));
        QVERIFY(!session->atEnd());
        QVERIFY(!session->failed());
    }

    void recorderPlaysOnItsThreadAndStops()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.wav"));
        const std::vector<float> input = pattern(0, 300000);  // longer than the 2 s ring
        QVERIFY(writeBytes(path, WavFile::header(48000, 300000) + bytesOf(input)));
        Recorder recorder(fastOptions());
        Events ev(recorder);
        const auto play = recorder.play(path, 48000);
        QCOMPARE(play->ring().capacity(), size_t(96000));
        QVERIFY(sameBits(drainPlay(*play), input));
        QVERIFY(play->atEnd());
        QVERIFY(ev.playFailedIds.isEmpty());

        // Stopped partway: the ring stops filling.
        const auto again = recorder.play(path, 48000);
        QTRY_VERIFY_WITH_TIMEOUT(again->ready(), kTimeout);
        recorder.stopPlaying(again);
        QTest::qWait(50);
        again->ring().skip(again->ring().size());
        QTest::qWait(50);
        QCOMPARE(again->ring().size(), size_t(0));
        QVERIFY(!again->atEnd());
        QVERIFY(again->cancelled());
    }

    void recorderReportsPlaybackFailure()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.wav"));
        QVERIFY(writeBytes(path, WavFile::header(24000, 10) + bytesOf(pattern(0, 10))));
        Recorder recorder(fastOptions());
        Events ev(recorder);
        const auto play = recorder.play(path, 48000);
        QTRY_VERIFY_WITH_TIMEOUT(ev.playFailedIds.contains(play->id()), kTimeout);
        QVERIFY(play->failed());
        QVERIFY(!play->ready());
        QCOMPARE(ev.reasons.value(0), play->failureReason());
    }

    void pacerDeliversFramesByElapsedTime()
    {
        constexpr qint64 kMs = 1000000;
        constexpr qint64 kPlenty = std::numeric_limits<qint64>::max();
        PlayPacer p;
        p.start(48000, 5 * kMs);
        QCOMPARE(p.take(4 * kMs, kPlenty), qint64(0));  // before the start
        QCOMPARE(p.take(15 * kMs, kPlenty), qint64(480));
        QCOMPARE(p.take(25 * kMs, kPlenty), qint64(480));
        // A ring that ran dry costs a gap, not a rush afterwards.
        QCOMPARE(p.take(35 * kMs, 100), qint64(100));
        QCOMPARE(p.take(45 * kMs, kPlenty), qint64(480));
        // A stalled engine thread gets at most a second.
        QCOMPARE(p.take(5045 * kMs, kPlenty), qint64(48000));
        QCOMPARE(p.taken(), qint64(480 * 3 + 100 + 48000));

        // No drift from rounding over a long run at an awkward tick: 100000 ticks of 10.4 ms.
        PlayPacer q;
        q.start(24000, 7);
        for (qint64 i = 1; i <= 100000; ++i) q.take(7 + i * 10400000, kPlenty);
        QCOMPARE(q.taken(), qint64(1040) * 24000);
    }

    // ── The app, headless ───────────────────────────────────────────────────

    void headlessRecordRecordsTheChainUntilTheDurationEnds()
    {
        MockExpert mock;
        QVERIFY(mock.listen());
        QVERIFY(mock.port() > 0);
        QTemporaryDir dir;
        const QString folder = dir.filePath(QStringLiteral("Recordings"));
        QProcess child;
        child.start(QStringLiteral(ESA_APP_BINARY), headlessArgs(mock, dir)
                                                        << QStringLiteral("--duration") << QStringLiteral("3")
                                                        << QStringLiteral("--record") << folder);
        QVERIFY2(child.waitForStarted(kTimeout), qPrintable(child.errorString()));
        QVERIFY(waitForExit(child, 60000));
        const QByteArray out = child.readAllStandardOutput();
        const QByteArray err = child.readAllStandardError();
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, (out + err).constData());
        QVERIFY2(!sanitizerSpoke(err), err.constData());

        const QJsonObject summary = summaryOf(out);
        QCOMPARE(summary.value(QStringLiteral("connection")).toString(), QStringLiteral("ready"));
        const QJsonObject rec = summary.value(QStringLiteral("recording")).toObject();
        qInfo("summary's recording: %s", QJsonDocument(rec).toJson(QJsonDocument::Compact).constData());
        const QString path = rec.value(QStringLiteral("path")).toString();
        QCOMPARE(QFileInfo(path).absolutePath(), QDir(folder).absolutePath());
        QVERIFY2(QRegularExpression(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}_\\d{6}Z_7\\.160MHz_LSB\\.wav$"))
                     .match(QFileInfo(path).fileName())
                     .hasMatch(),
                 qPrintable(path));
        QVERIFY2(rec.value(QStringLiteral("error")).toString().isEmpty(), out.constData());
        QCOMPARE(rec.value(QStringLiteral("droppedFrames")).toDouble(), 0.0);
        const double seconds = rec.value(QStringLiteral("seconds")).toDouble();
        QVERIFY2(seconds > 1.0 && seconds < 3.5, qPrintable(QString::number(seconds)));

        // Finished, and what the summary says it holds.
        const std::optional<WavFile::Info> info = WavFile::parseFile(path);
        QVERIFY(info);
        QCOMPARE(info->sampleRate, 48000);
        QCOMPARE(info->declaredFrames, info->framesInFile);
        QCOMPARE(seconds, double(info->declaredFrames) / 48000.0);
        const std::vector<float> frames = readFrames(path);
        float peak = 0.0f;
        for (float v : frames) peak = std::max(peak, std::fabs(v));
        QVERIFY2(peak > 0.1f && peak <= 1.0f, qPrintable(QString::number(peak)));  // the mock's tone
        QVERIFY2(radioChangingCommands(mock.commands).isEmpty(), qPrintable(mock.commands.join(u' ')));
    }

    // REC in the window is the button; --record belongs to headless runs.
    void recordNeedsHeadless()
    {
        QTemporaryDir dir;
        QProcess child;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        // Without --headless it starts as the window does, and on Windows a window program's
        // messages go to the debugger rather than to stderr unless Qt is told otherwise.
        env.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
        child.setProcessEnvironment(env);
        child.start(QStringLiteral(ESA_APP_BINARY),
                    {QStringLiteral("--settings"), dir.filePath(QStringLiteral("settings.ini")), QStringLiteral("--record"),
                     dir.filePath(QStringLiteral("Recordings"))});
        QVERIFY(child.waitForFinished(30000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 2);
        QVERIFY2(child.readAllStandardError().contains("--record needs --headless"), "no reason given");
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("Recordings"))));
    }

    // SIGKILL on POSIX, TerminateProcess on Windows, in the middle of a headless --record.
    void killedHeadlessRecordLeavesAParseableFile()
    {
        MockExpert mock;
        QVERIFY(mock.listen());
        QVERIFY(mock.port() > 0);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("take.wav"));
        QProcess child;
        child.start(QStringLiteral(ESA_APP_BINARY), headlessArgs(mock, dir) << QStringLiteral("--record") << path);
        QVERIFY2(child.waitForStarted(kTimeout), qPrintable(child.errorString()));
        // A second on disk and in the header, then most of the way to the next header patch.
        QVERIFY2(QTest::qWaitFor([&] {
                     const std::optional<WavFile::Info> i = WavFile::parseFile(path);
                     return i && i->declaredFrames >= 48000;
                 }, 30000),
                 child.readAllStandardError().constData());
        QTest::qWait(700);
        QCOMPARE(child.state(), QProcess::Running);
        child.kill();
        QVERIFY(waitForExit(child, kTimeout));
        QCOMPARE(child.exitStatus(), QProcess::CrashExit);
        const QByteArray err = child.readAllStandardError();
        QVERIFY2(!sanitizerSpoke(err), err.constData());

        QString error;
        const std::optional<WavFile::Info> info = WavFile::parseFile(path, &error);
        QVERIFY2(info, qPrintable(error));
        QCOMPARE(info->sampleRate, 48000);
        QVERIFY(info->declaredFrames >= 48000);
        QVERIFY(info->declaredFrames <= info->framesInFile);
        const quint64 behind = info->framesInFile - info->declaredFrames;
        qInfo("declared %llu of %llu frames on disk; %llu behind", (unsigned long long)info->declaredFrames,
              (unsigned long long)info->framesInFile, (unsigned long long)behind);
        QVERIFY2(behind <= quint64(48) * quint64(1000 + 2 * 200 + 500), qPrintable(QString::number(behind)));
        // Every frame on disk is audio, the header's or not.
        const std::vector<float> frames = framesOnDisk(path);
        QCOMPARE(frames.size(), size_t(info->framesInFile) * 2);
        float peak = 0.0f;
        for (float v : frames) {
            QVERIFY(std::isfinite(v));
            peak = std::max(peak, std::fabs(v));
        }
        QVERIFY2(peak > 0.1f && peak <= 1.0f, qPrintable(QString::number(peak)));
    }

    // --capture on the recorder: both sides, and a rate change goes on in _part2.
    void headlessCaptureWritesBothSidesAndSurvivesARateChange()
    {
        MockExpert mock;
        QVERIFY(mock.listen());
        QVERIFY(mock.port() > 0);
        QTemporaryDir dir;
        const QString base = dir.filePath(QStringLiteral("cap"));
        QProcess child;
        child.start(QStringLiteral(ESA_APP_BINARY), headlessArgs(mock, dir)
                                                        << QStringLiteral("--duration") << QStringLiteral("3")
                                                        << QStringLiteral("--capture") << base);
        QVERIFY2(child.waitForStarted(kTimeout), qPrintable(child.errorString()));
        QVERIFY(QTest::qWaitFor([&] { return mock.blocksSent > 60; }, 30000));
        mock.operatorSetStreamRate(24000);
        QVERIFY(waitForExit(child, 60000));
        const QByteArray out = child.readAllStandardOutput();
        const QByteArray err = child.readAllStandardError();
        QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, (out + err).constData());
        QVERIFY2(!sanitizerSpoke(err), err.constData());

        const QJsonObject capture = summaryOf(out).value(QStringLiteral("capture")).toObject();
        QVERIFY2(capture.value(QStringLiteral("error")).toString().isEmpty(), out.constData());
        QStringList files;
        for (const QJsonValue& v : capture.value(QStringLiteral("files")).toArray()) files << v.toString();
        files.sort();
        const QStringList expected = {base + QStringLiteral(".after.wav"), base + QStringLiteral(".after_part2.wav"),
                                      base + QStringLiteral(".before.wav"), base + QStringLiteral(".before_part2.wav")};
        QCOMPARE(files, expected);
        const QList<int> rates = {48000, 24000, 48000, 24000};
        for (qsizetype i = 0; i < files.size(); ++i) {
            const std::optional<WavFile::Info> info = WavFile::parseFile(files[i]);
            QVERIFY2(info, qPrintable(files[i]));
            QCOMPARE(info->sampleRate, rates[i]);
            QVERIFY(info->declaredFrames > 0);
            QCOMPARE(info->declaredFrames, info->framesInFile);
        }
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc >= 3 && std::strcmp(argv[1], kChildFlag) == 0) return runChild(app.arguments().mid(2));
    TestRecorder test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "test_recorder.moc"
