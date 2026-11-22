// A USB radio's audio, from its codec to the engine: the channel each receiver is on, the
// block size and rate, the trim, a codec that goes away or stalls, and the rules that keep the
// radio safe: no real input opens in a test, and the radio's own codec is never an output.
// Every codec here is a fake that produces audio in real time.
#include "app/Engine.h"
#include "app/Settings.h"
#include "audio/AudioSink.h"
#include "audio/PortAudioSource.h"
#include "radio/UsbBackend.h"
#include "radio/UsbModels.h"
#include "UsbFakes.h"

#include <QElapsedTimer>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

using namespace esa;
using namespace esa::testing;

namespace {

constexpr int kTimeout = 5000;

// Everything a backend hands on, block by block.
struct Heard {
    QList<int> receivers;
    QList<int> frames;
    QList<int> rates;
    std::vector<float> samples;  // stereo, all blocks together
};

void listen(UsbBackend& backend, Heard& heard)
{
    QObject::connect(&backend, &RadioBackend::audioBlock, &backend,
                     [&heard](int receiver, const float* stereo, int frames, int rate) {
                         heard.receivers << receiver;
                         heard.frames << frames;
                         heard.rates << rate;
                         heard.samples.insert(heard.samples.end(), stereo, stereo + size_t(frames) * 2);
                     });
}

} // namespace

class TestCapture : public QObject {
    Q_OBJECT

private slots:
    // ── The backend ──────────────────────────────────────────────────────────

    // On the TS-990S in Normal mode the main receiver is the left channel and the sub the
    // right, and the one chosen arrives in both channels.
    void mainIsLeftAndSubIsRight()
    {
        auto codec = std::make_shared<Codec>();
        UsbBackend backend(fakeCodec(codec));
        Heard heard;
        listen(backend, heard);
        backend.requestAudio(0, 48000);
        backend.open(usbConfig());
        QCOMPARE(backend.state(), RadioBackend::Link::Ready);
        QCOMPARE(codec->lastKey, QStringLiteral("Test|USB Audio CODEC"));
        QTRY_VERIFY_WITH_TIMEOUT(heard.samples.size() >= 4800, kTimeout);
        for (float v : heard.samples) QVERIFY2(std::abs(v - 0.25f) < 1e-6f, qPrintable(QString::number(v)));
        QVERIFY(std::all_of(heard.receivers.cbegin(), heard.receivers.cend(), [](int r) { return r == 0; }));

        heard = Heard();
        backend.requestAudio(1, 48000);
        QTRY_VERIFY_WITH_TIMEOUT(heard.samples.size() >= 4800, kTimeout);
        for (float v : heard.samples) QVERIFY2(std::abs(v + 0.5f) < 1e-6f, qPrintable(QString::number(v)));
        QVERIFY(std::all_of(heard.receivers.cbegin(), heard.receivers.cend(), [](int r) { return r == 1; }));

        // Its name, for the window.
        QCOMPARE(backend.radio().device, QStringLiteral("TS-990S"));
        QCOMPARE(backend.radio().receivers.value(1).name, QStringLiteral("Sub"));
        QVERIFY(backend.radio().receiveOnly);
        backend.close();
        QCOMPARE(backend.state(), RadioBackend::Link::Disconnected);
    }

    // About 10.7 ms a block, as TCI's are, at either chain rate. At 24 kHz a 48 kHz codec is
    // converted, keeping the pitch.
    void blocksAreTenMillisecondsAtTheChainsRate_data()
    {
        QTest::addColumn<int>("chainRate");
        QTest::addColumn<int>("blockFrames");
        QTest::newRow("48 kHz") << 48000 << 512;
        QTest::newRow("24 kHz") << 24000 << 256;
    }

    void blocksAreTenMillisecondsAtTheChainsRate()
    {
        QFETCH(int, chainRate);
        QFETCH(int, blockFrames);
        auto codec = std::make_shared<Codec>();
        codec->left = 0.0f;
        codec->toneHz = 1000.0;
        UsbBackend backend(fakeCodec(codec));
        Heard heard;
        listen(backend, heard);
        backend.requestAudio(0, chainRate);
        backend.open(usbConfig());
        QTRY_VERIFY_WITH_TIMEOUT(heard.samples.size() >= size_t(chainRate) * 2, kTimeout);  // a second
        QVERIFY(std::all_of(heard.frames.cbegin(), heard.frames.cend(), [&](int f) { return f == blockFrames; }));
        QVERIFY(std::all_of(heard.rates.cbegin(), heard.rates.cend(), [&](int r) { return r == chainRate; }));
        // 1000 Hz is 2000 zero crossings a second, at whatever rate it arrives. Skip the
        // converter's start.
        const size_t skip = size_t(chainRate / 10) * 2;
        int crossings = 0;
        for (size_t i = skip + 2; i + 1 < size_t(chainRate) * 2; i += 2) {
            if ((heard.samples[i - 2] < 0.0f) != (heard.samples[i] < 0.0f)) ++crossings;
        }
        const double perSecond = crossings / (double(chainRate - chainRate / 10) / chainRate);
        QVERIFY2(std::abs(perSecond - 2000.0) < 40.0, qPrintable(QString::number(perSecond)));
    }

