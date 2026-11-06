#include "MockExpert.h"

#include "app/Engine.h"
#include "audio/AudioSink.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSettings>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

using namespace esa;

namespace {

// Records what would have reached the speakers. Also a stand-in for an unpluggable
// device, which no real test machine offers on demand.
class RecordingSink final : public AudioSink {
public:
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
        return s;
    }
    void setGainDb(double) override {}
    void setBoost(bool) override {}
    void setMuted(bool) override {}
    void setTargetLatencyMs(int) override {}

    std::vector<float> samples;
    quint64 framesWritten{0};
    bool isAlive{true};
};

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
                    sink = created.get();
                    return created;
                });
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
    EngineStatus status;
    std::unique_ptr<Engine> m_engine;
};

constexpr int kTimeout = 5000;

// One settings store for one test: an ini file like the other tests use, or the native store
// that real installs use (a plist, the registry), under a name no real install has.
class SettingsStore {
public:
    explicit SettingsStore(bool native) : m_native(native) { clear(); }
    // The native store keeps an empty entry under this fixed name. Deleting that file as well would
    // only race a Mac's preferences daemon, which writes it back.
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
    QTemporaryDir m_dir;
};

void addEachStore()
{
    QTest::addColumn<bool>("native");
    QTest::newRow("ini file") << false;
    QTest::newRow("native store") << true;
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
        QTest::qWait(100);

        f.mock.operatorSetMute(false);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.mutePhase, MuteController::Phase::OperatorOverride, kTimeout);
        QTest::qWait(700);
        QCOMPARE(f.mock.muteCommandsSent(true), 1);  // not fought
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
        QTest::qWait(100);
        f.mock.operatorSetVolume(-12);
        QTRY_COMPARE_WITH_TIMEOUT(f.status.mutePhase, MuteController::Phase::OperatorOverride, kTimeout);
        QTest::qWait(600);
        QVERIFY(!f.mock.muted);
        QCOMPARE(f.mock.muteCommandsSent(true), 1);
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
        const SettingsStore store(native);
        store.writeOldMuteRecord(true, false);
        QVERIFY(store.open()->loadMuteOwnership(AppConfig{}.host).owned);
    }

    // What nearly every upgrade looks like: v0.1.0 also wrote the record when it owed nothing.
    void anOldMuteRecordOwingNothingLeavesNothingBehind_data() { addEachStore(); }
    void anOldMuteRecordOwingNothingLeavesNothingBehind()
    {
        QFETCH(bool, native);
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
        const int opensBefore = f.sinkOpenCount;
        f.engine().setOutputDevice(QStringLiteral("another device"));
        QCOMPARE(f.sinkOpenCount, opensBefore + 1);
        QTest::qWait(300);
        QCOMPARE(f.mock.muteCommandsSent(false), 0);
        QCOMPARE(f.mock.muteCommandsSent(true), 1);
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
        QVERIFY(silent.listen(QHostAddress::LocalHost, 0));
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
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const int closedPort = probe.serverPort();
        probe.close();  // reserved a number nothing is listening on

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
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const int closedPort = probe.serverPort();
        probe.close();

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
