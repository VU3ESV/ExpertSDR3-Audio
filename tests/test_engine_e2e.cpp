#include "MockExpert.h"

#include "app/Engine.h"
#include "app/WavFile.h"
#include "audio/AudioSink.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QHostAddress>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

using namespace esa;

namespace {

// Records what would have reached the speakers. Also a stand-in for an unpluggable
// device, which no real test machine offers on demand.
class RecordingSink final : public AudioSink {
public:
    ~RecordingSink() override
    {
        if (onClose) onClose(*this);
    }
    QString deviceName() const override { return QStringLiteral("recording"); }
    void write(const float* stereo, int frames) override
    {
        samples.insert(samples.end(), stereo, stereo + qsizetype(frames) * 2);
        framesWritten += frames;
    }
    bool alive() const override { return isAlive; }
    SinkStats stats() override
    {
        SinkStats s;
        s.framesPlayed = framesWritten;
        s.targetMs = targetMs;
        return s;
    }
    void setGainDb(double) override {}
    void setBoost(bool) override {}
    void setMuted(bool) override {}
    void setTargetLatencyMs(int) override {}

    std::vector<float> samples;
    quint64 framesWritten{0};
    bool isAlive{true};
    double targetMs{0.0};  // what a real device's buffer aims to hold, raised by underruns
    std::function<void(const RecordingSink&)> onClose;  // the engine let go of it
};

// Counts frames from whichever thread writes them, for tests where the engine has its own.
class CountingSink final : public AudioSink {
public:
    explicit CountingSink(std::shared_ptr<std::atomic<quint64>> frames) : m_frames(std::move(frames)) {}
    QString deviceName() const override { return QStringLiteral("counting"); }
    void write(const float*, int frames) override { m_frames->fetch_add(quint64(frames)); }
    bool alive() const override { return true; }
    SinkStats stats() override { return {}; }
    void setGainDb(double) override {}
    void setBoost(bool) override {}
    void setMuted(bool) override {}
    void setTargetLatencyMs(int) override {}

private:
    std::shared_ptr<std::atomic<quint64>> m_frames;
};

float peakOf(const std::vector<float>& samples)
{
    float peak = 0.0f;
    for (float v : samples) peak = std::max(peak, std::fabs(v));
    return peak;
}

std::vector<float> drain(SpscStereoRing& ring)
{
    std::vector<float> stereo(ring.size() * 2);
    stereo.resize(ring.read(stereo.data(), stereo.size() / 2) * 2);
    return stereo;
}

// The strongest bin of the left channel, Hann-windowed, by Goertzel: the same question
// the EQ's analyser asks of these samples.
int strongestBin(const std::vector<float>& stereo, int points)
{
    constexpr double kPi = 3.14159265358979323846;
    std::vector<double> windowed(static_cast<size_t>(points));
    for (int i = 0; i < points; ++i) {
        windowed[size_t(i)] = stereo[size_t(i) * 2] * 0.5 * (1.0 - std::cos(2.0 * kPi * i / (points - 1)));
    }
    int best = 0;
    double bestPower = -1.0;
    for (int bin = 1; bin < points / 2; ++bin) {
        const double coeff = 2.0 * std::cos(2.0 * kPi * bin / points);
        double s1 = 0.0;
        double s2 = 0.0;
        for (double x : windowed) {
            const double s0 = x + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        if (power > bestPower) {
            bestPower = power;
            best = bin;
        }
    }
    return best;
}

// Every stage meter at rest: what a stage that did not run shows.
bool metersAtRest(const StageSet& view)
{
    return view.gate.inputPeakDb() == -120.0f && view.gate.outputPeakDb() == -120.0f
           && view.gate.gainReductionDb() == 0.0f && view.comp.inputPeakDb() == -120.0f
           && view.comp.outputPeakDb() == -120.0f && view.comp.gainReductionDb() == 0.0f
           && view.tube.inputPeakDb() == -120.0f && view.tube.outputPeakDb() == -120.0f
           && view.pudu.inputPeakDb() == -120.0f && view.pudu.outputPeakDb() == -120.0f;
}

bool metersLive(const StageSet& view)
{
    return view.gate.inputPeakDb() > -40.0f && view.comp.inputPeakDb() > -40.0f
           && view.tube.inputPeakDb() > -40.0f && view.pudu.inputPeakDb() > -40.0f;
}

// Runs Engine on the test thread, so the test can inspect state without a thread hop:
// the event loop still drives everything.
class Fixture {
public:
    Fixture() : settingsPath(dir.filePath(QStringLiteral("settings.ini")))
    {
        mock.listen();
    }

    Engine& engine()
    {
        if (!m_engine) {
            m_engine = std::make_unique<Engine>(
                settingsPath,
                [this](const AppConfig&, QString* error) -> std::unique_ptr<AudioSink> {
                    ++sinkOpenCount;
                    if (failSinkOpen) {
                        if (error) *error = QStringLiteral("no such device");
                        return nullptr;
                    }
                    auto created = std::make_unique<RecordingSink>();
                    created->onClose = [this](const RecordingSink& closed) {
                        ++sinkCloseCount;
                        closedSamples = closed.samples;
                    };
                    sink = created.get();
                    return created;
                },
                nullptr, recorderOptions);
            QObject::connect(m_engine.get(), &Engine::statusChanged, &mock,
                             [this](const EngineStatus& s) { status = s; });
        }
        return *m_engine;
    }

    AppConfig baseConfig() const
    {
        AppConfig config;
        config.host = QStringLiteral("127.0.0.1");
        config.port = mock.port();
        // Never the defaults: one of them is where a real ExpertSDR3 may be listening.
        config.fallbackPorts = {};
        config.muteExpert = true;
        return config;
    }

    void start(const AppConfig& config)
    {
        engine().configure(config);
        engine().connectRadio();
    }
    void start() { start(baseConfig()); }

    bool audioFlowing() const { return status.audioFlowing; }

    QTemporaryDir dir;
    QString settingsPath;
    MockExpert mock;
    RecordingSink* sink{nullptr};
    bool failSinkOpen{false};
    int sinkOpenCount{0};
    int sinkCloseCount{0};
    std::vector<float> closedSamples;  // what the last sink closed had received
    Recorder::Options recorderOptions;  // set before the first engine() call
    EngineStatus status;
    std::unique_ptr<Engine> m_engine;

};

constexpr int kTimeout = 5000;

// The native store is this user's own (~/Library/Preferences on a Mac, the registry on Windows,
// ~/.config elsewhere), and on a Mac neither HOME nor CFFIXED_USER_HOME moves it. So the rows
// that use it run only where that was asked for, as CI does: ESA_TEST_NATIVE_SETTINGS=1.
bool nativeStoreWanted()
{
    return qEnvironmentVariableIntValue("ESA_TEST_NATIVE_SETTINGS") == 1;
}

// One settings store for one test: an ini file like the other tests use, or the native store
// that real installs use (a plist, the registry), under a name no real install has.
class SettingsStore {
public:
    explicit SettingsStore(bool native) : m_native(native)
    {
        // One name for every run, so runs at once (ctest -j, two build folders) take turns with
        // it, or one would clear what another has just written. A run that died leaves a lock
        // that the next sees is stale, by its process being gone.
        if (m_native) {
            m_lock = std::make_unique<QLockFile>(
                QDir(QDir::tempPath()).filePath(QStringLiteral("ExpertSDR3Audio-Tests-SettingsStore.lock")));
            m_lock->setStaleLockTime(0);
            m_lock->lock();
        }
        clear();
    }
    // The native store keeps an empty entry under this fixed name. Deleting that file as well would
    // only race a Mac's preferences daemon, which writes it back. The lock goes after this.
    ~SettingsStore() { clear(); }

    std::unique_ptr<QSettings> raw() const
    {
        return m_native ? std::make_unique<QSettings>(kOrganization, kApplication)
                        : std::make_unique<QSettings>(m_dir.filePath(QStringLiteral("settings.ini")),
                                                      QSettings::IniFormat);
    }
    std::unique_ptr<Settings> open() const
    {
        return m_native ? std::make_unique<Settings>(kOrganization, kApplication)
                        : std::make_unique<Settings>(m_dir.filePath(QStringLiteral("settings.ini")));
    }
    // Writes what v0.1.0 left behind: one record, naming no radio.
    void writeOldMuteRecord(bool owned, bool priorMuted) const
    {
        const auto store = raw();
        store->setValue(QStringLiteral("muteOwnership/owned"), owned);
        store->setValue(QStringLiteral("muteOwnership/priorMuted"), priorMuted);
    }
    // Macs add global keys to allKeys(), so only ours count.
    QStringList muteKeys() const
    {
        return raw()->allKeys().filter(QRegularExpression(QStringLiteral("^muteOwnership")));
    }

private:
    void clear() const
    {
        const auto store = raw();
        store->clear();
        store->sync();
    }

    inline static const QString kOrganization = QStringLiteral("ExpertSDR3Audio-Tests");
    inline static const QString kApplication = QStringLiteral("SettingsStore");
    bool m_native;
    std::unique_ptr<QLockFile> m_lock;
    QTemporaryDir m_dir;
};

void addEachStore()
{
    QTest::addColumn<bool>("native");
    QTest::newRow("ini file") << false;
    QTest::newRow("native store") << true;
}

// A disk that takes its time over every write, as a busy or network one can.
class SlowFile : public QFile {
public:
    SlowFile(const QString& path, int delayMs) : QFile(path), m_delayMs(delayMs) {}

protected:
    qint64 writeData(const char* data, qint64 len) override
    {
        QThread::msleep(static_cast<unsigned long>(m_delayMs));
        return QFile::writeData(data, len);
    }

private:
    const int m_delayMs;
};

// A disk that fills up at `capacity` bytes, the first write past it stopping short.
class FullDiskFile : public QFile {
public:
    FullDiskFile(const QString& path, qint64 capacity) : QFile(path), m_capacity(capacity) {}

protected:
    qint64 writeData(const char* data, qint64 len) override
    {
        const qint64 allowed = std::clamp<qint64>(m_capacity - pos(), 0, len);
        if (allowed == 0) {
            setErrorString(QStringLiteral("No space left on device"));
            return -1;
        }
        return QFile::writeData(data, allowed);
    }

private:
    const qint64 m_capacity;
};

// A disk that refuses the audio once `failing` is set; the header, at the start, still goes in.
class FailingFile : public QFile {
public:
    FailingFile(const QString& path, std::shared_ptr<std::atomic<bool>> failing)
        : QFile(path), m_failing(std::move(failing))
    {
    }

protected:
    qint64 writeData(const char* data, qint64 len) override
    {
        if (m_failing->load() && pos() >= WavFile::kHeaderBytes) {
            setErrorString(QStringLiteral("injected failure"));
            return -1;
        }
        return QFile::writeData(data, len);
    }

private:
    const std::shared_ptr<std::atomic<bool>> m_failing;
};

// A recording as a player sees it: its header, and the frames it declares.
struct Recorded {
    WavFile::Info info;
    std::vector<float> frames;
};

std::optional<Recorded> readRecording(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    const std::optional<WavFile::Info> info = WavFile::parse(file);
    if (!info) return std::nullopt;
    Recorded r{*info, std::vector<float>(size_t(info->frames) * 2)};
    const qint64 bytes = qint64(r.frames.size() * sizeof(float));
    if (file.read(reinterpret_cast<char*>(r.frames.data()), bytes) != bytes) return std::nullopt;
    return r;
}

bool sameBits(const float* a, const float* b, size_t count)
{
    return std::memcmp(a, b, count * sizeof(float)) == 0;
}

// Commands that would change what the radio does or how it sounds. VOLUME also clears MUTE on
// ExpertSDR3 (docs/TCI-NOTES.md), which is why REC and PLAY must never send one.
QStringList radioChangingCommands(const QStringList& commands)
{
    QStringList found;
    for (const QString& c : commands) {
        if (c.startsWith(QLatin1String("trx")) || c.startsWith(QLatin1String("volume"))
            || c.startsWith(QLatin1String("mute")) || c.startsWith(QLatin1String("rx_volume"))
            || c.startsWith(QLatin1String("rx_mute"))) {
            found << c;
        }
    }
    return found;
}

// Records `seconds` of the mock's audio into `path` and waits until PLAY has it.
bool recordFor(Fixture& f, const QString& path, double seconds)
{
    f.engine().startRecordingToFile(path);
    if (!f.status.recording.active) return false;
    if (!QTest::qWaitFor([&] { return f.status.recording.seconds >= seconds; }, kTimeout)) return false;
    f.engine().stopRecording();
    return QTest::qWaitFor([&] { return f.status.lastRecordingPath == path; }, kTimeout);
}

} // namespace

class TestEngineE2E : public QObject {
    Q_OBJECT

private slots:
    void requestsTheStreamItNeedsAndPlaysIt()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.audioStartCount() == 1, kTimeout);
        QVERIFY(f.mock.commands.contains(QStringLiteral("audio_samplerate:48000")));
        QVERIFY(f.mock.commands.contains(QStringLiteral("audio_stream_sample_type:float32")));
        QVERIFY(f.mock.commands.contains(QStringLiteral("audio_stream_channels:2")));
        QVERIFY(f.mock.commands.contains(QStringLiteral("audio_stream_samples:1024")));
        QVERIFY(f.mock.commands.contains(QStringLiteral("audio_start:0")));

        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        QCOMPARE(f.status.serverName, QStringLiteral("ExpertSDR3"));
        QCOMPARE(f.status.device, QStringLiteral("ColibriDDC"));
        QVERIFY(f.status.receiveOnly);
        QCOMPARE(f.status.trxCount, 2);
        QCOMPARE(f.status.frequencyHz, 7160000);
        QCOMPARE(f.status.modulation, QStringLiteral("LSB"));
        f.engine().shutdown();
    }

    void mutesOnlyAfterAudioIsFlowingAndRestoresOnShutdown()
    {
        Fixture f;
        f.mock.sendAudio = false;  // the stream is slow to start
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.audioStartCount() == 1, kTimeout);
        QTest::qWait(300);
        // Connected but silent: muting now would leave the operator with nothing.
        QCOMPARE(f.mock.muteCommandsSent(true), 0);
        QVERIFY(!f.mock.muted);

        f.mock.sendAudio = true;
        f.mock.operatorRestartDsp();  // `start;` makes the client ask for audio again
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        QCOMPARE(f.mock.muteCommandsSent(true), 1);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.mutePhase, MuteController::Phase::Holding, kTimeout);

        f.engine().shutdown();
        QVERIFY(!f.mock.muted);
        QCOMPARE(f.mock.muteCommandsSent(false), 1);
    }

    void leavesAnAlreadyMutedRadioMuted()
    {
        Fixture f;
        f.mock.muted = true;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 0, kTimeout);
        QTest::qWait(200);
        f.engine().shutdown();
        QCOMPARE(f.mock.muteCommandsSent(true), 0);
        QCOMPARE(f.mock.muteCommandsSent(false), 0);
        QVERIFY(f.mock.muted);
    }

    void optionOffNeverTouchesMute()
    {
        Fixture f;
        AppConfig config = f.baseConfig();
        config.muteExpert = false;
        f.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 0, kTimeout);
        QTest::qWait(200);
        f.engine().shutdown();
        QVERIFY(f.mock.commands.filter(QStringLiteral("mute")).isEmpty());
    }

    void togglingTheOptionMutesAndRestoresLive()
    {
        Fixture f;
        AppConfig config = f.baseConfig();
        config.muteExpert = false;
        f.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 0, kTimeout);
        QVERIFY(!f.mock.muted);

        f.engine().setMuteExpert(true);
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        f.engine().setMuteExpert(false);
        QTRY_VERIFY_WITH_TIMEOUT(!f.mock.muted, kTimeout);
        f.engine().shutdown();
    }

    void respectsAnOperatorUnmute()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        // Held, so a late confirmation's retry, which a slow machine can cause and which is no
        // fight, is behind us; from here on no MUTE:true may go out.
        QTRY_COMPARE_WITH_TIMEOUT(f.status.mutePhase, MuteController::Phase::Holding, kTimeout);
        const int engaged = f.mock.muteCommandsSent(true);
        QVERIFY(engaged >= 1);
        QTest::qWait(100);

        f.mock.operatorSetMute(false);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.mutePhase, MuteController::Phase::OperatorOverride, kTimeout);
        QTest::qWait(700);
        QCOMPARE(f.mock.muteCommandsSent(true), engaged);  // not fought
        QVERIFY(!f.mock.muted);

        f.engine().shutdown();
        QCOMPARE(f.mock.muteCommandsSent(false), 0);
    }

    // Measured on ExpertSDR3 2.1, and not in the spec: any VOLUME set clears MUTE. So
    // the operator touching the volume slider is an unmute, and is theirs to make.
    void movingTheVolumeSliderCountsAsTheOperatorUnmuting()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        // Held, so a late confirmation's retry, which a slow machine can cause and which is no
        // fight, is behind us; from here on no MUTE:true may go out.
        QTRY_COMPARE_WITH_TIMEOUT(f.status.mutePhase, MuteController::Phase::Holding, kTimeout);
        const int engaged = f.mock.muteCommandsSent(true);
        QVERIFY(engaged >= 1);
        QTest::qWait(100);
        f.mock.operatorSetVolume(-12);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.mutePhase, MuteController::Phase::OperatorOverride, kTimeout);
        QTest::qWait(600);
        QVERIFY(!f.mock.muted);
        QCOMPARE(f.mock.muteCommandsSent(true), engaged);
        f.engine().shutdown();
    }

    // The same behaviour makes this load-bearing: a VOLUME from us would silently undo
    // our own mute and leave both programs playing.
    void neverSendsVolume()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        f.engine().setOutputGainDb(-20);
        f.engine().setBufferMs(100);
        QTest::qWait(150);
        f.engine().shutdown();
        QVERIFY(f.mock.commands.filter(QStringLiteral("volume")).isEmpty());
    }

    void aDroppedMuteSetIsRetried()
    {
        Fixture f;
        f.mock.dropMuteSets = 2;  // another client is holding the parameter
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muteCommandsSent(true) >= 1, kTimeout);
        QVERIFY(!f.mock.muted);
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        QCOMPARE(f.mock.muteCommandsSent(true), 3);
        // Those contradicting replies must not have read as the operator unmuting.
        QTRY_COMPARE_WITH_TIMEOUT(f.status.mutePhase, MuteController::Phase::Holding, kTimeout);
        f.engine().shutdown();
        QVERIFY(!f.mock.muted);
    }

    // The trap this all exists for: a reconnect sees mute:true, which is OUR mute.
    void reconnectStillRestoresTheOriginalState()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);

        const int startsBefore = f.mock.audioStartCount();
        f.mock.dropClient();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.audioStartCount() > startsBefore, 8000);
        QVERIFY(f.mock.muted);
        QTest::qWait(200);
        QCOMPARE(f.mock.muteCommandsSent(false), 0);  // no audible blip on the way through

        f.engine().shutdown();
        QVERIFY(!f.mock.muted);
    }

    void aCrashedSessionIsRestoredOnTheNextRun()
    {
        Fixture f;
        {
            // The previous process died holding the mute.
            Settings previous(f.settingsPath);
            previous.saveMuteOwnership(QStringLiteral("127.0.0.1"), {true, false});
        }
        f.mock.muted = true;
        AppConfig config = f.baseConfig();
        config.muteExpert = false;  // and this run does not want to hold it
        f.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(!f.mock.muted, kTimeout);
        f.engine().shutdown();
        QCOMPARE(Settings(f.settingsPath).loadMuteOwnership(QStringLiteral("127.0.0.1")).owned, false);
    }

    // With several radios on a network, a mute owed to one says nothing about another, which its
    // operator may have muted themselves. Unmuting that one would be exactly wrong.
    void aMuteOwedToOneRadioIsNotAppliedToAnother()
    {
        Fixture f;
        {
            Settings previous(f.settingsPath);
            previous.saveMuteOwnership(QStringLiteral("192.168.1.50"), {true, false});
        }
        f.mock.muted = true;  // this radio's operator muted it
        AppConfig config = f.baseConfig();
        config.muteExpert = false;
        f.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 0, kTimeout);
        QTest::qWait(200);
        f.engine().shutdown();

        QCOMPARE(f.mock.muteCommandsSent(false), 0);
        QVERIFY(f.mock.muted);
        // And the other radio's debt is still on record for when it comes back.
        QVERIFY(Settings(f.settingsPath).loadMuteOwnership(QStringLiteral("192.168.1.50")).owned);
    }

    // v0.1.0 kept one record, naming no radio. Whoever it crashed on while holding the mute must
    // still get their radio back after upgrading.
    void aMuteOwedBeforeUpgradingIsStillRestored()
    {
        Fixture f;
        {
            QSettings previous(f.settingsPath, QSettings::IniFormat);
            previous.setValue(QStringLiteral("radio/host"), QStringLiteral("127.0.0.1"));
            previous.setValue(QStringLiteral("muteOwnership/owned"), true);
            previous.setValue(QStringLiteral("muteOwnership/priorMuted"), false);
        }
        f.mock.muted = true;
        AppConfig config = f.baseConfig();
        config.muteExpert = false;
        f.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(!f.mock.muted, kTimeout);
        f.engine().shutdown();
        QVERIFY(!Settings(f.settingsPath).loadMuteOwnership(QStringLiteral("127.0.0.1")).owned);
    }

    void theOldMuteRecordMovesToTheSavedRadio_data() { addEachStore(); }
    void theOldMuteRecordMovesToTheSavedRadio()
    {
        QFETCH(bool, native);
        if (native && !nativeStoreWanted()) QSKIP("Writes this user's own settings store; ESA_TEST_NATIVE_SETTINGS=1 runs it");
        const SettingsStore store(native);
        const QString radio = QStringLiteral("192.168.1.50");
        store.raw()->setValue(QStringLiteral("radio/host"), radio);
        store.writeOldMuteRecord(true, false);

        const auto settings = store.open();
        QVERIFY(settings->loadMuteOwnership(radio).owned);
        QVERIFY(!settings->loadMuteOwnership(radio).priorMuted);
        QVERIFY(!settings->loadMuteOwnership(QStringLiteral("127.0.0.1")).owned);
        QVERIFY(!store.raw()->contains(QStringLiteral("muteOwnership/owned")));
        QVERIFY(!store.raw()->contains(QStringLiteral("muteOwnership/priorMuted")));

        // Starting again changes nothing, and neither does the debt being paid.
        QVERIFY(store.open()->loadMuteOwnership(radio).owned);
        settings->saveMuteOwnership(radio, {false, false});
        QVERIFY(!store.open()->loadMuteOwnership(radio).owned);
        QCOMPARE(store.muteKeys(), QStringList());
    }

    // A radio that was already muted must not be unmuted later, so this has to survive the move.
    void theOldMuteRecordKeepsWhatTheRadioWasBefore_data() { addEachStore(); }
    void theOldMuteRecordKeepsWhatTheRadioWasBefore()
    {
        QFETCH(bool, native);
        if (native && !nativeStoreWanted()) QSKIP("Writes this user's own settings store; ESA_TEST_NATIVE_SETTINGS=1 runs it");
        const SettingsStore store(native);
        const QString radio = QStringLiteral("192.168.1.50");
        store.raw()->setValue(QStringLiteral("radio/host"), radio);
        store.writeOldMuteRecord(true, true);

        const auto record = store.open()->loadMuteOwnership(radio);
        QVERIFY(record.owned);
        QVERIFY(record.priorMuted);
    }

    void theOldMuteRecordWithNoSavedRadioMeansTheDefaultOne_data() { addEachStore(); }
    void theOldMuteRecordWithNoSavedRadioMeansTheDefaultOne()
    {
        QFETCH(bool, native);
        if (native && !nativeStoreWanted()) QSKIP("Writes this user's own settings store; ESA_TEST_NATIVE_SETTINGS=1 runs it");
        const SettingsStore store(native);
        store.writeOldMuteRecord(true, false);
        QVERIFY(store.open()->loadMuteOwnership(AppConfig{}.host).owned);
    }

    // What nearly every upgrade looks like: v0.1.0 also wrote the record when it owed nothing.
    void anOldMuteRecordOwingNothingLeavesNothingBehind_data() { addEachStore(); }
    void anOldMuteRecordOwingNothingLeavesNothingBehind()
    {
        QFETCH(bool, native);
        if (native && !nativeStoreWanted()) QSKIP("Writes this user's own settings store; ESA_TEST_NATIVE_SETTINGS=1 runs it");
        const SettingsStore store(native);
        store.raw()->setValue(QStringLiteral("radio/host"), QStringLiteral("192.168.1.50"));
        store.writeOldMuteRecord(false, true);

        const auto settings = store.open();
        QVERIFY(!settings->loadMuteOwnership(QStringLiteral("192.168.1.50")).owned);
        QCOMPARE(store.muteKeys(), QStringList());
    }

    // Host names carry dots, colons and percent signs, which a store could read as structure, and
    // clearing one radio must not take another whose name starts the same way.
    void aRadiosMuteRecordIsKeptAndClearedInEachStore_data() { addEachStore(); }
    void aRadiosMuteRecordIsKeptAndClearedInEachStore()
    {
        QFETCH(bool, native);
        if (native && !nativeStoreWanted()) QSKIP("Writes this user's own settings store; ESA_TEST_NATIVE_SETTINGS=1 runs it");
        const SettingsStore store(native);
        const auto settings = store.open();
        const QString ip = QStringLiteral("192.168.1.5");
        const QString longerIp = QStringLiteral("192.168.1.50");
        const QString name = QStringLiteral("radio");
        const QString longerName = QStringLiteral("radio.local");
        const QString ipv6 = QStringLiteral("fe80::1%en0");
        const QStringList hosts{ip, longerIp, name, longerName, ipv6};

        for (const QString& host : hosts) settings->saveMuteOwnership(host, {true, true});
        for (const QString& host : hosts) {
            const auto record = store.open()->loadMuteOwnership(host);
            QVERIFY2(record.owned, qPrintable(host));
            QVERIFY2(record.priorMuted, qPrintable(host));
        }

        settings->saveMuteOwnership(ip, {false, false});
        settings->saveMuteOwnership(name, {false, false});
        QVERIFY(!store.open()->loadMuteOwnership(ip).owned);
        QVERIFY(!store.open()->loadMuteOwnership(name).owned);
        QVERIFY(store.open()->loadMuteOwnership(longerIp).owned);
        QVERIFY(store.open()->loadMuteOwnership(longerName).owned);
        QVERIFY(store.open()->loadMuteOwnership(ipv6).owned);

        for (const QString& host : hosts) settings->saveMuteOwnership(host, {false, false});
        QCOMPARE(store.muteKeys(), QStringList());
    }

    void applyingAChainChangesTheSoundAtOnce()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);

        float peak = 0.0f;
        const auto measure = [&] {
            f.sink->samples.clear();
            QTest::qWait(150);
            peak = 0.0f;
            for (float v : f.sink->samples) peak = std::max(peak, std::fabs(v));
        };
        measure();
        QVERIFY(peak > 0.1f && peak <= 0.2001f);  // the mock's dry signal

        ChainSettings chain;
        chain.params.insert(QStringLiteral("eq.enabled"), 1);
        chain.params.insert(QStringLiteral("eq.master"), 12);
        f.engine().applyChainSettings(chain);
        QTest::qWait(100);
        measure();
        QVERIFY2(peak > 0.5f, "the chain did not change");

        f.engine().applyChainSettings(defaultChain());
        QTest::qWait(100);
        measure();
        QVERIFY2(peak <= 0.2001f, "factory settings did not restore the dry signal");
        f.engine().shutdown();
    }

    void applyingAChainStartsItsNoiseReduction()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 0, kTimeout);
        ChainSettings chain;
        chain.nrMethod = QStringLiteral("rn2");
        f.engine().applyChainSettings(chain);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.nrMethod, QStringLiteral("rn2"), kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.nrPreparing, kTimeout);
        QVERIFY(!f.status.nrFailed);
        f.engine().shutdown();
    }

    void ownershipIsPersistedWhileHeld()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        const auto held = Settings(f.settingsPath).loadMuteOwnership(QStringLiteral("127.0.0.1"));
        QVERIFY(held.owned);
        QVERIFY(!held.priorMuted);
        f.engine().shutdown();
        QVERIFY(!Settings(f.settingsPath).loadMuteOwnership(QStringLiteral("127.0.0.1")).owned);
    }

    void aFailedOutputMeansNoMute()
    {
        Fixture f;
        f.failSinkOpen = true;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.blocksSent > 20, kTimeout);
        QTest::qWait(150);
        // Audio is arriving, but nothing can play it, so ExpertSDR3 must stay audible.
        QCOMPARE(f.mock.muteCommandsSent(true), 0);
        QVERIFY(!f.mock.muted);
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.outputError.isEmpty(), kTimeout);
        f.engine().shutdown();
    }

    // Regression: switching device once closed the old one through the path that
    // re-evaluates the mute, briefly unmuting ExpertSDR3 with both programs live.
    void changingTheOutputDeviceDoesNotBlipTheMute()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        // Held, so a late confirmation's retry, which a slow machine can cause and which is no
        // fight, is behind us; from here on no MUTE:true may go out.
        QTRY_COMPARE_WITH_TIMEOUT(f.status.mutePhase, MuteController::Phase::Holding, kTimeout);
        const int engaged = f.mock.muteCommandsSent(true);
        QVERIFY(engaged >= 1);
        const int opensBefore = f.sinkOpenCount;
        f.engine().setOutputDevice(QStringLiteral("another device"));
        QCOMPARE(f.sinkOpenCount, opensBefore + 1);
        QTest::qWait(300);
        QCOMPARE(f.mock.muteCommandsSent(false), 0);
        QCOMPARE(f.mock.muteCommandsSent(true), engaged);
        QVERIFY(f.mock.muted);
        f.engine().shutdown();
    }

    void aFailedSwitchToAnotherDeviceHandsAudioBack()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        f.failSinkOpen = true;
        f.engine().setOutputDevice(QStringLiteral("a device that will not open"));
        QTRY_VERIFY_WITH_TIMEOUT(!f.mock.muted, kTimeout);
        f.engine().shutdown();
    }

    void anUnpluggedDeviceHandsAudioBackAndRetries()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);

        const int opensBefore = f.sinkOpenCount;
        f.failSinkOpen = true;
        f.sink->isAlive = false;
        QTRY_VERIFY_WITH_TIMEOUT(!f.mock.muted, kTimeout);  // back to ExpertSDR3 at once

        f.failSinkOpen = false;
        QTRY_VERIFY_WITH_TIMEOUT(f.sinkOpenCount > opensBefore && f.status.outputOpen, 8000);
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);  // and taken again on recovery
        f.engine().shutdown();
        QVERIFY(!f.mock.muted);
    }

    void aDeadStreamHandsAudioBack()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        QTest::qWait(100);
        f.mock.sendAudio = false;
        f.mock.operatorRestartDsp();  // the stream stops and does not come back
        QTRY_VERIFY_WITH_TIMEOUT(!f.mock.muted, 6000);
        f.engine().shutdown();
    }

    void cwModeBypassesNrAndSpeechModesDoNot()
    {
        Fixture f;
        AppConfig config = f.baseConfig();
        config.nrMethod = QStringLiteral("rn2");
        f.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 0, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.nrPreparing, kTimeout);
        QVERIFY(!f.status.nrBypassedByMode);

        f.mock.operatorSetModulation(QStringLiteral("CW"));
        QTRY_VERIFY_WITH_TIMEOUT(f.status.nrBypassedByMode, kTimeout);

        f.mock.operatorSetModulation(QStringLiteral("USB"));
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.nrBypassedByMode, kTimeout);
        f.engine().shutdown();
    }

    void autoBypassCanBeTurnedOff()
    {
        Fixture f;
        AppConfig config = f.baseConfig();
        config.nrMethod = QStringLiteral("rn2");
        config.nrAutoBypass = false;
        f.mock.modulation = QStringLiteral("CW");
        f.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 0, kTimeout);
        QTest::qWait(150);
        QVERIFY(!f.status.nrBypassedByMode);
        f.engine().shutdown();
    }

    // DIGL/DIGU in stereo are I/Q (spec 3.4). The mock sends sin on L and cos on R.
    void digitalModesPlayTheRealComponentOnBothEars()
    {
        Fixture f;
        f.mock.modulation = QStringLiteral("DIGU");
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        const auto& s = f.sink->samples;
        for (size_t i = 0; i + 1 < s.size(); i += 2) QCOMPARE(s[i + 1], s[i]);
        f.engine().shutdown();
    }

    void ordinaryModesKeepTheirStereoImage()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        const auto& s = f.sink->samples;
        bool differs = false;
        for (size_t i = 0; i + 1 < s.size(); i += 2) differs = differs || s[i] != s[i + 1];
        QVERIFY(differs);
        f.engine().shutdown();
    }

    void malformedBlocksAreCountedNotFatal()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 0, kTimeout);

        f.mock.sendBinary(QByteArray(10, 'x'));  // shorter than a header
        QByteArray lying(64 + 16, '\0');
        qToLittleEndian<quint32>(48000, lying.data() + 4);
        qToLittleEndian<quint32>(3, lying.data() + 8);
        qToLittleEndian<quint32>(0x7FFFFFFF, lying.data() + 20);  // claims 2 billion samples
        qToLittleEndian<quint32>(1, lying.data() + 24);
        qToLittleEndian<quint32>(2, lying.data() + 28);
        f.mock.sendBinary(lying);

        QTRY_VERIFY_WITH_TIMEOUT(f.status.malformedBlocks >= 2, kTimeout);
        const quint64 before = f.sink->framesWritten;
        QTRY_VERIFY_WITH_TIMEOUT(f.sink->framesWritten > before, kTimeout);  // still playing
        f.engine().shutdown();
    }

    // Blocks the radio sent before it saw the disconnect still arrive while the socket closes.
    // They belong to the session that has ended: not played, not metered, not taken as audio.
    void audioStillArrivingAfterADisconnectIsIgnored()
    {
        Fixture f;
        AppConfig config = f.baseConfig();
        config.muteExpert = false;  // no mute to give back, so the socket closes at once
        f.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(f.audioFlowing(), kTimeout);
        const std::shared_ptr<LiveTaps> taps = f.engine().taps();
        f.engine().disconnectRadio();
        QCOMPARE(f.status.connection, TciClient::State::Disconnected);
        QVERIFY(!f.status.audioFlowing);
        const quint64 blocks = taps->blocks.load();

        // Whole blocks, as the stream sends them, from a server that has not yet seen the close.
        QByteArray frame(64 + 1024 * int(sizeof(float)), '\0');
        qToLittleEndian<quint32>(0, frame.data());  // receiver 0
        qToLittleEndian<quint32>(48000, frame.data() + 4);
        qToLittleEndian<quint32>(3, frame.data() + 8);  // float32
        qToLittleEndian<quint32>(1024, frame.data() + 20);
        qToLittleEndian<quint32>(1, frame.data() + 24);  // RX audio
        qToLittleEndian<quint32>(2, frame.data() + 28);  // stereo
        auto* samples = reinterpret_cast<float*>(frame.data() + 64);
        for (int i = 0; i < 1024; ++i) samples[i] = 0.25f;
        QVERIFY(f.mock.hasClient());  // its end is still open: it has not processed the close yet
        for (int i = 0; i < 5; ++i) f.mock.sendBinary(frame);
        QTest::qWait(300);
        QCOMPARE(taps->blocks.load(), blocks);
        QVERIFY(!f.status.audioFlowing);
        QCOMPARE(f.status.connection, TciClient::State::Disconnected);
        f.engine().shutdown();
    }

    // With a mute to give back, the link stays up after Disconnect until ExpertSDR3 has taken it,
    // and the radio goes on streaming meanwhile. That session is over all the same: no status may
    // show its audio flowing, and its blocks must not set the mute policy going again.
    void audioArrivingWhileTheMuteIsGivenBackIsNotShownAsFlowing()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(f.audioFlowing(), kTimeout);
        QTest::qWait(100);
        const int engagesBefore = f.mock.muteCommandsSent(true);
        // The restore takes four tries, about a second, so the disconnect waits for it with the
        // radio streaming; within the 1.5 s a disconnect waits before letting go of the mute.
        f.mock.dropMuteSets = 3;

        QList<EngineStatus> after;
        // What the output had been handed, at each of those statuses while it was open: the
        // engine lets go of it as the link closes.
        QList<quint64> heardAfter;
        qint64 sentWhileClosing = -1;
        const qint64 sentAtDisconnect = f.mock.blocksSent;
        const QMetaObject::Connection watch = QObject::connect(&f.engine(), &Engine::statusChanged, &f.mock,
                                                               [&](const EngineStatus& s) {
            after.append(s);
            heardAfter.append(s.outputOpen && f.sink ? f.sink->framesWritten : 0);
            if (sentWhileClosing < 0 && s.connection == TciClient::State::Disconnected) {
                sentWhileClosing = f.mock.blocksSent - sentAtDisconnect;
            }
        });
        f.engine().disconnectRadio();
        QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Disconnected, kTimeout);
        QTest::qWait(300);
        f.engine().shutdown();
        QObject::disconnect(watch);

        // What makes this the case it is: the hand-back was still pending with the link up, and
        // the radio kept sending audio through it.
        QVERIFY(std::any_of(after.cbegin(), after.cend(), [](const EngineStatus& s) {
            return s.connection == TciClient::State::Ready && s.mutePhase == MuteController::Phase::Restoring;
        }));
        // Enough for the checks below; a slow CI runner's mock sends fewer than real time.
        QVERIFY2(sentWhileClosing > 10, qPrintable(QString::number(sentWhileClosing)));
        // And the engine still took that audio, for the output and any recording: it is only
        // not shown as flowing.
        qsizetype firstUp = -1;
        qsizetype lastUp = -1;
        for (qsizetype i = 0; i < after.size(); ++i) {
            if (after[i].connection != TciClient::State::Ready || !after[i].outputOpen) continue;
            if (firstUp < 0) firstUp = i;
            lastUp = i;
        }
        QVERIFY(firstUp >= 0 && lastUp > firstUp);
        QVERIFY2(after[lastUp].blocks > after[firstUp].blocks + 10,
                 qPrintable(QStringLiteral("%1 to %2").arg(after[firstUp].blocks).arg(after[lastUp].blocks)));
        QVERIFY2(heardAfter[lastUp] > heardAfter[firstUp],
                 qPrintable(QStringLiteral("%1 to %2").arg(heardAfter[firstUp]).arg(heardAfter[lastUp])));

        for (qsizetype i = 0; i < after.size(); ++i) {
            const EngineStatus& s = after[i];
            const QString which = QStringLiteral("status %1 of %2 after the disconnect, link %3")
                                      .arg(i + 1)
                                      .arg(after.size())
                                      .arg(s.connection == TciClient::State::Ready ? QStringLiteral("still up")
                                                                                   : QStringLiteral("down"));
            QVERIFY2(!s.audioFlowing, qPrintable(which));
            QVERIFY2(s.mutePhase != MuteController::Phase::Engaging && s.mutePhase != MuteController::Phase::Holding,
                     qPrintable(which));
        }
        QVERIFY(!f.mock.muted);
        QCOMPARE(f.mock.muteCommandsSent(true), engagesBefore);
        QVERIFY(!Settings(f.settingsPath).loadMuteOwnership(QStringLiteral("127.0.0.1")).owned);
    }

    void forwardCounterGapsAreCountedAsLoss()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 0, kTimeout);
        QByteArray frame(64 + 8, '\0');
        qToLittleEndian<quint32>(48000, frame.data() + 4);
        qToLittleEndian<quint32>(3, frame.data() + 8);
        qToLittleEndian<quint32>(2, frame.data() + 20);
        qToLittleEndian<quint32>(1, frame.data() + 24);
        qToLittleEndian<quint32>(2, frame.data() + 28);
        qToLittleEndian<quint32>(100000, frame.data() + 32);
        f.mock.sendBinary(frame);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.droppedBlocks > 0, kTimeout);
        f.engine().shutdown();
    }

    void transmittingBypassesTheChain()
    {
        Fixture f;
        AppConfig config = f.baseConfig();
        config.params.insert(QStringLiteral("tube.enabled"), 1);
        config.params.insert(QStringLiteral("tube.drive"), 24);
        f.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 0, kTimeout);
        QTest::qWait(100);

        f.mock.operatorSetTransmit(true);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.transmitting, kTimeout);
        QTest::qWait(60);
        f.sink->samples.clear();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink->samples.size() > 4800, kTimeout);
        // The mock's dry signal peaks at 0.2. Driven by 24 dB it would be far above.
        float peak = 0.0f;
        for (float v : f.sink->samples) peak = std::max(peak, std::fabs(v));
        QVERIFY2(peak <= 0.2001f, qPrintable(QString::number(peak)));

        f.mock.operatorSetTransmit(false);
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.transmitting, kTimeout);
        f.engine().shutdown();
    }

    void anAnsweringServerThatNeverSaysReadyIsAbandonedForTheNextPort()
    {
        // Accepts the WebSocket, then sends nothing: not a usable TCI server.
        QWebSocketServer silent(QStringLiteral("silent"), QWebSocketServer::NonSecureMode);
        QVERIFY(listenOffExpertPorts(silent));
        Fixture f;
        AppConfig config = f.baseConfig();
        config.port = silent.serverPort();
        config.fallbackPorts = {f.mock.port()};
        f.start(config);
        QTest::qWait(300);
        QVERIFY(f.status.connection != TciClient::State::Ready);
        QVERIFY(!f.mock.muted);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Ready, 8000);
        QCOMPARE(f.status.connectedPort, f.mock.port());
        f.engine().shutdown();
    }

    void aRefusedPortFallsBackAndThenStaysOnTheOneThatWorked()
    {
        Fixture f;
        const int closedPort = closedLocalPort();  // nothing listens there, and no radio could
        QVERIFY(closedPort > 0);

        AppConfig config = f.baseConfig();
        config.port = closedPort;
        config.fallbackPorts = {f.mock.port()};
        f.start(config);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Ready, 8000);
        QCOMPARE(f.status.connectedPort, f.mock.port());

        const int startsBefore = f.mock.audioStartCount();
        f.mock.dropClient();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.audioStartCount() > startsBefore, 8000);
        QCOMPARE(f.status.connectedPort, f.mock.port());
        f.engine().shutdown();
    }

    void anExplicitPortNeverWandersOffToAnotherServer()
    {
        Fixture f;
        const int closedPort = closedLocalPort();  // nothing listens there, and no radio could
        QVERIFY(closedPort > 0);

        AppConfig config = f.baseConfig();
        config.port = closedPort;
        config.fallbackPorts = {};
        f.start(config);
        QTest::qWait(600);
        QVERIFY(!f.mock.hasClient());
        QVERIFY(f.status.connection != TciClient::State::Ready);
        f.engine().shutdown();
    }

    void changingReceiverSwitchesTheStream()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.audioStartCount() == 1, kTimeout);
        f.engine().setReceiver(1);
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.commands.contains(QStringLiteral("audio_start:1")), kTimeout);
        QVERIFY(f.mock.commands.contains(QStringLiteral("audio_stop:0")));
        QTRY_COMPARE_WITH_TIMEOUT(f.status.receiver, 1, kTimeout);
        f.engine().shutdown();
    }

    void disconnectStopsTheStreamAndRestores()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        QTest::qWait(100);
        f.engine().disconnectRadio();
        QTRY_VERIFY_WITH_TIMEOUT(!f.mock.muted, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.commands.contains(QStringLiteral("audio_stop:0")), kTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Disconnected, kTimeout);
        f.engine().shutdown();
    }

    // Switching radios: a connect arriving while the old radio gets its mute back must not be
    // undone by the disconnect still in progress. It once was, by a nested event loop.
    void connectingDuringADisconnectEndsUpConnected()
    {
        Fixture f;
        MockExpert second;
        QVERIFY(second.listen());
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        QTest::qWait(100);

        f.engine().disconnectRadio();
        AppConfig config = f.baseConfig();
        config.port = second.port();
        f.engine().configure(config);
        f.engine().connectRadio();

        QTRY_VERIFY_WITH_TIMEOUT(!f.mock.muted, kTimeout);  // the first radio got its mute back
        QTRY_VERIFY_WITH_TIMEOUT(second.muted, kTimeout);   // and the second is playing
        QTest::qWait(300);
        QCOMPARE(f.status.connection, TciClient::State::Ready);
        QCOMPARE(f.status.connectedPort, second.port());
        QVERIFY2(f.status.audioFlowing, "the new connection was torn down");
        QVERIFY(f.status.outputOpen);
        f.engine().shutdown();
        QVERIFY(!second.muted);
    }

    void aDisconnectAfterThatConnectCancelsIt()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        QTest::qWait(100);
        const int startsBefore = f.mock.audioStartCount();

        f.engine().disconnectRadio();
        f.engine().connectRadio();
        f.engine().disconnectRadio();

        QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Disconnected, kTimeout);
        QTest::qWait(300);
        QCOMPARE(f.status.connection, TciClient::State::Disconnected);
        QCOMPARE(f.mock.audioStartCount(), startsBefore);
        QVERIFY(!f.mock.muted);
        f.engine().shutdown();
    }

    // The window closing just after Disconnect was pressed. shutdown() must still see it through.
    void shuttingDownDuringADisconnectStillRestoresTheMute()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        QTest::qWait(100);
        f.engine().disconnectRadio();
        f.engine().shutdown();
        QVERIFY(!f.mock.muted);
        QVERIFY(!Settings(f.settingsPath).loadMuteOwnership(QStringLiteral("127.0.0.1")).owned);
    }

    void aRadioThatNeverConfirmsTheRestoreCannotHangADisconnect()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        QTest::qWait(100);
        f.mock.dropMuteSets = 100;  // it will never honour the restore
        QElapsedTimer timer;
        timer.start();
        f.engine().disconnectRadio();
        QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Disconnected, kTimeout);
        QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
        // What is owed stays on record, so the next session can still put it right.
        QVERIFY(Settings(f.settingsPath).loadMuteOwnership(QStringLiteral("127.0.0.1")).owned);
        f.engine().shutdown();
    }

    // The status says it is disconnecting from the moment Disconnect is pressed until the link
    // is closed, while ExpertSDR3 takes its mute back, and at no other time: not while a connect
    // fails, nor through a connection, a link that drops and comes back, a disconnect with
    // nothing to give back, or the connection after one. Throughout it, audio is not flowing.
    void theStatusSaysDisconnectingOnlyWhileTheMuteIsGivenBack()
    {
        Fixture f;
        QList<EngineStatus> seen;
        QObject::connect(&f.engine(), &Engine::statusChanged, &f.mock, [&](const EngineStatus& s) { seen.append(s); });
        const auto disconnectingIn = [&](qsizetype from) {
            qsizetype count = 0;
            for (qsizetype i = from; i < seen.size(); ++i) count += seen[i].disconnecting ? 1 : 0;
            return count;
        };

        // A connect that fails, then given up: nothing to give back, so it closes at once.
        AppConfig nowhere = f.baseConfig();
        nowhere.port = closedLocalPort();  // nothing listens there, and no radio could
        QVERIFY(nowhere.port > 0);
        f.start(nowhere);
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.connectionError.isEmpty(), kTimeout);
        QTest::qWait(300);
        QCOMPARE(f.status.connection, TciClient::State::Connecting);
        f.engine().disconnectRadio();
        QCOMPARE(f.status.connection, TciClient::State::Disconnected);

        // Connected, muted, then the link drops and comes back by itself.
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(f.audioFlowing(), kTimeout);
        const int startsBefore = f.mock.audioStartCount();
        // Only what follows the drop: the failed connect above was Connecting too.
        const qsizetype dropped = seen.size();
        f.mock.dropClient();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.audioStartCount() > startsBefore, 8000);
        QTRY_VERIFY_WITH_TIMEOUT(f.audioFlowing() && f.mock.muted, kTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.mutePhase, MuteController::Phase::Holding, kTimeout);
        QTest::qWait(100);
        QVERIFY(std::any_of(seen.cbegin() + dropped, seen.cend(),
                            [](const EngineStatus& s) { return s.connection == TciClient::State::Connecting; }));
        QCOMPARE(disconnectingIn(0), 0);

        // Disconnect, with the restore taking three tries: disconnecting at once, and until closed.
        f.mock.dropMuteSets = 2;
        const qsizetype pressed = seen.size();
        f.engine().disconnectRadio();
        QVERIFY(seen.size() > pressed);  // said straight away, not at the next tick
        QVERIFY(f.status.disconnecting);
        QCOMPARE(f.status.connection, TciClient::State::Ready);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Disconnected, kTimeout);
        QVERIFY(!f.status.disconnecting);
        QVERIFY(!f.mock.muted);
        qsizetype closed = -1;
        for (qsizetype i = pressed; i < seen.size(); ++i) {
            const EngineStatus& s = seen[i];
            QVERIFY2(!s.audioFlowing, qPrintable(QStringLiteral("status %1 after the press").arg(i - pressed + 1)));
            if (s.connection == TciClient::State::Disconnected) {
                if (closed < 0) closed = i;
            } else {
                // Up to the close, every status says so, and the link was up all along.
                QVERIFY2(closed < 0 && s.disconnecting && s.connection == TciClient::State::Ready,
                         qPrintable(QStringLiteral("status %1 after the press").arg(i - pressed + 1)));
            }
        }
        QVERIFY(closed > pressed + 1);  // it did wait, through more than one status
        QTest::qWait(300);
        QCOMPARE(disconnectingIn(closed), 0);

        // A connection after it, and a connect pressed during a disconnect: the old link says it
        // is disconnecting until closed, and the new one never does.
        f.engine().connectRadio();
        QTRY_VERIFY_WITH_TIMEOUT(f.audioFlowing() && f.mock.muted, kTimeout);
        QCOMPARE(disconnectingIn(closed), 0);
        f.mock.dropMuteSets = 2;
        f.engine().disconnectRadio();
        QVERIFY(f.status.disconnecting);
        const qsizetype switching = seen.size();
        f.engine().connectRadio();
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.disconnecting, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.connection == TciClient::State::Ready && f.audioFlowing(), kTimeout);
        qsizetype reopened = -1;
        for (qsizetype i = switching; i < seen.size(); ++i) {
            if (reopened < 0 && seen[i].connection != TciClient::State::Ready) reopened = i;
        }
        // The restore takes three tries, so statuses came between the press and the close, and
        // every one of them still said disconnecting, Connect pressed or not.
        QVERIFY(reopened > switching);
        for (qsizetype i = switching; i < reopened; ++i) {
            QVERIFY2(seen[i].disconnecting && seen[i].connection == TciClient::State::Ready,
                     qPrintable(QStringLiteral("status %1 after the second press").arg(i - switching + 1)));
        }
        QCOMPARE(disconnectingIn(reopened), 0);

        f.engine().shutdown();
        QVERIFY(!f.mock.muted);
        QVERIFY(!f.status.disconnecting);
    }

    // ── Live data for the window ─────────────────────────────────────────────

    void theTapsAreOneObjectForTheEnginesLife()
    {
        Fixture f;
        const std::shared_ptr<LiveTaps> taps = f.engine().taps();
        QVERIFY(taps);
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 0, kTimeout);
        f.engine().configure(f.baseConfig());
        QCOMPARE(f.engine().taps(), taps);
        f.engine().shutdown();
        f.m_engine.reset();
        // The window can outlive the engine, and what it holds is still there.
        QCOMPARE(taps.use_count(), 1);
        QCOMPARE(taps->view.gate.inputPeakDb(), -120.0f);
    }

    void stageMetersFollowTheAudioAndFallWhenItStops()
    {
        Fixture f;
        AppConfig config = f.baseConfig();
        for (const char* stage : {"gate", "comp", "tube", "pudu"}) {
            config.params.insert(QString::fromLatin1(stage) + QStringLiteral(".enabled"), 1);
        }
        config.params.insert(QStringLiteral("gate.threshold"), -60);  // the -14 dBFS tone keeps it open
        config.params.insert(QStringLiteral("comp.threshold"), -40);
        f.start(config);
        const std::shared_ptr<LiveTaps> taps = f.engine().taps();
        const StageSet& view = taps->view;
        QTRY_VERIFY_WITH_TIMEOUT(metersLive(view), kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(view.comp.gainReductionDb() < -1.0f, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(taps->blocks.load() > 10, kTimeout);
        QVERIFY(taps->inputPeak.exchange(0.0f) > 0.1f);  // the mock's tone peaks at 0.2

        QElapsedTimer timer;
        timer.start();
        f.engine().setParam(QStringLiteral("gate.enabled"), 0);  // unticked
        QTRY_VERIFY_WITH_TIMEOUT(view.gate.inputPeakDb() == -120.0f, kTimeout);
        qInfo() << "gate meter at rest" << timer.elapsed() << "ms after unticking";
        QVERIFY2(timer.elapsed() < 300, qPrintable(QString::number(timer.elapsed())));
        QVERIFY(view.comp.inputPeakDb() > -40.0f);  // the others play on
        QCOMPARE(view.gate.gainReductionDb(), 0.0f);

        timer.restart();
        f.mock.operatorSetTransmit(true);
        QTRY_VERIFY_WITH_TIMEOUT(metersAtRest(view), kTimeout);
        qInfo() << "meters at rest" << timer.elapsed() << "ms after transmitting";
        QVERIFY2(timer.elapsed() < 300, qPrintable(QString::number(timer.elapsed())));
        f.mock.operatorSetTransmit(false);
        f.engine().setParam(QStringLiteral("gate.enabled"), 1);
        QTRY_VERIFY_WITH_TIMEOUT(metersLive(view), kTimeout);

        // Nothing arrives at all: the engine notices 250 ms after the last block. Polled
        // finely here, because QTRY's own 50 ms steps would blur the measurement.
        f.mock.sendAudio = false;
        f.mock.operatorRestartDsp();
        timer.restart();
        while (!metersAtRest(view) && timer.elapsed() < kTimeout) QTest::qWait(2);
        qInfo() << "meters at rest" << timer.elapsed() << "ms after the stream stopped";
        QVERIFY(metersAtRest(view));
        QVERIFY2(timer.elapsed() < 300, qPrintable(QString::number(timer.elapsed())));
        QVERIFY(!taps->nrActive.load());
        f.engine().shutdown();
    }

    void theEqTapCarriesTheToneOnlyWhenWanted()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 9600, kTimeout);
        const std::shared_ptr<LiveTaps> taps = f.engine().taps();
        QCOMPARE(taps->eqTap.size(), size_t(0));  // a hidden page costs nothing
        QCOMPARE(taps->outTap.size(), size_t(0));

        taps->eqTap.skip(taps->eqTap.size());
        taps->eqWanted = true;
        QTRY_VERIFY_WITH_TIMEOUT(taps->eqTap.size() >= 4096, kTimeout);
        const std::vector<float> samples = drain(taps->eqTap);
        // The mock plays 600 Hz: bin 51 of 4096 at 48 kHz.
        QCOMPARE(strongestBin(samples, 4096), int(std::lround(600.0 * 4096 / 48000.0)));

        // A new chain is handed the tap too.
        f.engine().configure(f.baseConfig());
        drain(taps->eqTap);
        QTRY_VERIFY_WITH_TIMEOUT(taps->eqTap.size() >= 1024, kTimeout);

        taps->eqWanted = false;
        drain(taps->eqTap);
        const quint64 before = f.sink->framesWritten;
        QTRY_VERIFY_WITH_TIMEOUT(f.sink->framesWritten > before + 4800, kTimeout);
        QCOMPARE(taps->eqTap.size(), size_t(0));
        f.engine().shutdown();
    }

    void theOutputTapHoldsExactlyWhatTheSpeakersGot()
    {
        Fixture f;
        AppConfig config = f.baseConfig();
        config.params.insert(QStringLiteral("tube.enabled"), 1);
        config.params.insert(QStringLiteral("tube.drive"), 12);
        f.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        const std::shared_ptr<LiveTaps> taps = f.engine().taps();
        QCOMPARE(taps->outTap.size(), size_t(0));

        // The engine runs on this thread, so no block lands between these two lines.
        f.sink->samples.clear();
        taps->outWanted = true;
        QTRY_VERIFY_WITH_TIMEOUT(taps->outTap.size() >= 9600, kTimeout);
        taps->outWanted = false;
        const std::vector<float> tapped = drain(taps->outTap);
        QVERIFY(f.sink->samples.size() >= tapped.size());
        QVERIFY2(std::memcmp(tapped.data(), f.sink->samples.data(), tapped.size() * sizeof(float)) == 0,
                 "the tap and the speakers got different samples");
        QVERIFY(peakOf(tapped) > 0.25f);  // the chain's output, not the radio's 0.2
        f.engine().shutdown();
    }

    // With no output device, the meter and scope still have the audio to show.
    void theOutputTapRunsWithoutAnOutputDevice()
    {
        Fixture f;
        f.failSinkOpen = true;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.blocksSent > 10, kTimeout);
        const std::shared_ptr<LiveTaps> taps = f.engine().taps();
        taps->outWanted = true;
        QTRY_VERIFY_WITH_TIMEOUT(taps->outTap.size() >= 4800, kTimeout);
        f.engine().shutdown();
    }

    void bypassPlaysTheRadioDryUntilReleased()
    {
        Fixture f;
        AppConfig config = f.baseConfig();
        config.params.insert(QStringLiteral("tube.enabled"), 1);
        config.params.insert(QStringLiteral("tube.drive"), 24);
        f.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        const auto measure = [&] {
            f.sink->samples.clear();
            QTest::qWait(150);
            return peakOf(f.sink->samples);
        };
        QVERIFY(measure() > 0.5f);
        QVERIFY(!f.status.rxBypassed);

        f.engine().setRxBypassed(true);
        QVERIFY(f.status.rxBypassed);  // reported at once, not at the next tick
        QVERIFY2(measure() <= 0.2001f, "BYPASS still drove the tube");

        // A new chain, as on a new configuration, a connect or a rate change, keeps it.
        f.engine().configure(config);
        QVERIFY2(measure() <= 0.2001f, "BYPASS was lost by a rebuild");
        QVERIFY(f.status.rxBypassed);

        // It is the session's, never the settings': a crash cannot leave the chain off.
        const QStringList saved = QSettings(f.settingsPath, QSettings::IniFormat).allKeys();
        QVERIFY2(saved.filter(QStringLiteral("bypass"), Qt::CaseInsensitive).isEmpty(), qPrintable(saved.join(u',')));

        f.engine().setRxBypassed(false);
        QVERIFY(!f.status.rxBypassed);
        QVERIFY(measure() > 0.5f);
        f.engine().shutdown();
    }

    // The radio sends a rate other than the chain's: the chain is rebuilt at that rate,
    // and the window's taps and the operator's BYPASS come along.
    void aStreamRateChangeKeepsTheTapsAndBypass()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        const std::shared_ptr<LiveTaps> taps = f.engine().taps();
        QCOMPARE(taps->chainRate.load(), 48000);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.chainRate, 48000, kTimeout);
        f.engine().setRxBypassed(true);

        AppConfig slow = f.baseConfig();
        slow.chainRate = 24000;
        f.engine().configure(slow);  // the stream was asked for at 48 kHz and stays so
        QCOMPARE(taps->chainRate.load(), 24000);
        QTRY_COMPARE_WITH_TIMEOUT(taps->chainRate.load(), 48000, kTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.chainRate, 48000, kTimeout);
        QVERIFY(f.status.rxBypassed);

        f.engine().setRxBypassed(false);
        drain(taps->eqTap);
        taps->eqWanted = true;
        QTRY_VERIFY_WITH_TIMEOUT(taps->eqTap.size() >= 1024, kTimeout);
        f.engine().shutdown();
    }

    // The window's timers read the display mirror and drain the taps while the engine,
    // on its own thread as in the app, replaces its chain again and again: on each new
    // configuration, and each time the stream's rate disagrees with it. Under the
    // sanitizers this is what finds a lifetime or ordering mistake in the shared data.
    void rebuildingTheChainWhileTheWindowPollsIsSafe()
    {
        MockExpert mock;
        QVERIFY(mock.listen());
        QTemporaryDir dir;
        auto frames = std::make_shared<std::atomic<quint64>>(0);
        auto* engine = new Engine(dir.filePath(QStringLiteral("s.ini")),
                                  [frames](const AppConfig&, QString*) -> std::unique_ptr<AudioSink> {
                                      return std::make_unique<CountingSink>(frames);
                                  });
        const std::shared_ptr<LiveTaps> taps = engine->taps();
        QThread thread;
        engine->moveToThread(&thread);
        thread.start();

        AppConfig config;
        config.host = QStringLiteral("127.0.0.1");
        config.port = mock.port();
        config.fallbackPorts = {};  // never the real radio's ports
        config.muteExpert = false;
        config.nrMethod = QStringLiteral("nr2");
        for (const QString& stage : RxChain::stageIds()) config.params.insert(stage + QStringLiteral(".enabled"), 1);
        QMetaObject::invokeMethod(engine, [engine, config] {
            engine->configure(config);
            engine->connectRadio();
        });

        // What the pages' 16 ms ticks do, faster.
        std::vector<float> scratch(24000 * 2);
        int polls = 0;
        volatile float readings = 0.0f;
        QTimer window;
        window.setInterval(4);
        QObject::connect(&window, &QTimer::timeout, &mock, [&] {
            const StageSet& view = taps->view;
            float sum = view.gate.inputPeakDb() + view.gate.gainReductionDb() + view.comp.outputPeakDb()
                        + view.comp.gainReductionDb() + view.tube.inputPeakDb() + view.pudu.outputPeakDb();
            double value = 0.0;
            for (const ParamSpec& spec : RxChain::stageParams()) {
                RxChain::readStageParam(view, spec.key, &value);
                sum += float(value);
            }
            RxChain::applyStageParam(taps->view, QStringLiteral("gate.threshold"), -40.0 - polls % 10);
            taps->eqTap.read(scratch.data(), 24000);
            taps->outTap.read(scratch.data(), 24000);
            sum += taps->inputPeak.exchange(0.0f) + taps->nrGain.load() + float(taps->blocks.load() % 2);
            readings = sum;  // so the reads cannot be optimised away
            ++polls;
        });
        taps->eqWanted = true;
        taps->outWanted = true;
        window.start();

        QTRY_VERIFY_WITH_TIMEOUT(frames->load() > 4800, kTimeout);
        for (int i = 0; i < 100; ++i) {
            AppConfig next = config;
            next.chainRate = i % 2 ? 24000 : 48000;
            next.nrMethod = i % 3 ? QStringLiteral("nr2") : QString();
            QMetaObject::invokeMethod(engine, [engine, next] { engine->configure(next); });
            QTest::qWait(8);
        }
        const quint64 before = frames->load();
        QTRY_VERIFY_WITH_TIMEOUT(frames->load() > before + 4800, kTimeout);  // still playing
        QVERIFY(polls > 50);
        window.stop();
        taps->eqWanted = false;
        taps->outWanted = false;

        QMetaObject::invokeMethod(engine, &Engine::shutdown, Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(engine, [engine] { delete engine; }, Qt::BlockingQueuedConnection);
        thread.quit();
        QVERIFY(thread.wait(kTimeout));
        QCOMPARE(taps.use_count(), 1);
    }

    // REC and PLAY as the app runs them: the engine on its own thread handing frames to the disk
    // thread and taking them back from it, while the window polls the taps and turns them on and
    // off. The CI's ThreadSanitizer job runs this.
    void recordingAndPlayingWhileTheWindowPollsIsSafe()
    {
        MockExpert mock;
        QVERIFY(mock.listen());
        QTemporaryDir dir;
        auto frames = std::make_shared<std::atomic<quint64>>(0);
        Recorder::Options recorder;
        // Many more hand-offs to and from the disk thread than the defaults make.
        recorder.drainIntervalMs = 20;
        recorder.headerIntervalMs = 50;
        recorder.playRefillMs = 20;
        auto* engine = new Engine(dir.filePath(QStringLiteral("s.ini")),
                                  [frames](const AppConfig&, QString*) -> std::unique_ptr<AudioSink> {
                                      return std::make_unique<CountingSink>(frames);
                                  },
                                  nullptr, recorder);
        const std::shared_ptr<LiveTaps> taps = engine->taps();
        QThread thread;
        engine->moveToThread(&thread);
        thread.start();

        // Delivered queued, on this thread, as the window gets them.
        EngineStatus status;
        QStringList failures;
        QObject::connect(engine, &Engine::statusChanged, &mock, [&](const EngineStatus& s) { status = s; });
        QObject::connect(engine, &Engine::recordingFailed, &mock, [&](const QString& r) { failures << r; });
        QObject::connect(engine, &Engine::playbackFailed, &mock, [&](const QString& r) { failures << r; });
        // A failed check returns early. The engine and its thread are closed all the same, so the
        // failure is reported rather than aborting the whole program. Declared after what the
        // engine's signals write to, so it runs before those go.
        bool closed = false;
        const auto closeOnFailure = qScopeGuard([&] {
            if (closed) return;
            QMetaObject::invokeMethod(engine, &Engine::shutdown, Qt::BlockingQueuedConnection);
            QMetaObject::invokeMethod(engine, [engine] { delete engine; }, Qt::BlockingQueuedConnection);
            thread.quit();
            thread.wait(kTimeout);
        });
        // A std::function keeps its target on the heap, written here and read on the engine's
        // thread. Qt's queue orders that, out of ThreadSanitizer's sight; a release once the
        // call is built and an acquire where it runs state the order where it can see it.
        std::atomic<int> handoff{0};
        const auto call = [engine, &handoff](std::function<void(Engine*)> fn) {
            auto run = [engine, fn = std::move(fn), &handoff] {
                (void)handoff.load(std::memory_order_acquire);
                fn(engine);
            };
            handoff.fetch_add(1, std::memory_order_release);
            QMetaObject::invokeMethod(engine, std::move(run));
        };

        AppConfig config;
        config.host = QStringLiteral("127.0.0.1");
        config.port = mock.port();
        config.fallbackPorts = {};  // never the real radio's ports
        config.muteExpert = false;
        config.nrMethod = QStringLiteral("nr2");
        for (const QString& stage : RxChain::stageIds()) config.params.insert(stage + QStringLiteral(".enabled"), 1);
        call([config](Engine* e) {
            e->configure(config);
            e->connectRadio();
        });

        // The pages' 16 ms ticks, faster, with a page change every so often.
        std::vector<float> scratch(24000 * 2);
        int polls = 0;
        volatile float readings = 0.0f;
        QTimer window;
        window.setInterval(4);
        QObject::connect(&window, &QTimer::timeout, &mock, [&] {
            const StageSet& view = taps->view;
            float sum = view.gate.inputPeakDb() + view.comp.gainReductionDb() + view.tube.outputPeakDb()
                        + view.pudu.inputPeakDb();
            taps->eqTap.read(scratch.data(), 24000);
            taps->outTap.read(scratch.data(), 24000);
            sum += taps->inputPeak.exchange(0.0f) + taps->nrGain.load() + float(taps->chainRate.load() % 2);
            if (++polls % 25 == 0) {
                const bool show = !taps->outWanted.load();
                if (show) taps->outTap.skip(taps->outTap.size());
                taps->outWanted = show;
                if (show) taps->eqTap.skip(taps->eqTap.size());
                taps->eqWanted = show;
            }
            readings = sum;  // so the reads cannot be optimised away
        });
        window.start();
        QTRY_VERIFY_WITH_TIMEOUT(status.audioFlowing, kTimeout);

        // A recording, then PLAY of it to its end while the next one records.
        const QString first = dir.filePath(QStringLiteral("first.wav"));
        call([first](Engine* e) { e->startRecordingToFile(first); });
        // Long enough that each PLAY below lasts over a second: a slow CI runner sends statuses
        // far enough apart to miss a shorter one's middle.
        QTRY_VERIFY_WITH_TIMEOUT(status.recording.seconds > 1.5, kTimeout);
        call([](Engine* e) { e->stopRecording(); });
        QTRY_COMPARE_WITH_TIMEOUT(status.lastRecordingPath, first, kTimeout);
        const QString during = dir.filePath(QStringLiteral("during.wav"));
        call([during](Engine* e) {
            e->startRecordingToFile(during);
            e->startPlayback();
        });
        QTRY_VERIFY_WITH_TIMEOUT(status.playback.active, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(!status.playback.active, kTimeout);
        // As long as the first, so the PLAYs of it below last over a second however fast the
        // radio streamed meanwhile.
        QTRY_VERIFY_WITH_TIMEOUT(status.recording.path == during && status.recording.seconds > 1.5, kTimeout);
        call([](Engine* e) { e->stopRecording(); });
        QTRY_COMPARE_WITH_TIMEOUT(status.lastRecordingPath, during, kTimeout);

        // PLAY stopped half-way, a second click.
        call([](Engine* e) { e->startPlayback(); });
        QTRY_VERIFY2_WITH_TIMEOUT(status.playback.active && status.playback.seconds > 0.2,
                                  qPrintable(QStringLiteral("playing %1, waiting %2, %3 of %4 s, failures: %5")
                                                 .arg(status.playback.active)
                                                 .arg(status.playback.waiting)
                                                 .arg(status.playback.seconds)
                                                 .arg(status.playback.totalSeconds)
                                                 .arg(failures.join(QStringLiteral("; ")))),
                                  kTimeout);
        call([](Engine* e) { e->stopPlayback(); });
        QTRY_VERIFY_WITH_TIMEOUT(!status.playback.active, kTimeout);

        // With no radio: PLAY opens the output for itself and closes it after.
        call([](Engine* e) { e->disconnectRadio(); });
        QTRY_COMPARE_WITH_TIMEOUT(status.connection, TciClient::State::Disconnected, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(!status.outputOpen, kTimeout);
        call([](Engine* e) { e->startPlayback(); });
        QTRY_VERIFY_WITH_TIMEOUT(status.playback.active, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(!status.playback.active && !status.outputOpen, kTimeout);

        // And PLAY cut short by connecting, with a REC straight after.
        call([](Engine* e) { e->startPlayback(); });
        QTRY_VERIFY_WITH_TIMEOUT(status.playback.seconds > 0.1, kTimeout);
        const QString last = dir.filePath(QStringLiteral("last.wav"));
        call([last](Engine* e) {
            e->connectRadio();
            e->startRecordingToFile(last);
        });
        // A status from after the call: until then the status still shows the last
        // recording's length.
        QTRY_VERIFY_WITH_TIMEOUT(status.recording.active && status.recording.path == last && status.audioFlowing
                                     && status.recording.seconds > 0.3,
                                 kTimeout);
        QVERIFY2(!status.playback.active, qPrintable(QStringLiteral("playing %1 of %2 s, connection %3, recording %4")
                                                         .arg(status.playback.seconds)
                                                         .arg(status.playback.totalSeconds)
                                                         .arg(int(status.connection))
                                                         .arg(status.recording.path)));
        QVERIFY(polls > 50);
        window.stop();
        taps->eqWanted = false;
        taps->outWanted = false;

        // Quitting finishes the recording in progress.
        QMetaObject::invokeMethod(engine, &Engine::shutdown, Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(engine, [engine] { delete engine; }, Qt::BlockingQueuedConnection);
        closed = true;
        thread.quit();
        QVERIFY(thread.wait(kTimeout));
        QCOMPARE(taps.use_count(), 1);
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(u'\n')));
        for (const QString& path : {first, during, last}) {
            const std::optional<WavFile::Info> info = WavFile::parseFile(path);
            QVERIFY2(info, qPrintable(path));
            QVERIFY2(info->frames > 0, qPrintable(path));
            QCOMPARE(info->declaredFrames, info->framesInFile);
        }
    }

    // ── Recording and playback ───────────────────────────────────────────────

    // The status names the file playing. A recording finished meanwhile is the last one, and
    // the next PLAY's, but not what is playing now.
    void thePlaybackSaysWhichFileItPlays()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        const QString first = f.dir.filePath(QStringLiteral("first.wav"));
        f.engine().startRecordingToFile(first);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds >= 3.0, 2 * kTimeout);
        f.engine().stopRecording();
        QTRY_COMPARE_WITH_TIMEOUT(f.status.lastRecordingPath, first, kTimeout);
        QVERIFY(f.status.playback.path.isEmpty());

        f.engine().startPlayback();
        QVERIFY(f.status.playback.active);
        QCOMPARE(f.status.playback.path, first);
        const QString second = f.dir.filePath(QStringLiteral("second.wav"));
        f.engine().startRecordingToFile(second);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds >= 0.2, kTimeout);
        f.engine().stopRecording();
        QTRY_COMPARE_WITH_TIMEOUT(f.status.lastRecordingPath, second, kTimeout);
        QVERIFY2(f.status.playback.active, "the first recording should still be playing");
        QCOMPARE(f.status.playback.path, first);

        QTRY_VERIFY_WITH_TIMEOUT(!f.status.playback.active, kTimeout);
        QVERIFY(f.status.playback.path.isEmpty());
        f.engine().shutdown();
    }

    // REC holds the chain's output, before the volume: exactly what the speakers were handed.
    void aRecordingHoldsExactlyWhatTheSpeakersGot()
    {
        Fixture f;
        AppConfig config = f.baseConfig();
        config.params.insert(QStringLiteral("tube.enabled"), 1);
        config.params.insert(QStringLiteral("tube.drive"), 12);  // so it is the chain's output, not the radio's
        config.outputGainDb = 0.0;
        f.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        const QString folder = f.dir.filePath(QStringLiteral("Recordings"));

        // The engine runs on this thread, so no block lands between these two lines.
        f.sink->samples.clear();
        f.engine().startRecording(folder);
        QVERIFY(f.status.recording.active);
        QVERIFY(f.status.recording.error.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(f.sink->samples.size() >= size_t(2 * 48000 * 2), kTimeout);  // 2 s of tone
        f.engine().stopRecording();
        const std::vector<float> heard = f.sink->samples;
        QVERIFY(!f.status.recording.active);

        QTRY_VERIFY_WITH_TIMEOUT(f.status.lastRecordingAvailable, kTimeout);
        const QString path = f.status.lastRecordingPath;
        QCOMPARE(QFileInfo(path).absolutePath(), QDir(folder).absolutePath());
        QCOMPARE(f.status.recording.path, path);
        // Named from the time and the radio: the mock's receiver 0 is on 7.160 MHz LSB.
        const QString name = QFileInfo(path).fileName();
        QVERIFY2(QRegularExpression(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}_\\d{6}Z_7\\.160MHz_LSB\\.wav$"))
                     .match(name)
                     .hasMatch(),
                 qPrintable(name));

        const std::optional<Recorded> file = readRecording(path);
        QVERIFY(file);
        QCOMPARE(file->info.sampleRate, 48000);
        QCOMPARE(file->info.declaredFrames, file->info.framesInFile);  // finished, header and all
        QCOMPARE(file->frames.size(), heard.size());
        QVERIFY2(sameBits(file->frames.data(), heard.data(), heard.size()),
                 "the recording and the speakers got different samples");
        QVERIFY(peakOf(file->frames) > 0.25f);  // the chain's output, not the radio's 0.2
        QCOMPARE(f.status.recording.seconds, double(heard.size() / 2) / 48000.0);
        QCOMPARE(f.status.recording.droppedFrames, quint64(0));
        QVERIFY(f.status.recording.error.isEmpty());
        f.engine().shutdown();
    }

    void recIsRefusedUntilARadioIsConnected()
    {
        Fixture f;
        f.engine().configure(f.baseConfig());
        QStringList failures;
        QObject::connect(&f.engine(), &Engine::recordingFailed, &f.mock,
                         [&](const QString& reason) { failures << reason; });
        const QString folder = f.dir.filePath(QStringLiteral("Recordings"));

        f.engine().startRecording(folder);
        QVERIFY(!f.status.recording.active);  // reported at once, so the button flips back
        QCOMPARE(f.status.recording.error, QStringLiteral("Connect to a radio first"));
        // A press turned down, not a recording that stopped: the window says so differently.
        QVERIFY(f.status.recording.refused);
        QCOMPARE(failures, QStringList{QStringLiteral("Connect to a radio first")});
        QVERIFY(!QFileInfo::exists(folder));

        // Connecting answers it. REC is taken even before the link is up, and waits for audio.
        f.engine().connectRadio();
        QVERIFY(f.status.recording.error.isEmpty());
        QVERIFY(!f.status.recording.refused);
        f.engine().startRecording(folder);
        QVERIFY(f.status.recording.active);
        QVERIFY(f.status.connection != TciClient::State::Ready);

        // Stopped before any audio arrived: nothing is left behind, not even the folder.
        f.engine().stopRecording();
        QVERIFY(!f.status.recording.active);
        QTRY_VERIFY_WITH_TIMEOUT(f.audioFlowing(), kTimeout);
        QTest::qWait(400);  // longer than the disk thread's drain interval
        f.engine().shutdown();
        QVERIFY(!QFileInfo::exists(folder));
        QVERIFY(f.status.recording.path.isEmpty());
        QVERIFY(!f.status.lastRecordingAvailable);
        QCOMPARE(failures.size(), 1);
    }

    // Connected, but the receiver sends nothing: a REC there leaves no file, and its status
    // names none.
    void aRecordingThatGotNoAudioNamesNoFile()
    {
        Fixture f;
        f.mock.sendAudio = false;
        f.start();
        QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Ready, kTimeout);
        const QString path = f.dir.filePath(QStringLiteral("take.wav"));
        f.engine().startRecordingToFile(path);
        QVERIFY(f.status.recording.active);
        QVERIFY(f.status.recording.path.isEmpty());  // no file yet, so none named
        QTest::qWait(400);  // longer than the disk thread's drain interval
        f.engine().stopRecording();
        QTest::qWait(400);
        QVERIFY(!f.status.recording.active);
        QVERIFY2(f.status.recording.path.isEmpty(), qPrintable(f.status.recording.path));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!f.status.lastRecordingAvailable);
        QCOMPARE(f.status.recording.seconds, 0.0);
        QVERIFY(f.status.recording.error.isEmpty());

        // Nor does it take over a name the last recording left: the status is about this one.
        // The mock decides whether to stream when the stream is asked for, so each change of
        // mind takes a reconnect.
        const auto reconnect = [&f] {
            f.engine().disconnectRadio();
            QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Disconnected, kTimeout);
            f.engine().connectRadio();
            QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Ready, kTimeout);
        };
        f.mock.sendAudio = true;
        reconnect();
        QTRY_VERIFY_WITH_TIMEOUT(f.audioFlowing(), kTimeout);
        QVERIFY(recordFor(f, path, 0.2));
        QCOMPARE(f.status.recording.path, path);
        f.mock.sendAudio = false;
        reconnect();
        f.engine().startRecordingToFile(f.dir.filePath(QStringLiteral("silent.wav")));
        QVERIFY(f.status.recording.path.isEmpty());
        f.engine().stopRecording();
        f.engine().shutdown();
        QVERIFY2(f.status.recording.path.isEmpty(), qPrintable(f.status.recording.path));
        QVERIFY(!QFileInfo::exists(f.dir.filePath(QStringLiteral("silent.wav"))));
        QCOMPARE(f.status.lastRecordingPath, path);  // PLAY still has the last real one
    }

    // REC pressed while the link comes up takes its name from the radio once it is there.
    void aRecordingStartedWhileConnectingIsNamedFromTheRadio()
    {
        Fixture f;
        AppConfig config = f.baseConfig();
        config.receiver = 1;  // the mock's receiver 1 is on 14.017 MHz CW
        f.engine().configure(config);
        f.engine().connectRadio();
        const QString folder = f.dir.filePath(QStringLiteral("Recordings"));
        f.engine().startRecording(folder);
        QVERIFY(f.status.recording.active);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds > 0.3, kTimeout);
        f.engine().stopRecording();
        QTRY_VERIFY_WITH_TIMEOUT(f.status.lastRecordingAvailable, kTimeout);
        const QString name = QFileInfo(f.status.lastRecordingPath).fileName();
        QVERIFY2(QRegularExpression(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}_\\d{6}Z_14\\.017MHz_CW_RX1\\.wav$"))
                     .match(name)
                     .hasMatch(),
                 qPrintable(name));

        // A name already taken gets _1: nothing is ever overwritten.
        const QString path = f.dir.filePath(QStringLiteral("take.wav"));
        QVERIFY(recordFor(f, path, 0.2));
        const qint64 firstSize = QFileInfo(path).size();
        f.engine().startRecordingToFile(path);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds > 0.2, kTimeout);
        f.engine().stopRecording();
        QTRY_COMPARE_WITH_TIMEOUT(f.status.lastRecordingPath, f.dir.filePath(QStringLiteral("take_1.wav")), kTimeout);
        QCOMPARE(QFileInfo(path).size(), firstSize);
        f.engine().shutdown();
    }

    // Frames at two rates never share a file: a rate change goes on in _part2.
    void aStreamRateChangeContinuesTheRecordingInPart2()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        const QString path = f.dir.filePath(QStringLiteral("take.wav"));
        f.engine().startRecordingToFile(path);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds > 0.5, kTimeout);

        f.mock.operatorSetStreamRate(24000);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.chainRate, 24000, kTimeout);
        const double atChange = f.status.recording.seconds;
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds > atChange + 0.3, kTimeout);
        QVERIFY(f.status.recording.active);
        f.engine().stopRecording();

        const QString part2 = f.dir.filePath(QStringLiteral("take_part2.wav"));
        QTRY_COMPARE_WITH_TIMEOUT(f.status.lastRecordingPath, part2, kTimeout);
        QCOMPARE(f.status.recording.path, part2);
        const std::optional<Recorded> first = readRecording(path);
        const std::optional<Recorded> second = readRecording(part2);
        QVERIFY(first && second);
        QCOMPARE(first->info.sampleRate, 48000);
        QCOMPARE(second->info.sampleRate, 24000);
        QVERIFY(first->info.frames >= 24000);
        QVERIFY(second->info.frames > 0);
        QCOMPARE(first->info.declaredFrames, first->info.framesInFile);
        QCOMPARE(second->info.declaredFrames, second->info.framesInFile);
        // Nothing was lost across the roll: the status counted what the files hold.
        QCOMPARE(f.status.recording.seconds,
                 double(first->info.frames) / 48000.0 + double(second->info.frames) / 24000.0);
        QCOMPARE(QDir(f.dir.path()).entryList({QStringLiteral("take*")}, QDir::Files).size(), 2);
        f.engine().shutdown();
    }

    void disconnectAndQuitFinishTheRecording()
    {
        Fixture f;
        // Every write takes a while, so a disconnect that did not wait for the disk would report
        // before the file was finished.
        f.recorderOptions.openDevice = [](const QString& path) { return std::make_unique<SlowFile>(path, 60); };
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.audioFlowing(), kTimeout);
        const QString a = f.dir.filePath(QStringLiteral("a.wav"));
        f.engine().startRecordingToFile(a);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds > 0.5, kTimeout);

        // Whole the moment the engine reports it is disconnected, not some time after: it waited
        // for the disk. Looked at from inside that report, before anything else can run here.
        std::optional<WavFile::Info> atDisconnect;
        double secondsAtDisconnect = 0.0;
        const QMetaObject::Connection watch = QObject::connect(&f.engine(), &Engine::statusChanged, &f.mock,
                                                               [&](const EngineStatus& s) {
            if (!atDisconnect && s.connection == TciClient::State::Disconnected && !s.recording.active) {
                atDisconnect = WavFile::parseFile(a);
                secondsAtDisconnect = s.recording.seconds;
            }
        });
        f.engine().disconnectRadio();
        QTRY_VERIFY_WITH_TIMEOUT(atDisconnect.has_value(), kTimeout);
        QObject::disconnect(watch);
        QVERIFY(atDisconnect->declaredFrames > 0);
        QCOMPARE(atDisconnect->declaredFrames, atDisconnect->framesInFile);
        QCOMPARE(double(atDisconnect->declaredFrames) / 48000.0, secondsAtDisconnect);  // every frame recorded
        QCOMPARE(QFileInfo(a).size(), qint64(WavFile::kHeaderBytes) + qint64(atDisconnect->declaredFrames) * 8);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.lastRecordingPath, a, kTimeout);
        std::optional<WavFile::Info> info;

        f.engine().connectRadio();
        QTRY_VERIFY_WITH_TIMEOUT(f.audioFlowing(), kTimeout);
        const QString b = f.dir.filePath(QStringLiteral("b.wav"));
        f.engine().startRecordingToFile(b);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds > 0.5, kTimeout);
        f.engine().shutdown();  // quitting asks nothing, and loses nothing
        info = WavFile::parseFile(b);
        QVERIFY(info);
        QCOMPARE(info->declaredFrames, info->framesInFile);
        // The last status, which a headless run prints, has the finished file.
        QVERIFY(!f.status.recording.active);
        QCOMPARE(f.status.recording.path, b);
        QCOMPARE(f.status.recording.seconds, double(info->declaredFrames) / 48000.0);
        QCOMPARE(f.status.lastRecordingPath, b);
    }

    // A recording can fail in its last write, after the stop. The failure is reported once.
    // If REC has been pressed again by then, the new recording's status stays its own.
    void aLateFailureIsReportedWithoutTakingOverTheNextRecording()
    {
        Fixture f;
        auto failing = std::make_shared<std::atomic<bool>>(false);
        // Only the drain at the stop writes, so that is where a failure comes.
        f.recorderOptions.drainIntervalMs = 5000;
        f.recorderOptions.openDevice = [failing](const QString& path) -> std::unique_ptr<QFileDevice> {
            if (QFileInfo(path).fileName().startsWith(QLatin1String("doomed"))) {
                return std::make_unique<FailingFile>(path, failing);
            }
            return std::make_unique<QFile>(path);
        };
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.audioFlowing(), kTimeout);
        QStringList failures;
        QObject::connect(&f.engine(), &Engine::recordingFailed, &f.mock,
                         [&](const QString& reason) { failures << reason; });

        // Nothing else running: the status says why it stopped.
        f.engine().startRecordingToFile(f.dir.filePath(QStringLiteral("doomed-a.wav")));
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds > 0.3, kTimeout);
        failing->store(true);
        f.engine().stopRecording();
        QTRY_COMPARE_WITH_TIMEOUT(failures.size(), 1, kTimeout);
        QVERIFY2(failures.last().contains(QStringLiteral("doomed-a.wav"))
                     && failures.last().contains(QStringLiteral("injected failure")),
                 qPrintable(failures.last()));
        QTRY_COMPARE_WITH_TIMEOUT(f.status.recording.error, failures.last(), kTimeout);
        QVERIFY(!f.status.recording.refused);
        QVERIFY(!f.status.recording.active);
        // No audio reached the disk, so no file is left and none is named.
        QVERIFY(f.status.recording.path.isEmpty());
        QVERIFY(!QFileInfo::exists(f.dir.filePath(QStringLiteral("doomed-a.wav"))));

        // REC pressed again at once, before the last write has failed.
        f.engine().startRecordingToFile(f.dir.filePath(QStringLiteral("doomed-b.wav")));
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds > 0.3, kTimeout);
        const QString c = f.dir.filePath(QStringLiteral("c.wav"));
        f.engine().stopRecording();
        f.engine().startRecordingToFile(c);
        QVERIFY(f.status.recording.error.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(failures.size(), 2, kTimeout);
        QVERIFY2(failures.last().contains(QStringLiteral("doomed-b.wav")), qPrintable(failures.last()));
        QTest::qWait(100);  // the status that followed the failure
        QVERIFY(f.status.recording.active);
        QVERIFY2(f.status.recording.error.isEmpty(), qPrintable(f.status.recording.error));
        f.engine().stopRecording();
        QTRY_COMPARE_WITH_TIMEOUT(f.status.lastRecordingPath, c, kTimeout);
        QCOMPARE(f.status.recording.path, c);
        QVERIFY(f.status.recording.error.isEmpty());
        QCOMPARE(failures.size(), 2);
        f.engine().shutdown();
    }

    // A recording that cannot be written is reported once, keeps nothing broken, and the
    // audio goes on.
    void aRecordingThatCannotBeWrittenIsReportedAndTheAudioGoesOn()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        QStringList failures;
        QObject::connect(&f.engine(), &Engine::recordingFailed, &f.mock,
                         [&](const QString& reason) { failures << reason; });
        // A file where the folder should be, so the folder cannot be made.
        QFile blocker(f.dir.filePath(QStringLiteral("blocked")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();

        f.engine().startRecording(f.dir.filePath(QStringLiteral("blocked/Recordings")));
        QVERIFY(f.status.recording.active);
        QTRY_COMPARE_WITH_TIMEOUT(failures.size(), 1, kTimeout);
        QVERIFY(!f.status.recording.active);
        QCOMPARE(f.status.recording.error, failures.first());
        QVERIFY(!f.status.recording.refused);  // it was taken, then stopped
        QVERIFY(f.status.recording.path.isEmpty());  // nothing could be created
        QVERIFY2(failures.first().contains(QStringLiteral("blocked")), qPrintable(failures.first()));
        const quint64 before = f.sink->framesWritten;
        QTRY_VERIFY_WITH_TIMEOUT(f.sink->framesWritten > before + 4800, kTimeout);  // still playing
        QTest::qWait(300);
        QCOMPARE(failures.size(), 1);
        QVERIFY(!f.status.lastRecordingAvailable);

        // The next REC starts clean.
        f.engine().startRecordingToFile(f.dir.filePath(QStringLiteral("ok.wav")));
        QVERIFY(f.status.recording.active);
        QVERIFY(f.status.recording.error.isEmpty());
        f.engine().shutdown();
    }

    // A recording that stopped early belongs to its session. The status keeps the reason
    // through the disconnect that ends it, as a headless run's summary reports it, and the
    // next connection starts with none.
    void aNewConnectionStartsWithNoRecordingFailure()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        QSignalSpy failed(&f.engine(), &Engine::recordingFailed);
        QFile blocker(f.dir.filePath(QStringLiteral("blocked")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        f.engine().startRecording(f.dir.filePath(QStringLiteral("blocked/Recordings")));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, kTimeout);
        const QString reason = f.status.recording.error;
        QVERIFY(!reason.isEmpty());
        QVERIFY(!f.status.recording.refused);

        f.engine().disconnectRadio();
        QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Disconnected, kTimeout);
        QCOMPARE(f.status.recording.error, reason);

        f.engine().connectRadio();
        QVERIFY2(f.status.recording.error.isEmpty(), qPrintable(f.status.recording.error));
        QVERIFY(!f.status.recording.refused);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.audioFlowing, kTimeout);
        QVERIFY(f.status.recording.error.isEmpty());
        QCOMPARE(failed.count(), 1);
        f.engine().shutdown();
    }

    // The disk fills up mid-recording: the file keeps every whole frame that fitted, the
    // failure is reported once, PLAY can have what was kept, and the live audio never stopped.
    void aFullDiskStopsTheRecordingButNotTheAudio()
    {
        Fixture f;
        const qint64 capacity = WavFile::kHeaderBytes + 24000 * 8 + 3;  // half a second, and a part frame
        f.recorderOptions.openDevice = [capacity](const QString& path) {
            return std::make_unique<FullDiskFile>(path, capacity);
        };
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        QStringList failures;
        QObject::connect(&f.engine(), &Engine::recordingFailed, &f.mock,
                         [&](const QString& reason) { failures << reason; });
        const QString path = f.dir.filePath(QStringLiteral("full.wav"));
        f.engine().startRecordingToFile(path);
        QTRY_COMPARE_WITH_TIMEOUT(failures.size(), 1, kTimeout);
        QVERIFY(!f.status.recording.active);
        QCOMPARE(f.status.recording.error, failures.first());
        QVERIFY2(failures.first().contains(QStringLiteral("No space left on device"))
                     && failures.first().contains(QStringLiteral("full.wav")),
                 qPrintable(failures.first()));
        const quint64 before = f.sink->framesWritten;
        QTRY_VERIFY_WITH_TIMEOUT(f.sink->framesWritten > before + 9600, kTimeout);  // the audio went on

        const std::optional<Recorded> file = readRecording(path);
        QVERIFY(file);
        QCOMPARE(file->info.declaredFrames, quint64(24000));
        QCOMPARE(file->info.framesInFile, quint64(24000));
        QCOMPARE(QFileInfo(path).size(), qint64(WavFile::kHeaderBytes) + 24000 * 8);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.lastRecordingPath, path, kTimeout);
        QTest::qWait(300);
        QCOMPARE(failures.size(), 1);
        f.engine().shutdown();
    }

    // The recording is the chain's output whatever becomes of the speakers: a device that dies
    // mid-recording and will not open again stops nothing.
    void aRecordingGoesOnWhenTheOutputFails()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        QStringList failures;
        QObject::connect(&f.engine(), &Engine::recordingFailed, &f.mock,
                         [&](const QString& reason) { failures << reason; });
        // The chain's output as the scope gets it, which goes on without a device.
        const std::shared_ptr<LiveTaps> taps = f.engine().taps();
        std::vector<float> chain;
        QTimer window;
        window.setInterval(5);
        QObject::connect(&window, &QTimer::timeout, &f.mock, [&] {
            const std::vector<float> got = drain(taps->outTap);
            chain.insert(chain.end(), got.begin(), got.end());
        });

        // The engine runs on this thread, so no block lands between these lines.
        taps->outTap.skip(taps->outTap.size());
        taps->outWanted = true;
        window.start();
        const QString path = f.dir.filePath(QStringLiteral("take.wav"));
        f.engine().startRecordingToFile(path);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds > 0.3, kTimeout);

        // Unplugged, and it does not come back.
        f.failSinkOpen = true;
        f.sink->isAlive = false;
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.outputOpen, kTimeout);
        QVERIFY(!f.status.outputError.isEmpty());
        const double atFailure = f.status.recording.seconds;
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds > atFailure + 1.0, kTimeout);
        QVERIFY(f.status.recording.active);
        QVERIFY(f.status.recording.error.isEmpty());
        QVERIFY(!f.status.outputOpen);

        f.engine().stopRecording();
        window.stop();
        const std::vector<float> rest = drain(taps->outTap);
        chain.insert(chain.end(), rest.begin(), rest.end());
        taps->outWanted = false;
        QTRY_COMPARE_WITH_TIMEOUT(f.status.lastRecordingPath, path, kTimeout);
        const std::optional<Recorded> file = readRecording(path);
        QVERIFY(file);
        QCOMPARE(file->info.declaredFrames, file->info.framesInFile);
        QCOMPARE(file->frames.size(), chain.size());
        QVERIFY2(sameBits(file->frames.data(), chain.data(), chain.size()), "the recording is not the chain's output");
        QCOMPARE(f.status.recording.seconds, double(file->info.frames) / 48000.0);
        QVERIFY(failures.isEmpty());
        f.engine().shutdown();
    }

    // A stream-rate change ends a recording's part and goes on in the next. When the old part's
    // last write fails, the recorder fails its continuation too, with the same reason: one
    // cause, so one report.
    void aRateRollWhoseOldPartFailsIsReportedOnce()
    {
        Fixture f;
        auto failing = std::make_shared<std::atomic<bool>>(false);
        f.recorderOptions.drainIntervalMs = 5000;  // only the drain at the roll writes
        f.recorderOptions.openDevice = [failing](const QString& path) -> std::unique_ptr<QFileDevice> {
            return std::make_unique<FailingFile>(path, failing);
        };
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.audioFlowing(), kTimeout);
        QStringList failures;
        QObject::connect(&f.engine(), &Engine::recordingFailed, &f.mock,
                         [&](const QString& reason) { failures << reason; });
        const QString path = f.dir.filePath(QStringLiteral("take.wav"));
        f.engine().startRecordingToFile(path);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds > 0.3, kTimeout);

        failing->store(true);
        f.mock.operatorSetStreamRate(24000);
        QTRY_COMPARE_WITH_TIMEOUT(failures.size(), 1, kTimeout);
        QVERIFY2(failures.first().contains(QStringLiteral("take.wav"))
                     && failures.first().contains(QStringLiteral("injected failure")),
                 qPrintable(failures.first()));
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.recording.active, kTimeout);
        QCOMPARE(f.status.recording.error, failures.first());
        QVERIFY(!f.status.recording.refused);
        QCOMPARE(f.status.chainRate, 24000);
        QTest::qWait(500);  // the continuation's failure and the end of both have come by now
        QCOMPARE(failures.size(), 1);
        QVERIFY(!QFileInfo::exists(f.dir.filePath(QStringLiteral("take_part2.wav"))));
        f.engine().shutdown();
        QCOMPARE(failures.size(), 1);
    }

    // REC pressed while connecting to a radio that never answers waits for it. When a disconnect
    // or quit ends it still waiting, the status (and so a headless run's summary) says nothing
    // was recorded, rather than looking like a recording that got no audio. A refusal, so the
    // status that says so comes before the signal. A stop is the operator's own choice, and
    // says nothing (as in recIsRefusedUntilARadioIsConnected).
    void aRecordingWaitingForARadioThatNeverAnswersSaysSo()
    {
        Fixture f;
        const int closedPort = closedLocalPort();  // nothing listens there, and no radio could
        QVERIFY(closedPort > 0);
        AppConfig config = f.baseConfig();
        config.port = closedPort;
        config.fallbackPorts = {};  // never the real radio's ports
        config.muteExpert = false;  // and nothing to say to one, were it reached
        f.engine().configure(config);
        QStringList events;  // statuses carrying a refusal, and the signal, in the order they came
        QObject::connect(&f.engine(), &Engine::statusChanged, &f.mock, [&](const EngineStatus& s) {
            if (s.recording.refused) events << QStringLiteral("status: ") + s.recording.error;
        });
        QObject::connect(&f.engine(), &Engine::recordingFailed, &f.mock,
                         [&](const QString& reason) { events << QStringLiteral("signal: ") + reason; });
        const QString folder = f.dir.filePath(QStringLiteral("Recordings"));
        const QString never = QStringLiteral("The radio never connected, so nothing was recorded");

        // Disconnected: refused at once.
        f.engine().startRecording(folder);
        const QString notConnected = QStringLiteral("Connect to a radio first");
        QCOMPARE(events.mid(0, 2), QStringList({QStringLiteral("status: ") + notConnected,
                                                QStringLiteral("signal: ") + notConnected}));

        // Connecting: taken, and waits.
        f.engine().connectRadio();
        f.engine().startRecording(folder);
        QVERIFY(f.status.recording.active);
        QVERIFY(f.status.recording.error.isEmpty());
        QTest::qWait(500);
        QVERIFY(f.status.connection != TciClient::State::Ready);
        QVERIFY(f.status.recording.active);
        QCOMPARE(f.status.recording.seconds, 0.0);

        events.clear();
        f.engine().stopRecording();
        QVERIFY(!f.status.recording.active);
        QVERIFY(f.status.recording.error.isEmpty());
        QVERIFY(events.isEmpty());

        // A disconnect while it waits.
        f.engine().startRecording(folder);
        QVERIFY(f.status.recording.active);
        QTest::qWait(300);
        f.engine().disconnectRadio();
        QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Disconnected, kTimeout);
        QVERIFY(!f.status.recording.active);
        QCOMPARE(f.status.recording.error, never);
        QVERIFY(f.status.recording.refused);  // nothing was kept, so nothing to warn about
        QVERIFY(f.status.recording.path.isEmpty());
        QCOMPARE(events.mid(0, 2), QStringList({QStringLiteral("status: ") + never, QStringLiteral("signal: ") + never}));

        // Quitting while it waits says the same in the last status, which a headless run prints.
        f.engine().connectRadio();
        QVERIFY(f.status.recording.error.isEmpty());  // connecting answers a refusal
        f.engine().startRecording(folder);
        QVERIFY(f.status.recording.active);
        QTest::qWait(300);
        f.engine().shutdown();
        QVERIFY(!f.status.recording.active);
        QCOMPARE(f.status.recording.error, never);
        QVERIFY(f.status.recording.refused);
        QVERIFY(f.status.recording.path.isEmpty());
        QCOMPARE(f.status.recording.seconds, 0.0);
        QVERIFY(!QFileInfo::exists(folder));
    }

    // PLAY sends the speakers exactly the file's frames, at the pace of the file, in place of
    // the live audio, which goes on being processed. Afterwards the live audio is back.
    void playSendsExactlyTheFilesFramesPacedInPlaceOfTheLiveAudio()
    {
        Fixture f;
        AppConfig config = f.baseConfig();
        config.params.insert(QStringLiteral("tube.enabled"), 1);
        config.params.insert(QStringLiteral("tube.drive"), 12);
        f.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        const QString path = f.dir.filePath(QStringLiteral("take.wav"));
        QVERIFY(recordFor(f, path, 1.0));
        const std::optional<Recorded> file = readRecording(path);
        QVERIFY(file);
        const size_t fileFrames = file->frames.size() / 2;
        const qint64 fileMs = qint64(fileFrames) * 1000 / 48000;
        // The live audio turned well down, so a live block among the playback would show.
        f.engine().setParam(QStringLiteral("tube.enabled"), 0);
        f.engine().setParam(QStringLiteral("eq.enabled"), 1);
        f.engine().setParam(QStringLiteral("eq.master"), -24);

        // What the scope would have drawn, drained as the window does.
        const std::shared_ptr<LiveTaps> taps = f.engine().taps();
        taps->outTap.skip(taps->outTap.size());
        std::vector<float> scoped;
        QTimer window;
        window.setInterval(5);
        QObject::connect(&window, &QTimer::timeout, &f.mock, [&] {
            const std::vector<float> got = drain(taps->outTap);
            scoped.insert(scoped.end(), got.begin(), got.end());
        });
        // The engine reports the end straight away, so the sink then holds the playback alone.
        // That status also says how much the recording holds, read as the radio's stream stood.
        bool wasPlaying = false;
        std::optional<size_t> heardAtEnd;
        double recordedAtEnd = 0.0;
        qint64 sentAtEnd = 0;
        quint64 chainBlocksAtEnd = 0;
        QObject::connect(&f.engine(), &Engine::statusChanged, &f.mock, [&](const EngineStatus& s) {
            if (wasPlaying && !s.playback.active && !heardAtEnd) {
                heardAtEnd = f.sink->samples.size();
                recordedAtEnd = s.recording.seconds;
                sentAtEnd = f.mock.blocksSent;
                chainBlocksAtEnd = taps->blocks.load();
                const std::vector<float> got = drain(taps->outTap);
                scoped.insert(scoped.end(), got.begin(), got.end());
                taps->outWanted = false;
            }
            wasPlaying = s.playback.active;
        });
        QStringList failures;
        QObject::connect(&f.engine(), &Engine::playbackFailed, &f.mock,
                         [&](const QString& reason) { failures << reason; });

        // A recording made meanwhile holds the live chain, not what was played.
        const QString during = f.dir.filePath(QStringLiteral("during.wav"));
        const qint64 sentAtRecording = f.mock.blocksSent;
        f.engine().startRecordingToFile(during);
        QVERIFY(f.status.recording.active);

        const quint64 blocksBefore = taps->blocks.load();
        const qint64 sentBefore = f.mock.blocksSent;
        f.sink->samples.clear();
        taps->outWanted = true;
        window.start();
        QElapsedTimer timer;
        timer.start();
        f.engine().startPlayback();
        QVERIFY(f.status.playback.active);

        // Paced: half-way through, about half has gone out, not all of it.
        QTest::qWait(int(fileMs / 2));
        const double midway = double(f.sink->samples.size() / 2) / double(fileFrames);
        qInfo() << "played" << midway * 100 << "% of the file after" << timer.elapsed() << "ms of" << fileMs;
        QVERIFY2(midway > 0.2 && midway < 0.8, qPrintable(QString::number(midway)));
        QTRY_VERIFY_WITH_TIMEOUT(heardAtEnd.has_value(), kTimeout);
        const qint64 took = timer.elapsed();
        qInfo() << "played" << fileMs << "ms of audio in" << took << "ms";
        // Never faster than the file, and not much slower.
        QVERIFY2(took >= fileMs - 20 && took < fileMs + 1000, qPrintable(QString::number(took)));
        window.stop();

        QCOMPARE(*heardAtEnd, file->frames.size());
        QVERIFY2(sameBits(f.sink->samples.data(), file->frames.data(), file->frames.size()),
                 "the speakers did not get the file's frames");
        QCOMPARE(scoped.size(), file->frames.size());
        QVERIFY2(sameBits(scoped.data(), file->frames.data(), file->frames.size()), "the scope missed the playback");
        QVERIFY(failures.isEmpty());
        // The chain, its meters and the recording kept running meanwhile: every block the radio
        // sent went through the chain and into the recording, less the few still on their way.
        // Measured against the stream, not the clock: on a busy machine the mock's timer falls
        // behind and sends less than real time, which PLAY, paced by the clock, does not.
        const double blockSeconds = double(f.mock.blockSamples / 2) / 48000.0;
        constexpr qint64 kInFlight = 5;
        const qint64 sentDuringPlay = sentAtEnd - sentBefore;
        const qint64 sentDuringRecording = sentAtEnd - sentAtRecording;
        qInfo() << "while playing, the radio sent" << sentDuringPlay << "blocks and the chain took"
                << chainBlocksAtEnd - blocksBefore << "; the recording holds" << recordedAtEnd << "s of"
                << double(sentDuringRecording) * blockSeconds << "s sent";
        QVERIFY2(sentDuringPlay > 20, qPrintable(QString::number(sentDuringPlay)));  // it did stream meanwhile
        QVERIFY2(qint64(chainBlocksAtEnd - blocksBefore) >= sentDuringPlay - kInFlight,
                 qPrintable(QStringLiteral("%1 of %2").arg(chainBlocksAtEnd - blocksBefore).arg(sentDuringPlay)));
        QVERIFY(f.status.recording.active);
        QVERIFY2(recordedAtEnd >= double(sentDuringRecording - kInFlight) * blockSeconds,
                 qPrintable(QStringLiteral("%1 s of %2 s").arg(recordedAtEnd).arg(double(sentDuringRecording) * blockSeconds)));
        f.engine().stopRecording();
        QTRY_COMPARE_WITH_TIMEOUT(f.status.lastRecordingPath, during, kTimeout);
        const std::optional<Recorded> live = readRecording(during);
        QVERIFY(live);
        QVERIFY(peakOf(live->frames) > 0.005f && peakOf(live->frames) < 0.05f);  // the live chain, turned down

        // And the live audio is back.
        QTRY_VERIFY_WITH_TIMEOUT(f.sink->samples.size() > *heardAtEnd + 4800, kTimeout);
        const std::vector<float> after(f.sink->samples.begin() + qsizetype(*heardAtEnd), f.sink->samples.end());
        QVERIFY(peakOf(after) < 0.05f);  // the live chain, turned down
        QVERIFY(!f.status.playback.active);
        f.engine().shutdown();
    }

    // Disconnected, PLAY opens the output for itself and closes it after, leaving ExpertSDR3's
    // mute alone: there is no session, so the mute policy wants nothing.
    void playWhileDisconnectedOpensAndClosesTheOutput()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.mock.muted, kTimeout);
        const QString path = f.dir.filePath(QStringLiteral("take.wav"));
        QVERIFY(recordFor(f, path, 0.5));
        const std::optional<Recorded> file = readRecording(path);
        QVERIFY(file);
        f.engine().disconnectRadio();
        QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Disconnected, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(!f.mock.muted, kTimeout);
        // The mock has seen the whole disconnect, its audio_stop included, once the socket has
        // closed at its end: anything it hears after that came from the playback.
        QTRY_VERIFY_WITH_TIMEOUT(!f.mock.hasClient(), kTimeout);
        QVERIFY(!f.status.outputOpen);
        const MuteController::Phase phase = f.status.mutePhase;
        const qsizetype commandsBefore = f.mock.commands.size();
        const int opens = f.sinkOpenCount;
        const int closes = f.sinkCloseCount;

        // At the end of the file the playback is over, but the output stays open for as long as
        // its buffer takes to play out: closing it at once would cut off the end.
        std::optional<EngineStatus> atEnd;
        std::optional<int> closesAtEnd;
        QElapsedTimer sinceEnd;
        qint64 closedAfterMs = -1;
        QObject::connect(&f.engine(), &Engine::statusChanged, &f.mock, [&](const EngineStatus& s) {
            if (!atEnd && !s.playback.active) {
                atEnd = s;
                closesAtEnd = f.sinkCloseCount;
                sinceEnd.start();
            }
            if (atEnd && closedAfterMs < 0 && !s.outputOpen) closedAfterMs = sinceEnd.elapsed();
        });
        f.engine().startPlayback();
        QVERIFY(f.status.playback.active);
        QCOMPARE(f.sinkOpenCount, opens + 1);
        QVERIFY(f.status.outputOpen);
        QTRY_VERIFY_WITH_TIMEOUT(atEnd.has_value(), kTimeout);
        QVERIFY(atEnd->outputOpen);
        QCOMPARE(*closesAtEnd, closes);
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.outputOpen, kTimeout);
        QCOMPARE(f.sinkCloseCount, closes + 1);
        QVERIFY2(closedAfterMs >= f.baseConfig().bufferMs, qPrintable(QString::number(closedAfterMs)));
        QCOMPARE(f.closedSamples.size(), file->frames.size());
        QVERIFY(sameBits(f.closedSamples.data(), file->frames.data(), file->frames.size()));

        QTest::qWait(200);
        QVERIFY2(f.mock.commands.size() == commandsBefore, qPrintable(f.mock.commands.mid(commandsBefore).join(u' ')));
        QVERIFY(!f.mock.hasClient());
        QCOMPARE(f.status.mutePhase, phase);
        QVERIFY(!f.mock.muted);
        f.engine().shutdown();
    }

    void playStopsOnASecondClickOnConnectAndWhenTheOutputFails()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        QVERIFY(recordFor(f, f.dir.filePath(QStringLiteral("take.wav")), 1.0));
        QStringList failures;
        QObject::connect(&f.engine(), &Engine::playbackFailed, &f.mock,
                         [&](const QString& reason) { failures << reason; });

        // A second click.
        f.engine().startPlayback();
        QTRY_VERIFY_WITH_TIMEOUT(f.status.playback.seconds > 0.1, kTimeout);
        f.engine().stopPlayback();
        QVERIFY(!f.status.playback.active);
        QVERIFY(failures.isEmpty());

        // The output failing.
        f.engine().startPlayback();
        QTRY_VERIFY_WITH_TIMEOUT(f.status.playback.seconds > 0.1, kTimeout);
        f.sink->isAlive = false;
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.playback.active, kTimeout);
        QCOMPARE(failures, QStringList{QStringLiteral("Output device stopped")});
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.outputOpen, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.outputOpen, 8000);  // the session's output comes back

        // Disconnecting.
        QTRY_VERIFY_WITH_TIMEOUT(f.status.outputOpen && f.audioFlowing(), kTimeout);
        f.engine().startPlayback();
        QVERIFY(f.status.playback.active);
        f.engine().disconnectRadio();
        QVERIFY(!f.status.playback.active);

        // Connecting, here from a playback with no radio.
        QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Disconnected, kTimeout);
        f.engine().startPlayback();
        QVERIFY(f.status.playback.active);
        f.engine().connectRadio();
        QVERIFY(!f.status.playback.active);
        QTRY_VERIFY_WITH_TIMEOUT(f.audioFlowing(), kTimeout);
        QVERIFY(f.status.outputOpen);
        QCOMPARE(failures.size(), 1);
        f.engine().shutdown();
    }

    void playRefusesWhatItCannotPlay()
    {
        Fixture f;
        QStringList failures;
        QObject::connect(&f.engine(), &Engine::playbackFailed, &f.mock,
                         [&](const QString& reason) { failures << reason; });
        f.engine().configure(f.baseConfig());
        f.engine().startPlayback();
        QVERIFY(!f.status.playback.active);
        QVERIFY(!f.status.lastRecordingAvailable);
        QCOMPARE(failures, QStringList{QStringLiteral("Nothing has been recorded yet")});
        QCOMPARE(f.sinkOpenCount, 0);

        // A recording at 48 kHz, then the stream changes to 24 kHz and the output with it.
        f.engine().connectRadio();
        QTRY_VERIFY_WITH_TIMEOUT(f.audioFlowing(), kTimeout);
        QVERIFY(recordFor(f, f.dir.filePath(QStringLiteral("take.wav")), 0.3));
        f.mock.operatorSetStreamRate(24000);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.chainRate, 24000, kTimeout);
        const int opens = f.sinkOpenCount;
        f.engine().startPlayback();
        QVERIFY(!f.status.playback.active);
        QCOMPARE(failures.size(), 2);
        QVERIFY2(failures.last().contains(QStringLiteral("48000")) && failures.last().contains(QStringLiteral("24000")),
                 qPrintable(failures.last()));
        QCOMPARE(f.sinkOpenCount, opens);
        f.engine().shutdown();
    }

    // PLAY pressed straight after REC stops, before the disk thread has finished the file, is
    // for that recording: it waits for it, showing as playing, rather than being refused or
    // playing the one before. A second click while it waits calls it off.
    void playPressedAsARecordingStopsWaitsForIt()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        QStringList failures;
        QObject::connect(&f.engine(), &Engine::playbackFailed, &f.mock,
                         [&](const QString& reason) { failures << reason; });
        double totalSeconds = 0.0;
        QObject::connect(&f.engine(), &Engine::statusChanged, &f.mock, [&](const EngineStatus& s) {
            if (s.playback.totalSeconds > 0.0) totalSeconds = s.playback.totalSeconds;
        });
        const QString older = f.dir.filePath(QStringLiteral("older.wav"));
        QVERIFY(recordFor(f, older, 0.3));

        const QString newer = f.dir.filePath(QStringLiteral("newer.wav"));
        f.engine().startRecordingToFile(newer);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds > 0.8, kTimeout);
        // The engine runs on this thread, so the disk thread's word that the file is finished
        // cannot have arrived between these lines.
        f.engine().stopRecording();
        f.engine().startPlayback();
        QVERIFY(failures.isEmpty());
        QVERIFY(f.status.playback.active);
        QVERIFY(f.status.playback.waiting);  // on, but nothing plays yet
        QCOMPARE(f.status.lastRecordingPath, older);  // the newer one is still being written

        QTRY_VERIFY_WITH_TIMEOUT(totalSeconds > 0.0, kTimeout);
        QVERIFY(!f.status.playback.waiting);
        QCOMPARE(f.status.lastRecordingPath, newer);
        const std::optional<WavFile::Info> info = WavFile::parseFile(newer);
        QVERIFY(info);
        QCOMPARE(totalSeconds, double(info->frames) / 48000.0);
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.playback.active, kTimeout);
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(u'\n')));

        // Called off while it waits: nothing plays.
        const QString third = f.dir.filePath(QStringLiteral("third.wav"));
        f.engine().startRecordingToFile(third);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds > 0.3, kTimeout);
        f.engine().stopRecording();
        f.engine().startPlayback();
        QVERIFY(f.status.playback.active && f.status.playback.waiting);
        f.engine().stopPlayback();
        QVERIFY(!f.status.playback.active && !f.status.playback.waiting);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.lastRecordingPath, third, kTimeout);
        QTest::qWait(200);
        QVERIFY(!f.status.playback.active);
        QVERIFY(failures.isEmpty());
        f.engine().shutdown();
    }

    // PLAY pressed while a disconnect waits for ExpertSDR3's mute to come back is turned down,
    // even with a recording still being written out: it does not wait, to play by itself once
    // the radio has gone.
    void playPressedWhileDisconnectingIsRefused()
    {
        Fixture f;
        f.start();
        QTRY_COMPARE_WITH_TIMEOUT(f.status.mutePhase, MuteController::Phase::Holding, kTimeout);
        QStringList failures;
        QObject::connect(&f.engine(), &Engine::playbackFailed, &f.mock,
                         [&](const QString& reason) { failures << reason; });
        const QString path = f.dir.filePath(QStringLiteral("take.wav"));
        f.engine().startRecordingToFile(path);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.recording.seconds > 0.3, kTimeout);

        // The engine runs on this thread, so neither the disk thread's word that the file is
        // finished nor the radio's answer to the mute can arrive between these lines.
        f.engine().stopRecording();
        f.engine().disconnectRadio();
        f.engine().startPlayback();
        QCOMPARE(failures, QStringList{QStringLiteral("Still disconnecting from the radio")});
        QVERIFY(!f.status.playback.active && !f.status.playback.waiting);

        QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Disconnected, kTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.lastRecordingPath, path, kTimeout);
        const int opens = f.sinkOpenCount;
        QTest::qWait(300);
        QVERIFY(!f.status.playback.active);
        QVERIFY(!f.status.outputOpen);
        QCOMPARE(f.sinkOpenCount, opens);  // nothing opened the output to play
        QCOMPARE(failures.size(), 1);
        f.engine().shutdown();
    }

    // While PLAY plays, the status's output peak and the output's freshness counter follow the
    // playback, not the live chain, with or without a radio: the window's output meter and
    // scope go by them.
    void theOutputMeterFollowsWhatPlays()
    {
        Fixture f;
        AppConfig config = f.baseConfig();
        config.params.insert(QStringLiteral("tube.enabled"), 1);
        config.params.insert(QStringLiteral("tube.drive"), 12);  // a loud recording
        f.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 4800, kTimeout);
        const QString path = f.dir.filePath(QStringLiteral("take.wav"));
        QVERIFY(recordFor(f, path, 1.0));
        const std::optional<Recorded> file = readRecording(path);
        QVERIFY(file);
        const float fileDb = 20.0f * std::log10(peakOf(file->frames));
        QVERIFY2(fileDb > -12.0f, qPrintable(QString::number(fileDb)));
        // The live audio turned well down.
        f.engine().setParam(QStringLiteral("tube.enabled"), 0);
        f.engine().setParam(QStringLiteral("eq.enabled"), 1);
        f.engine().setParam(QStringLiteral("eq.master"), -24);
        QTRY_VERIFY_WITH_TIMEOUT(f.status.outputPeakDb < -20.0f && f.status.outputPeakDb > -100.0f, kTimeout);

        const std::shared_ptr<LiveTaps> taps = f.engine().taps();
        float loudest = -120.0f;
        QObject::connect(&f.engine(), &Engine::statusChanged, &f.mock, [&](const EngineStatus& s) {
            if (s.playback.active) loudest = std::max(loudest, s.outputPeakDb);
        });
        const quint64 outBefore = taps->outBlocks.load();
        f.engine().startPlayback();
        QVERIFY(f.status.playback.active);
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.playback.active, kTimeout);
        QVERIFY2(loudest > fileDb - 1.0f, qPrintable(QStringLiteral("%1 against %2").arg(loudest).arg(fileDb)));
        QVERIFY(taps->outBlocks.load() > outBefore + 20);  // a second at a 10 ms pump: about 100

        // No radio: only PLAY feeds the output, and the counter moves for it alone.
        f.engine().disconnectRadio();
        QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Disconnected, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.outputOpen, kTimeout);
        const quint64 blocks = taps->blocks.load();
        const quint64 still = taps->outBlocks.load();
        QTest::qWait(100);
        QCOMPARE(taps->outBlocks.load(), still);  // nothing plays, so it stands still
        loudest = -120.0f;
        f.engine().startPlayback();
        QVERIFY(f.status.playback.active);
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.playback.active, kTimeout);
        QCOMPARE(taps->blocks.load(), blocks);  // no radio, so no chain
        QVERIFY2(taps->outBlocks.load() > still + 20, qPrintable(QString::number(taps->outBlocks.load() - still)));
        QVERIFY2(loudest > fileDb - 1.0f, qPrintable(QStringLiteral("%1 against %2").arg(loudest).arg(fileDb)));
        f.engine().shutdown();
    }

    // With no radio, the output PLAY opened for itself stays open after the file ends for as long
    // as the output says it holds, which underruns can have raised well above the buffer setting.
    void playWithNoRadioWaitsForWhatTheOutputHolds()
    {
        Fixture f;
        f.start();
        QTRY_VERIFY_WITH_TIMEOUT(f.audioFlowing(), kTimeout);
        QVERIFY(recordFor(f, f.dir.filePath(QStringLiteral("take.wav")), 0.5));
        f.engine().disconnectRadio();
        QTRY_COMPARE_WITH_TIMEOUT(f.status.connection, TciClient::State::Disconnected, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.outputOpen, kTimeout);

        QElapsedTimer clock;
        clock.start();
        std::optional<qint64> endedAt;
        qint64 closedAfterMs = -1;
        QObject::connect(&f.engine(), &Engine::statusChanged, &f.mock, [&](const EngineStatus& s) {
            if (!endedAt && !s.playback.active) endedAt = clock.elapsed();
            if (endedAt && closedAfterMs < 0 && !s.outputOpen) closedAfterMs = clock.elapsed() - *endedAt;
        });
        f.engine().startPlayback();
        QVERIFY(f.status.playback.active);
        constexpr double kGrownMs = 420.0;  // far past the 60 ms buffer setting
        f.sink->targetMs = kGrownMs;
        QTRY_VERIFY_WITH_TIMEOUT(endedAt.has_value(), kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(closedAfterMs >= 0, kTimeout);
        QVERIFY2(closedAfterMs >= qint64(kGrownMs), qPrintable(QString::number(closedAfterMs)));
        f.engine().shutdown();
    }

    // Neither REC nor PLAY says anything to the radio.
    void recAndPlaySendNothingToTheRadio()
    {
        Fixture f;
        f.start();
        QTRY_COMPARE_WITH_TIMEOUT(f.status.mutePhase, MuteController::Phase::Holding, kTimeout);
        QTest::qWait(300);  // nothing more is due from the mute policy
        const qsizetype before = f.mock.commands.size();

        const QString path = f.dir.filePath(QStringLiteral("take.wav"));
        QVERIFY(recordFor(f, path, 0.5));
        f.engine().startPlayback();
        QTRY_VERIFY_WITH_TIMEOUT(f.status.playback.active, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(!f.status.playback.active, kTimeout);
        f.engine().startPlayback();
        QTest::qWait(100);
        f.engine().stopPlayback();
        f.engine().startRecording(f.dir.filePath(QStringLiteral("Recordings")));
        QTest::qWait(300);
        f.engine().stopRecording();
        QTest::qWait(300);

        const QStringList sent = f.mock.commands.mid(before);
        QVERIFY2(radioChangingCommands(sent).isEmpty(), qPrintable(sent.join(u' ')));
        QVERIFY2(sent.isEmpty(), qPrintable(sent.join(u' ')));
        QVERIFY(f.mock.muted);  // still this app's audio
        f.engine().shutdown();
    }

    // --capture: the radio's audio and the chain's output, each from the first block, and a
    // rate change goes on in _part2 rather than starting the files again.
    void captureWritesBothSidesAndSurvivesARateChange()
    {
        Fixture f;
        AppConfig config = f.baseConfig();
        config.params.insert(QStringLiteral("tube.enabled"), 1);
        config.params.insert(QStringLiteral("tube.drive"), 12);
        f.engine().configure(config);
        const QString base = f.dir.filePath(QStringLiteral("cap"));
        f.engine().setCapture(base);
        f.engine().connectRadio();
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 24000, kTimeout);
        f.mock.operatorSetStreamRate(24000);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.chainRate, 24000, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(f.sink && f.sink->framesWritten > 12000, kTimeout);  // the 24 kHz output
        f.engine().shutdown();

        const QStringList names = {QStringLiteral("cap.before.wav"), QStringLiteral("cap.after.wav"),
                                   QStringLiteral("cap.before_part2.wav"), QStringLiteral("cap.after_part2.wav")};
        QStringList expected;
        for (const QString& n : names) expected << f.dir.filePath(n);
        QStringList reported = f.status.capture.files;
        reported.sort();
        expected.sort();
        QCOMPARE(reported, expected);
        QVERIFY(f.status.capture.error.isEmpty());

        std::vector<Recorded> files;
        for (const QString& n : names) {
            std::optional<Recorded> r = readRecording(f.dir.filePath(n));
            QVERIFY2(r, qPrintable(n));
            QVERIFY2(r->info.frames > 0, qPrintable(n));
            QCOMPARE(r->info.declaredFrames, r->info.framesInFile);
            files.push_back(std::move(*r));
        }
        QCOMPARE(files[0].info.sampleRate, 48000);
        QCOMPARE(files[1].info.sampleRate, 48000);
        QCOMPARE(files[2].info.sampleRate, 24000);
        QCOMPARE(files[3].info.sampleRate, 24000);
        // Before and after hold the same blocks: no NR changes their length here.
        QCOMPARE(files[0].info.frames, files[1].info.frames);
        QCOMPARE(files[2].info.frames, files[3].info.frames);
        // Before is the radio, from its very first sample (the mock's tone starts at sin 0, cos 1);
        // after is the chain.
        QCOMPARE(files[0].frames[0], 0.0f);
        QCOMPARE(files[0].frames[1], 0.2f);
        QVERIFY(peakOf(files[0].frames) <= 0.2001f);
        QVERIFY(peakOf(files[1].frames) > 0.25f);
    }

    void settingsRoundTrip()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("s.ini"));
        AppConfig config;
        config.host = QStringLiteral("10.0.0.5");
        config.port = 60001;
        config.receiver = 1;
        config.deviceKey = QStringLiteral("Core Audio|USB Audio");
        config.bufferMs = 120;
        config.outputGainDb = -6;
        config.muteExpert = false;
        config.nrMethod = QStringLiteral("rn2");
        config.stageOrder = {QStringLiteral("eq"), QStringLiteral("gate")};
        config.params.insert(QStringLiteral("comp.threshold"), -33.5);
        config.params.insert(QStringLiteral("eq.b4.gain"), 7.0);
        Settings(path).save(config);

        const AppConfig loaded = Settings(path).load();
        QCOMPARE(loaded.host, config.host);
        QCOMPARE(loaded.port, config.port);
        QCOMPARE(loaded.receiver, config.receiver);
        QCOMPARE(loaded.deviceKey, config.deviceKey);
        QCOMPARE(loaded.bufferMs, config.bufferMs);
        QCOMPARE(loaded.outputGainDb, config.outputGainDb);
        QCOMPARE(loaded.muteExpert, config.muteExpert);
        QCOMPARE(loaded.nrMethod, config.nrMethod);
        QCOMPARE(loaded.stageOrder, config.stageOrder);
        QCOMPARE(loaded.params.value(QStringLiteral("comp.threshold")), -33.5);
        QCOMPARE(loaded.params.value(QStringLiteral("eq.b4.gain")), 7.0);
    }

    // recording/folder: Documents/<display name>/Recordings until the operator picks one, and
    // kept apart from the audio settings, which a save rewrites.
    void theRecordingFolderDefaultsToDocumentsAndSurvivesASave()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("s.ini"));
        const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        const QString fallback = Settings::defaultRecordingFolder();
        QCOMPARE(fallback, QDir(documents.isEmpty() ? QDir::homePath() : documents)
                               .filePath(QStringLiteral("ExpertSDR3 Audio/Recordings")));
        QCOMPARE(Settings(path).recordingFolder(), fallback);

        const QString chosen = dir.filePath(QStringLiteral("My recordings"));
        Settings(path).setRecordingFolder(chosen);
        QCOMPARE(Settings(path).recordingFolder(), chosen);
        {
            Settings settings(path);
            settings.save(settings.load());  // what the window does on every change
        }
        QCOMPARE(Settings(path).recordingFolder(), chosen);
        QCOMPARE(QSettings(path, QSettings::IniFormat).value(QStringLiteral("recording/folder")).toString(), chosen);

        Settings(path).setRecordingFolder(QString());
        QCOMPARE(Settings(path).recordingFolder(), fallback);
        QVERIFY(!QSettings(path, QSettings::IniFormat).contains(QStringLiteral("recording/folder")));
    }

    void settingsRejectValuesOutOfRangeAndMethodsFromOtherBuilds()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("s.ini"));
        {
            QSettings raw(path, QSettings::IniFormat);
            raw.setValue(QStringLiteral("nr/method"), QStringLiteral("not-a-method"));
            raw.setValue(QStringLiteral("params/comp.threshold"), 999);
            raw.setValue(QStringLiteral("params/not.a.param"), 1);
            raw.setValue(QStringLiteral("chain/rate"), 12345);
        }
        const AppConfig loaded = Settings(path).load();
        QVERIFY(loaded.nrMethod.isEmpty());
        QCOMPARE(loaded.params.value(QStringLiteral("comp.threshold")), 0.0);  // clamped to max
        QVERIFY(!loaded.params.contains(QStringLiteral("not.a.param")));
        QCOMPARE(loaded.chainRate, 48000);
    }
};

QTEST_GUILESS_MAIN(TestEngineE2E)
#include "test_engine_e2e.moc"