    void theTrimScalesTheAudio()
    {
        auto codec = std::make_shared<Codec>();
        UsbBackend backend(fakeCodec(codec));
        Heard heard;
        listen(backend, heard);
        AppConfig config = usbConfig();
        config.usbTrimDb = -6.0;
        backend.requestAudio(0, 48000);
        backend.open(config);
        QTRY_VERIFY_WITH_TIMEOUT(heard.samples.size() >= 2048, kTimeout);
        const float expected = 0.25f * float(std::pow(10.0, -6.0 / 20.0));
        QVERIFY2(std::abs(heard.samples.back() - expected) < 1e-5f, qPrintable(QString::number(heard.samples.back())));
        heard = Heard();
        backend.setInputTrimDb(6.0);
        QTRY_VERIFY_WITH_TIMEOUT(heard.samples.size() >= 2048, kTimeout);
        QVERIFY(std::abs(heard.samples.back() - 0.25f * float(std::pow(10.0, 6.0 / 20.0))) < 1e-5f);
        backend.setInputTrimDb(std::nan(""));  // ignored
        heard = Heard();
        QTRY_VERIFY_WITH_TIMEOUT(heard.samples.size() >= 2048, kTimeout);
        QVERIFY(std::isfinite(heard.samples.back()));
    }

    // With no input chosen nothing opens: never this computer's default input, which would be
    // its microphone taken for the radio.
    void withNoInputChosenNothingIsHeard()
    {
        auto codec = std::make_shared<Codec>();
        UsbBackend backend(fakeCodec(codec));
        AppConfig config = usbConfig();
        config.usbInput.clear();
        backend.open(config);
        QCOMPARE(backend.state(), RadioBackend::Link::Connecting);
        QCOMPARE(codec->lastKey, QString());
        QVERIFY(!backend.lastError().isEmpty());
        QCOMPARE(backend.input().device, QString());
        backend.close();
    }

    // Unplugged, or the radio switched off: looked for again until it comes back.
    void aLostCodecIsFoundAgain()
    {
        auto codec = std::make_shared<Codec>();
        UsbBackend backend(fakeCodec(codec));
        QSignalSpy states(&backend, &RadioBackend::stateChanged);
        backend.open(usbConfig());
        QCOMPARE(backend.state(), RadioBackend::Link::Ready);
        codec->present = false;
        codec->alive = false;
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), RadioBackend::Link::Connecting, kTimeout);
        QVERIFY(backend.lastError().contains(QLatin1String("stopped")));
        const int opens = codec->opens;
        codec->present = true;
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), RadioBackend::Link::Ready, kTimeout);
        QVERIFY(codec->opens > opens);
        QCOMPARE(backend.lastError(), QString());
        backend.close();
    }

    // A codec that stops delivering without reporting an error (macOS keeps a stream "active"
    // after its device is gone) is caught by its callbacks no longer advancing.
    void aStalledCodecCountsAsLost()
    {
        auto codec = std::make_shared<Codec>();
        UsbBackend backend(fakeCodec(codec));
        backend.open(usbConfig());
        QCOMPARE(backend.state(), RadioBackend::Link::Ready);
        codec->stalled = true;
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), RadioBackend::Link::Connecting, kTimeout);
        QVERIFY2(backend.lastError().contains(QLatin1String("stopped delivering")), qPrintable(backend.lastError()));
        backend.close();
    }

    void lostAudioIsCounted()
    {
        auto codec = std::make_shared<Codec>();
        UsbBackend backend(fakeCodec(codec));
        backend.open(usbConfig());
        codec->overflows = 3;
        QCOMPARE(backend.input().overflows, quint64(3));
        QCOMPARE(backend.input().device, QStringLiteral("Test codec"));
        QCOMPARE(backend.input().sampleRate, 48000);
        backend.close();
    }

    // ── Safety ───────────────────────────────────────────────────────────────

    // No test may open a real input: not a microphone, not a radio's codec, and not the macOS
    // permission prompt. The backend's own way of opening devices refuses under the sandbox.
    void noRealInputOpensInATest()
    {
        QVERIFY(!audio::realInputRefusal().isEmpty());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("^Test sandbox: refused to open a real input")));
        QString error;
        QVERIFY(!PortAudioSource::open(QStringLiteral("Core Audio|USB Audio CODEC"), 48000, &error));
        QVERIFY2(error.contains(QLatin1String("ESA_TEST_SANDBOX")), qPrintable(error));

        UsbBackend backend;  // real devices
        backend.open(usbConfig());
        QCOMPARE(backend.state(), RadioBackend::Link::Connecting);
        QVERIFY2(backend.lastError().contains(QLatin1String("ESA_TEST_SANDBOX")), qPrintable(backend.lastError()));
        backend.close();
    }

    // One device's two ends are one physical device: the radio's codec, never to be played
    // into. On Windows its ends carry different names.
    void bothEndsOfACodecAreOneDevice_data()
    {
        QTest::addColumn<QString>("a");
        QTest::addColumn<QString>("b");
        QTest::addColumn<bool>("same");
        QTest::newRow("macOS, one name") << "Core Audio|USB Audio CODEC" << "Core Audio|USB Audio CODEC" << true;
        QTest::newRow("Windows ends") << "MME|Microphone (USB Audio CODEC)" << "MME|Speakers (USB Audio CODEC)" << true;
        QTest::newRow("Windows numbered") << "Windows WASAPI|Microphone (2- USB Audio CODEC)"
                                          << "Windows WASAPI|Speakers (2- USB Audio CODEC)" << true;
        QTest::newRow("across host APIs") << "MME|Microphone (USB Audio CODEC)"
                                          << "Windows DirectSound|Speakers (USB Audio CODEC )" << true;
        QTest::newRow("different devices") << "Core Audio|USB Audio CODEC" << "Core Audio|Mac mini Speakers" << false;
        QTest::newRow("nothing chosen") << "" << "Core Audio|USB Audio CODEC" << false;
    }

    void bothEndsOfACodecAreOneDevice()
    {
        QFETCH(QString, a);
        QFETCH(QString, b);
        QFETCH(bool, same);
        QCOMPARE(audio::samePhysicalDevice(a, b), same);
        QCOMPARE(audio::samePhysicalDevice(b, a), same);
    }

    // ── Through the engine ───────────────────────────────────────────────────

    void anEngineHearsAUsbRadio()
    {
        QTemporaryDir dir;
        auto codec = std::make_shared<Codec>();
        auto played = std::make_shared<std::atomic<quint64>>(0);
        Engine engine(dir.filePath(QStringLiteral("s.ini")),
                      [played](const AppConfig&, QString*) { return std::make_unique<CountingSink>(played); });
        engine.setBackendFactory([codec](const QString& kind, QObject* parent) -> RadioBackend* {
            if (kind == QLatin1String("usb")) return new UsbBackend(fakeCodec(codec), parent);
            return nullptr;
        });
        EngineStatus status;
        QObject::connect(&engine, &Engine::statusChanged, &engine, [&](const EngineStatus& s) { status = s; });
        engine.configure(usbConfig());
        engine.connectRadio();
        QTRY_VERIFY_WITH_TIMEOUT(played->load() > 24000, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(status.audioFlowing, kTimeout);
        QCOMPARE(status.connection, RadioBackend::Link::Ready);
        QCOMPARE(status.radioKind, QStringLiteral("usb"));
        QCOMPARE(status.device, QStringLiteral("TS-990S"));
        QCOMPARE(status.receiverName, QStringLiteral("Main"));
        QCOMPARE(status.inputDevice, QStringLiteral("Test codec"));
        QCOMPARE(status.inputRate, 48000);
        QCOMPARE(status.connectedPort, 0);
        // Without CAT this app cannot silence the radio, so the mute is never asked for.
        QCOMPARE(status.mutePhase, MuteController::Phase::Idle);
        QVERIFY(status.inputPeakDb > -20.0f);

        engine.disconnectRadio();
        QTRY_COMPARE_WITH_TIMEOUT(status.connection, RadioBackend::Link::Disconnected, kTimeout);
        engine.shutdown();
    }

    // The codec streams silence when the radio is off, which is no radio at all.
    void silenceIsNoRadio()
    {
        QTemporaryDir dir;
        auto codec = std::make_shared<Codec>();
        codec->left = 0.0f;
        codec->right = 0.0f;
        auto played = std::make_shared<std::atomic<quint64>>(0);
        Engine engine(dir.filePath(QStringLiteral("s.ini")),
                      [played](const AppConfig&, QString*) { return std::make_unique<CountingSink>(played); });
        engine.setBackendFactory([codec](const QString&, QObject* parent) -> RadioBackend* {
            return new UsbBackend(fakeCodec(codec), parent);
        });
        EngineStatus status;
        QObject::connect(&engine, &Engine::statusChanged, &engine, [&](const EngineStatus& s) { status = s; });
        engine.configure(usbConfig());
        engine.connectRadio();
        QTRY_VERIFY_WITH_TIMEOUT(played->load() > 24000, kTimeout);
        QTest::qWait(300);
        QCOMPARE(status.connection, RadioBackend::Link::Ready);
        QVERIFY(!status.audioFlowing);
        // Sound arriving is the radio, and the guard drops it again after three silent seconds.
        codec->left = 0.1f;
        QTRY_VERIFY_WITH_TIMEOUT(status.audioFlowing, kTimeout);
        codec->left = 0.0f;
        QTRY_VERIFY_WITH_TIMEOUT(!status.audioFlowing, kTimeout);
        engine.shutdown();
    }

    // With DATA VOX on, the radio can transmit what is played into its own codec, so the
    // output refuses it, however it was chosen.
    void theRadiosCodecIsNeverTheOutput_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("output");
        QTest::newRow("macOS") << "Core Audio|USB Audio CODEC" << "Core Audio|USB Audio CODEC";
        QTest::newRow("Windows") << "MME|Microphone (USB Audio CODEC)" << "MME|Speakers (USB Audio CODEC)";
    }

    void theRadiosCodecIsNeverTheOutput()
    {
        QFETCH(QString, input);
        QFETCH(QString, output);
        QTemporaryDir dir;
        auto codec = std::make_shared<Codec>();
        int sinksOpened = 0;
        Engine engine(dir.filePath(QStringLiteral("s.ini")), [&sinksOpened](const AppConfig&, QString*) {
            ++sinksOpened;
            return std::make_unique<CountingSink>(std::make_shared<std::atomic<quint64>>(0));
        });
        engine.setBackendFactory([codec](const QString&, QObject* parent) -> RadioBackend* {
            return new UsbBackend(fakeCodec(codec), parent);
        });
        EngineStatus status;
        QObject::connect(&engine, &Engine::statusChanged, &engine, [&](const EngineStatus& s) { status = s; });
        AppConfig config = usbConfig();
        config.usbInput = input;
        config.deviceKey = output;
        engine.configure(config);
        engine.connectRadio();
        QTRY_VERIFY_WITH_TIMEOUT(!status.outputError.isEmpty(), kTimeout);
        QCOMPARE(sinksOpened, 0);
        QVERIFY(!status.outputOpen);
        QVERIFY2(status.outputError.contains(QLatin1String("radio's own audio")), qPrintable(status.outputError));
        // Choosing another output plays again.
        engine.setOutputDevice(QStringLiteral("Test|Speakers"));
        QCOMPARE(sinksOpened, 1);
        engine.shutdown();
    }

    // ── Settings ─────────────────────────────────────────────────────────────

    // A file from before USB radios is an ExpertSDR3 one, and a USB radio's choices come back.
    void theRadioIsKeptInTheSettings()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("old.ini"));
        {
            QSettings old(path, QSettings::IniFormat);
            old.setValue(QStringLiteral("radio/host"), QStringLiteral("192.0.2.5"));
        }
        QCOMPARE(Settings(path).load().radioKind, QStringLiteral("tci"));

        AppConfig config = usbConfig();
        config.usbTrimDb = -4.5;
        config.receiver = 1;
        Settings(path).save(config);
        const AppConfig back = Settings(path).load();
        QCOMPARE(back.radioKind, QStringLiteral("usb"));
        QCOMPARE(back.usbModel, QStringLiteral("ts990s"));
        QCOMPARE(back.usbInput, QStringLiteral("Test|USB Audio CODEC"));
        QCOMPARE(back.usbTrimDb, -4.5);
        QCOMPARE(back.receiver, 1);
        QCOMPARE(back.host, QStringLiteral("192.0.2.1"));

        // What a damaged or hand-edited file may hold.
        {
            QSettings edited(path, QSettings::IniFormat);
            edited.setValue(QStringLiteral("radio/kind"), QStringLiteral("carrier-pigeon"));
            edited.setValue(QStringLiteral("radio/usb/model"), QStringLiteral("ts-9999"));
            edited.setValue(QStringLiteral("radio/usb/trimDb"), QStringLiteral("nan"));
        }
        const AppConfig edited = Settings(path).load();
        QCOMPARE(edited.radioKind, QStringLiteral("tci"));
        QCOMPARE(edited.usbModel, QStringLiteral("generic"));
        QCOMPARE(edited.usbTrimDb, 0.0);
    }

    void theTs990sIsKnown()
    {
        const UsbModel& ts990 = usb::model(QStringLiteral("ts990s"));
        QCOMPARE(ts990.label(), QStringLiteral("Kenwood TS-990S"));
        QCOMPARE(ts990.receivers, QStringList({QStringLiteral("Main"), QStringLiteral("Sub")}));
        QCOMPARE(ts990.channels, QList<int>({0, 1}));
        QCOMPARE(usb::model(QStringLiteral("unknown")).id, QStringLiteral("generic"));
    }
};

QTEST_GUILESS_MAIN(TestCapture)
#include "test_capture.moc"
