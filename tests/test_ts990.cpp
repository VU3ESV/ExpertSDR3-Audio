// The Kenwood TS-990S's CAT, against a mock radio: what may be sent to it (an allowlist, and
// nothing else whatever asks), identifying it, following its frequency and mode, a radio that
// stops answering or is unplugged, the AF level read and set, and the Radio page's Test. No
// test here opens a real serial port: the sandbox refuses them, and that is tested too.
#include "app/Engine.h"
#include "app/TestSandbox.h"
#include "dsp/NrProcessor.h"
#include "radio/CatTransport.h"
#include "radio/Ts990Cat.h"
#include "radio/UsbBackend.h"
#include "radio/UsbModels.h"
#include "UsbFakes.h"

#include <QFile>
#include <QPointer>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace esa;
using namespace esa::testing;

namespace {

constexpr int kTimeout = 5000;
const QString kPort = QStringLiteral("mock-ts990");

class MockTs990;

// The radio, as the test controls it, shared with whatever port the driver opened.
struct Radio {
    QByteArray id{"022"};
    bool present{true};   // the port opens
    bool answers{true};   // switched on
    bool busy{false};     // answers ?; to everything
    bool knobHolds{false}; // the operator holds the AF knob: a set does not take
    int chunk{0};         // > 0: replies arrive this many bytes at a time
    qint64 fa{14074000};
    qint64 fb{7030000};
    bool blankFb{false};  // a memory channel with nothing stored
    char om0{'2'};        // USB
    char om1{'3'};        // CW
    int af{120};

    QList<QByteArray> received;  // every command, in order
    int opens{0};
    bool open{false};
    QString lastPort;
    int lastBaud{0};
    QPointer<MockTs990> port;
};

// The radio's end of the wire: answers as the TS-990S's reference says it does.
class MockTs990 final : public CatTransport {
public:
    MockTs990(std::shared_ptr<Radio> radio, QObject* parent) : CatTransport(parent), m_radio(std::move(radio))
    {
        m_radio->port = this;
    }
    ~MockTs990() override { m_radio->open = false; }

    bool open(const QString& port, int baud, QString* error) override
    {
        ++m_radio->opens;
        m_radio->lastPort = port;
        m_radio->lastBaud = baud;
        if (!m_radio->present) {
            if (error) *error = QStringLiteral("%1: no such port").arg(port);
            return false;
        }
        m_radio->open = true;
        return true;
    }
    void close() override { m_radio->open = false; }
    bool isOpen() const override { return m_radio->open; }

    void write(const QByteArray& bytes) override
    {
        QVERIFY2(m_radio->open, "written while closed");
        for (const QByteArray& part : bytes.split(';')) {
            if (part.isEmpty()) continue;
            const QByteArray command = part + ';';
            m_radio->received << command;
            if (m_radio->answers) m_out += reply(part);
        }
        if (!m_out.isEmpty()) QTimer::singleShot(1, this, [this] { deliver(); });
    }

    QByteArray readAll() override
    {
        const qsizetype take = m_radio->chunk > 0 ? std::min<qsizetype>(m_radio->chunk, m_out.size()) : m_out.size();
        const QByteArray out = m_out.left(take);
        m_out.remove(0, take);
        return out;
    }

    // Unplugged, or the radio switched off.
    void unplug()
    {
        m_radio->open = false;
        m_radio->present = false;
        emit lost(QStringLiteral("unplugged"));
    }

private:
    void deliver()
    {
        while (!m_out.isEmpty() && m_radio->open) emit readyRead();
    }

    QByteArray reply(const QByteArray& command) const
    {
        const Radio& r = *m_radio;
        if (r.busy) return QByteArrayLiteral("?;");
        auto frequency = [](const char* name, qint64 hz) {
            return QByteArray(name) + QByteArray::number(hz).rightJustified(11, '0') + ';';
        };
        if (command == "ID") return "ID" + r.id + ';';
        if (command == "FA") return frequency("FA", r.fa);
        if (command == "FB") return r.blankFb ? QByteArrayLiteral("FB           ;") : frequency("FB", r.fb);
        if (command == "OM0") return QByteArrayLiteral("OM0") + r.om0 + ';';
        if (command == "OM1") return QByteArrayLiteral("OM1") + r.om1 + ';';
        if (command == "AG0") return "AG0" + QByteArray::number(r.af).rightJustified(3, '0') + ';';
        if (command.size() == 6 && command.startsWith("AG0")) {
            if (!r.knobHolds) m_radio->af = command.mid(3).toInt();
            return QByteArray();  // a set has no answer
        }
        return QByteArrayLiteral("?;");
    }

    std::shared_ptr<Radio> m_radio;
    QByteArray m_out;
};

Ts990Cat::TransportFactory mockRadio(const std::shared_ptr<Radio>& radio)
{
    return [radio](QObject* parent) -> std::unique_ptr<CatTransport> {
        return std::make_unique<MockTs990>(radio, parent);
    };
}

QList<QByteArray> distinct(const QList<QByteArray>& commands)
{
    QList<QByteArray> out;
    for (const QByteArray& c : commands) {
        if (!out.contains(c)) out << c;
    }
    return out;
}

AppConfig catConfig()
{
    AppConfig c = usbConfig();
    c.usbCatPort = kPort;
    c.usbCatBaud = 115200;
    c.nrMethod = QStringLiteral("nr2");
    c.nrAutoBypass = true;
    return c;
}

// The engine, with a fake codec and the mock radio on its CAT port, and its every status.
struct Station {
    QTemporaryDir dir;
    std::shared_ptr<Radio> radio = std::make_shared<Radio>();
    std::shared_ptr<Codec> codec = std::make_shared<Codec>();
    std::shared_ptr<std::atomic<quint64>> played = std::make_shared<std::atomic<quint64>>(0);
    std::unique_ptr<Engine> engine;
    EngineStatus status;

    QString settingsPath() const { return dir.filePath(QStringLiteral("s.ini")); }

    void start(AppConfig config)
    {
        auto playedCount = played;
        engine = std::make_unique<Engine>(settingsPath(), [playedCount](const AppConfig&, QString*) {
            return std::make_unique<CountingSink>(playedCount);
        });
        engine->setBackendFactory([this](const QString& kind, QObject* parent) -> RadioBackend* {
            if (kind == QLatin1String("usb")) return new UsbBackend(fakeCodec(codec), parent, mockRadio(radio));
            return nullptr;
        });
        QObject::connect(engine.get(), &Engine::statusChanged, engine.get(), [this](const EngineStatus& s) { status = s; });
        engine->configure(config);
        engine->connectRadio();
    }

    Settings::MuteOwnership owed() const
    {
        return Settings(settingsPath()).loadMuteOwnership(QStringLiteral("usb:ts990s"));
    }
};

} // namespace

class TestTs990 : public QObject {
    Q_OBJECT

private slots:
    // ── What may be sent ─────────────────────────────────────────────────────

    void theAllowlistIsExact_data()
    {
        QTest::addColumn<QByteArray>("command");
        QTest::addColumn<bool>("allowed");
        for (const char* ok : {"ID;", "FA;", "FB;", "OM0;", "OM1;", "AG0;", "AG0000;", "AG0001;", "AG0128;", "AG0255;"}) {
            QTest::newRow(ok) << QByteArray(ok) << true;
        }
        // Transmit, receive, auto-information, menus, keying, mute, power, the sub AF, setting the
        // frequency or mode, levels out of range, malformed commands and two commands at once.
        for (const char* no : {"TX;", "TX0;", "TX1;", "TX2;", "RX;", "AI;", "AI1;", "AI2;", "AI0;", "EX;",
                               "EX00603001;", "KY;", "KY CQ;", "MU;", "MU1;", "PS;", "PS0;", "SR;", "AG1;",
                               "AG1000;", "AG0256;", "AG0999;", "AG000;", "AG00000;", "AG0-01;", "AG0+01;",
                               "AG0 12;", "ag0;", "id;", "FA00014074000;", "FB00007030000;", "OM02;", "OM13;",
                               "ID", "ID;;", ";", "", "FA;FB;", "ID;TX;", "\nID;", "ID;\n"}) {
            QTest::newRow(no) << QByteArray(no) << false;
        }
    }

    void theAllowlistIsExact()
    {
        QFETCH(QByteArray, command);
        QFETCH(bool, allowed);
        QCOMPARE(Ts990Cat::allowed(command), allowed);
    }

    // The driver's own source names no command but these. A new one has to be added here, on
    // purpose, with the allowlist beside it.
    void theDriverSendsNothingElse()
    {
        QFile source(QStringLiteral(ESA_SOURCE_DIR "/src/radio/Ts990Cat.cpp"));
        QVERIFY2(source.open(QIODevice::ReadOnly), qPrintable(source.fileName()));
        const QString text = QString::fromUtf8(source.readAll());
        const QSet<QString> known = {QStringLiteral("ID;"),  QStringLiteral("FA;"),  QStringLiteral("FB;"),
                                     QStringLiteral("OM0;"), QStringLiteral("OM1;"), QStringLiteral("AG0;"),
                                     QStringLiteral("AG0%1;")};
        static const QRegularExpression literal(QStringLiteral("\"([^\"\\n]*;)\""));
        int found = 0;
        for (auto it = literal.globalMatch(text); it.hasNext();) {
            const QString command = it.next().captured(1);
            QVERIFY2(known.contains(command), qPrintable(QStringLiteral("Ts990Cat.cpp names %1").arg(command)));
            ++found;
        }
        QVERIFY(found >= 7);
        // Commands are checked as they are queued, and one place writes to the port, checking
        // them again.
        QCOMPARE(text.count(QStringLiteral("->write(")), 1);
        QCOMPARE(text.count(QStringLiteral("if (!allowed(command))")), 1);
        QCOMPARE(text.count(QStringLiteral("if (!allowed(command) || (m_identifyOnly && command != \"ID;\"))")), 1);
        const qsizetype writer = text.indexOf(QStringLiteral("void Ts990Cat::writeCommand"));
        QVERIFY(writer > 0);
        QVERIFY(text.indexOf(QStringLiteral("->write("), writer) > writer);
    }

    // ── Identifying ──────────────────────────────────────────────────────────

    void aTs990IsIdentifiedAndRead()
    {
        auto radio = std::make_shared<Radio>();
        Ts990Cat cat(mockRadio(radio));
        QSignalSpy bands(&cat, &Ts990Cat::bandChanged);
        cat.open(kPort, 115200);
        QCOMPARE(radio->lastPort, kPort);
        QCOMPARE(radio->lastBaud, 115200);
        QTRY_COMPARE_WITH_TIMEOUT(cat.state(), Ts990Cat::State::Ready, kTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(cat.band(1).mode, QStringLiteral("CW"), kTimeout);
        QCOMPARE(cat.band(0).frequencyHz, 14074000);
        QCOMPARE(cat.band(0).mode, QStringLiteral("USB"));
        QCOMPARE(cat.band(1).frequencyHz, 7030000);
        QVERIFY(bands.count() >= 4);
        QCOMPARE(radio->received.first(), QByteArray("ID;"));
        QCOMPARE(distinct(radio->received),
                 (QList<QByteArray>{"ID;", "FA;", "OM0;", "FB;", "OM1;"}));
        QVERIFY(cat.lastError().isEmpty());

        // It keeps reading, and nothing new is ever sent.
        const qsizetype before = radio->received.size();
        QTRY_VERIFY_WITH_TIMEOUT(radio->received.size() >= before + 8, kTimeout);
        for (const QByteArray& c : std::as_const(radio->received)) QVERIFY2(Ts990Cat::allowed(c), c.constData());
        QCOMPARE(distinct(radio->received).size(), 5);

        cat.close();
        QCOMPARE(cat.state(), Ts990Cat::State::Closed);
        QVERIFY(!radio->open);
    }

    // The Radio page's Test asks who it is, and nothing more.
    void identifyOnlySendsIdAlone()
    {
        auto radio = std::make_shared<Radio>();
        Ts990Cat cat(mockRadio(radio));
        cat.open(kPort, 115200, true);
        QTRY_COMPARE_WITH_TIMEOUT(cat.state(), Ts990Cat::State::Ready, kTimeout);
        QTest::qWait(3 * Ts990Cat::kPollMs);
        QCOMPARE(radio->received, (QList<QByteArray>{"ID;"}));
        cat.setAfLevel(0);
        cat.readAfLevel();
        QTest::qWait(100);
        QCOMPARE(radio->received, (QList<QByteArray>{"ID;"}));
    }

    // A TS-2000 or any other radio is not read, nor sent anything, nor asked again.
    void anotherRadioIsRefused()
    {
        auto radio = std::make_shared<Radio>();
        radio->id = "019";
        Ts990Cat cat(mockRadio(radio));
        cat.open(kPort, 115200);
        QTRY_COMPARE_WITH_TIMEOUT(cat.state(), Ts990Cat::State::Refused, kTimeout);
        QVERIFY2(cat.lastError().contains(QStringLiteral("ID019")), qPrintable(cat.lastError()));
        QVERIFY(cat.lastError().contains(QStringLiteral("TS-990S")));
        QVERIFY(cat.active());
        QVERIFY(!radio->open);
        QTest::qWait(Ts990Cat::kRetryMs + 500);
        QCOMPARE(radio->received, (QList<QByteArray>{"ID;"}));
        QCOMPARE(radio->opens, 1);
        cat.close();
        QVERIFY(!cat.active());
    }

    // Switched off, or at another baud rate: it says so, and keeps asking.
    void noAnswerIsReportedAndAskedAgain()
    {
        auto radio = std::make_shared<Radio>();
        radio->answers = false;
        Ts990Cat cat(mockRadio(radio));
        cat.open(kPort, 38400);
        QTRY_VERIFY_WITH_TIMEOUT(cat.lastError().contains(QStringLiteral("0-07-01")), kTimeout);
        QVERIFY(cat.lastError().contains(QStringLiteral("38400")));
        QCOMPARE(cat.state(), Ts990Cat::State::Identifying);
        QCOMPARE(radio->received.size(), 3);
        radio->answers = true;
        QTRY_COMPARE_WITH_TIMEOUT(cat.state(), Ts990Cat::State::Ready, Ts990Cat::kRetryMs + kTimeout);
        QVERIFY(cat.lastError().isEmpty());
        QCOMPARE(radio->opens, 1);  // the port stayed open: only the radio was silent
    }

    // ?; while it is busy is no answer, and the ID is asked for again.
    void aBusyRadioIsAskedAgain()
    {
        auto radio = std::make_shared<Radio>();
        radio->busy = true;
        Ts990Cat cat(mockRadio(radio));
        cat.open(kPort, 115200);
        QTRY_VERIFY_WITH_TIMEOUT(radio->received.size() >= 3, kTimeout);
        QCOMPARE(cat.state(), Ts990Cat::State::Identifying);
        radio->busy = false;
        QTRY_COMPARE_WITH_TIMEOUT(cat.state(), Ts990Cat::State::Ready, Ts990Cat::kRetryMs + kTimeout);
    }

    void aPortThatWillNotOpenIsTriedAgain()
    {
        auto radio = std::make_shared<Radio>();
        radio->present = false;
        Ts990Cat cat(mockRadio(radio));
        cat.open(kPort, 115200);
        QCOMPARE(cat.state(), Ts990Cat::State::Closed);
        QVERIFY(cat.lastError().contains(QStringLiteral("no such port")));
        QVERIFY(cat.active());
        radio->present = true;
        QTRY_COMPARE_WITH_TIMEOUT(cat.state(), Ts990Cat::State::Ready, Ts990Cat::kRetryMs + kTimeout);
        QCOMPARE(radio->opens, 2);
    }

    // ── Following the radio ──────────────────────────────────────────────────

    void followsTheRadiosFrequencyAndMode()
    {
        auto radio = std::make_shared<Radio>();
        Ts990Cat cat(mockRadio(radio));
        cat.open(kPort, 115200);
        QTRY_COMPARE_WITH_TIMEOUT(cat.band(0).mode, QStringLiteral("USB"), kTimeout);
        QSignalSpy bands(&cat, &Ts990Cat::bandChanged);
        radio->fa = 7074000;
        radio->om0 = 'D';
        QTRY_COMPARE_WITH_TIMEOUT(cat.band(0).frequencyHz, 7074000, kTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(cat.band(0).mode, QStringLiteral("USB-D1"), kTimeout);
        QVERIFY(bands.count() >= 2);
        for (const QList<QVariant>& args : std::as_const(bands)) QCOMPARE(args.at(0).toInt(), 0);
        // Unchanged, nothing is announced.
        bands.clear();
        QTest::qWait(3 * Ts990Cat::kPollMs);
        QCOMPARE(bands.count(), 0);
    }

    void everyModeHasItsName_data()
    {
        QTest::addColumn<char>("code");
        QTest::addColumn<QString>("name");
        const QList<QPair<char, const char*>> modes = {
            {'1', "LSB"},    {'2', "USB"},    {'3', "CW"},     {'4', "FM"},     {'5', "AM"},     {'6', "FSK"},
            {'7', "CW-R"},   {'9', "FSK-R"},  {'A', "PSK"},    {'B', "PSK-R"},  {'C', "LSB-D1"}, {'D', "USB-D1"},
            {'E', "FM-D1"},  {'F', "AM-D1"},  {'G', "LSB-D2"}, {'H', "USB-D2"}, {'I', "FM-D2"},  {'J', "AM-D2"},
            {'K', "LSB-D3"}, {'L', "USB-D3"}, {'M', "FM-D3"},  {'N', "AM-D3"},  {'0', ""},       {'8', ""},
            {'Z', ""},
        };
        for (const auto& [code, name] : modes) {
            QTest::newRow(qPrintable(QStringLiteral("%1 %2").arg(QLatin1Char(code)).arg(QLatin1String(name))))
                << code << QString::fromLatin1(name);
        }
    }

    void everyModeHasItsName()
    {
        QFETCH(char, code);
        QFETCH(QString, name);
        QCOMPARE(Ts990Cat::modeName(code), name);
        // CW and every data mode skip NR, as on ExpertSDR3; voice does not.
        if (!name.isEmpty()) {
            const bool voice = name == QLatin1String("LSB") || name == QLatin1String("USB")
                               || name == QLatin1String("FM") || name == QLatin1String("AM");
            QCOMPARE(modeBypassesNr(name), !voice);
            QVERIFY(!modeIsComplexStereo(name));
        }
    }

    void aBlankFrequencyReadsAsNone()
    {
        auto radio = std::make_shared<Radio>();
        radio->blankFb = true;
        Ts990Cat cat(mockRadio(radio));
        cat.open(kPort, 115200);
        QTRY_COMPARE_WITH_TIMEOUT(cat.band(1).mode, QStringLiteral("CW"), kTimeout);
        QCOMPARE(cat.band(1).frequencyHz, 0);
        QCOMPARE(cat.band(0).frequencyHz, 14074000);
    }

    void repliesInPiecesAreJoined()
    {
        auto radio = std::make_shared<Radio>();
        radio->chunk = 3;
        Ts990Cat cat(mockRadio(radio));
        cat.open(kPort, 115200);
        QTRY_COMPARE_WITH_TIMEOUT(cat.band(1).mode, QStringLiteral("CW"), kTimeout);
        QCOMPARE(cat.band(0).frequencyHz, 14074000);
        QCOMPARE(cat.band(1).frequencyHz, 7030000);
    }

    // ── Losing it ────────────────────────────────────────────────────────────

    // Switched off while read: the radio is gone, and what was read is forgotten, so a CW mode
    // left standing cannot keep NR off.
    void aRadioThatStopsAnsweringIsLostAndFound()
    {
        auto radio = std::make_shared<Radio>();
        Ts990Cat cat(mockRadio(radio));
        cat.open(kPort, 115200);
        QTRY_COMPARE_WITH_TIMEOUT(cat.band(1).mode, QStringLiteral("CW"), kTimeout);
        radio->answers = false;
        QTRY_COMPARE_WITH_TIMEOUT(cat.state(), Ts990Cat::State::Identifying,
                                  Ts990Cat::kMissesToLose * Ts990Cat::kReplyTimeoutMs + kTimeout);
        QVERIFY(cat.lastError().contains(QStringLiteral("stopped answering")));
        QCOMPARE(cat.band(0).frequencyHz, 0);
        QVERIFY(cat.band(1).mode.isEmpty());
        radio->answers = true;
        QTRY_COMPARE_WITH_TIMEOUT(cat.state(), Ts990Cat::State::Ready, Ts990Cat::kRetryMs + kTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(cat.band(1).mode, QStringLiteral("CW"), kTimeout);
    }

    void anUnpluggedPortIsOpenedAgain()
    {
        auto radio = std::make_shared<Radio>();
        Ts990Cat cat(mockRadio(radio));
        cat.open(kPort, 115200);
        QTRY_COMPARE_WITH_TIMEOUT(cat.band(0).mode, QStringLiteral("USB"), kTimeout);
        radio->port->unplug();
        QCOMPARE(cat.state(), Ts990Cat::State::Identifying);
        QCOMPARE(cat.lastError(), QStringLiteral("unplugged"));
        QVERIFY(cat.band(0).mode.isEmpty());
        radio->present = true;
        QTRY_COMPARE_WITH_TIMEOUT(cat.state(), Ts990Cat::State::Ready, Ts990Cat::kRetryMs + kTimeout);
        QCOMPARE(radio->opens, 2);
    }

    // ── The AF level ─────────────────────────────────────────────────────────

    void theAfLevelIsReadAndSetAndReadBack()
    {
        auto radio = std::make_shared<Radio>();
        Ts990Cat cat(mockRadio(radio));
        QSignalSpy reads(&cat, &Ts990Cat::afLevelRead);
        QSignalSpy sets(&cat, &Ts990Cat::afLevelSet);
        // Nothing is read or set before the radio is known.
        cat.setAfLevel(0);
        cat.readAfLevel();
        QVERIFY(!cat.afKnown());
        cat.open(kPort, 115200);
        QTRY_COMPARE_WITH_TIMEOUT(cat.state(), Ts990Cat::State::Ready, kTimeout);
        QVERIFY(!radio->received.contains("AG0;"));

        cat.readAfLevel();
        QTRY_COMPARE_WITH_TIMEOUT(reads.count(), 1, kTimeout);
        QCOMPARE(reads.first().at(0).toInt(), 120);
        QCOMPARE(cat.afLevel(), 120);

        cat.setAfLevel(0);
        QTRY_COMPARE_WITH_TIMEOUT(sets.count(), 1, kTimeout);
        QCOMPARE(sets.first().at(0).toInt(), 0);
        QCOMPARE(sets.first().at(1).toBool(), true);
        QCOMPARE(radio->af, 0);
        const qsizetype set = radio->received.indexOf("AG0000;");
        QVERIFY(set > 0);
        QCOMPARE(radio->received.at(set + 1), QByteArray("AG0;"));

        // Out of range is not sent at all.
        const qsizetype before = radio->received.size();
        cat.setAfLevel(256);
        cat.setAfLevel(-1);
        QTest::qWait(3 * Ts990Cat::kPollMs);
        for (qsizetype i = before; i < radio->received.size(); ++i) {
            QVERIFY(!radio->received.at(i).startsWith("AG0") || radio->received.at(i) == "AG0;");
        }
    }

    // A set is judged by its own read-back, not by a read already on its way when it was asked.
    void aSetIsJudgedByItsOwnReadBack()
    {
        auto radio = std::make_shared<Radio>();
        Ts990Cat cat(mockRadio(radio));
        cat.open(kPort, 115200);
        QTRY_COMPARE_WITH_TIMEOUT(cat.state(), Ts990Cat::State::Ready, kTimeout);
        QSignalSpy sets(&cat, &Ts990Cat::afLevelSet);
        cat.readAfLevel();
        cat.setAfLevel(0);
        cat.readAfLevel();
        QTRY_COMPARE_WITH_TIMEOUT(sets.count(), 1, kTimeout);
        QCOMPARE(sets.first().at(1).toBool(), true);
        QCOMPARE(radio->received.count("AG0000;"), 1);
    }

    void aSetTheRadioDidNotTakeIsSaidSo()
    {
        auto radio = std::make_shared<Radio>();
        radio->knobHolds = true;
        Ts990Cat cat(mockRadio(radio));
        cat.open(kPort, 115200);
        QTRY_COMPARE_WITH_TIMEOUT(cat.state(), Ts990Cat::State::Ready, kTimeout);
        QSignalSpy sets(&cat, &Ts990Cat::afLevelSet);
        cat.setAfLevel(0);
        QTRY_COMPARE_WITH_TIMEOUT(sets.count(), 1, kTimeout);
        QCOMPARE(sets.first().at(1).toBool(), false);
        QCOMPARE(cat.afLevel(), 120);
    }

    // A newer set replaces one still waiting to go out: nothing can undo it after.
    void aNewerSetReplacesOneNotYetSent()
    {
        auto radio = std::make_shared<Radio>();
        Ts990Cat cat(mockRadio(radio));
        cat.open(kPort, 115200);
        QTRY_COMPARE_WITH_TIMEOUT(cat.state(), Ts990Cat::State::Ready, kTimeout);
        QSignalSpy sets(&cat, &Ts990Cat::afLevelSet);
        cat.readAfLevel();  // on the wire, so the sets below wait
        cat.setAfLevel(0);
        cat.setAfLevel(90);
        QTRY_COMPARE_WITH_TIMEOUT(sets.count(), 1, kTimeout);
        QCOMPARE(sets.first().at(0).toInt(), 90);
        QCOMPARE(sets.first().at(1).toBool(), true);
        QVERIFY(!radio->received.contains("AG0000;"));
        QCOMPARE(radio->af, 90);
    }

    // As the session closes, a restore still queued goes out; a 0 never does.
    void aPendingRestoreGoesOutAtClose()
    {
        auto radio = std::make_shared<Radio>();
        Ts990Cat cat(mockRadio(radio));
        cat.open(kPort, 115200);
        QTRY_COMPARE_WITH_TIMEOUT(cat.state(), Ts990Cat::State::Ready, kTimeout);
        radio->answers = false;  // what is on the wire now waits for its timeout
        cat.readAfLevel();
        cat.setAfLevel(0);
        cat.flushPendingLevel();
        QVERIFY(!radio->received.contains("AG0000;"));
        cat.readAfLevel();
        cat.setAfLevel(120);
        cat.flushPendingLevel();
        QCOMPARE(radio->received.count("AG0120;"), 1);
        cat.close();
        QCOMPARE(radio->received.count("AG0120;"), 1);
        for (const QByteArray& c : std::as_const(radio->received)) QVERIFY2(Ts990Cat::allowed(c), c.constData());
    }

    // ── The test sandbox ─────────────────────────────────────────────────────

    void theSandboxRefusesRealSerialPorts()
    {
        QVERIFY(testSandbox());
        QVERIFY(!realSerialRefusal().isEmpty());
        auto port = makeSerialTransport();
        QString error;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Test sandbox: refused to open a serial port")));
        QVERIFY(!port->open(QStringLiteral("/dev/esa-test-no-such-port"), 115200, &error));
        QVERIFY2(error.contains(QStringLiteral("ESA_TEST_SANDBOX")), qPrintable(error));
        QVERIFY(!port->isOpen());

        // The driver with no seam opens real ports, and so is refused too.
        Ts990Cat cat;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Test sandbox: refused to open a serial port")));
        cat.open(QStringLiteral("/dev/esa-test-no-such-port"), 115200);
        QCOMPARE(cat.state(), Ts990Cat::State::Closed);
        QVERIFY(cat.lastError().contains(QStringLiteral("ESA_TEST_SANDBOX")));
        cat.close();
    }

    // ── Through the backend and the engine ───────────────────────────────────

    // Without a port, or for a radio with no CAT, no port is ever opened.
    void noPortIsOpenedUnlessChosen()
    {
        auto radio = std::make_shared<Radio>();
        auto codec = std::make_shared<Codec>();
        {
            UsbBackend backend(fakeCodec(codec), nullptr, mockRadio(radio));
            AppConfig config = catConfig();
            config.usbCatPort.clear();
            backend.open(config);
            QTest::qWait(200);
            QVERIFY(!backend.cat().used);
            QVERIFY(!backend.capabilities().reportsMode);
            backend.close();
        }
        {
            UsbBackend backend(fakeCodec(codec), nullptr, mockRadio(radio));
            AppConfig config = catConfig();
            config.usbModel = QStringLiteral("generic");
            backend.open(config);
            QTest::qWait(200);
            QVERIFY(!backend.cat().used);
            backend.close();
        }
        QCOMPARE(radio->opens, 0);
        QVERIFY(radio->received.isEmpty());
    }

    // The radio's frequency and mode reach the status, and CW turns NR off as it does on
    // ExpertSDR3. A radio that stops answering turns it back on.
    void theEngineFollowsTheRadioOverCat()
    {
        QTemporaryDir dir;
        auto radio = std::make_shared<Radio>();
        auto codec = std::make_shared<Codec>();
        auto played = std::make_shared<std::atomic<quint64>>(0);
        Engine engine(dir.filePath(QStringLiteral("s.ini")),
                      [played](const AppConfig&, QString*) { return std::make_unique<CountingSink>(played); });
        engine.setBackendFactory([codec, radio](const QString& kind, QObject* parent) -> RadioBackend* {
            if (kind == QLatin1String("usb")) return new UsbBackend(fakeCodec(codec), parent, mockRadio(radio));
            return nullptr;
        });
        EngineStatus status;
        QObject::connect(&engine, &Engine::statusChanged, &engine, [&](const EngineStatus& s) { status = s; });
        engine.configure(catConfig());
        engine.connectRadio();
        QTRY_VERIFY_WITH_TIMEOUT(status.catReady, kTimeout);
        QVERIFY(status.catUsed);
        QCOMPARE(status.catPort, kPort);
        QTRY_COMPARE_WITH_TIMEOUT(status.frequencyHz, 14074000, kTimeout);
        QCOMPARE(status.modulation, QStringLiteral("USB"));
        QVERIFY(!status.nrBypassedByMode);
        QTRY_VERIFY_WITH_TIMEOUT(played->load() > 24000, kTimeout);

        radio->om0 = '3';
        QTRY_COMPARE_WITH_TIMEOUT(status.modulation, QStringLiteral("CW"), kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(status.nrBypassedByMode, kTimeout);

        // The sub receiver's own frequency and mode, at once.
        engine.setReceiver(1);
        QTRY_COMPARE_WITH_TIMEOUT(status.frequencyHz, 7030000, kTimeout);
        QCOMPARE(status.receiverName, QStringLiteral("Sub"));
        engine.setReceiver(0);
        QTRY_COMPARE_WITH_TIMEOUT(status.frequencyHz, 14074000, kTimeout);

        radio->answers = false;
        QTRY_VERIFY_WITH_TIMEOUT(!status.catReady && !status.catError.isEmpty(),
                                 Ts990Cat::kMissesToLose * Ts990Cat::kReplyTimeoutMs + kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(status.modulation.isEmpty(), kTimeout);
        QVERIFY(!status.nrBypassedByMode);
        // The audio carries on without CAT.
        QCOMPARE(status.connection, RadioBackend::Link::Ready);

        engine.disconnectRadio();
        QTRY_COMPARE_WITH_TIMEOUT(status.connection, RadioBackend::Link::Disconnected, kTimeout);
        QVERIFY(!radio->open);
        for (const QByteArray& c : std::as_const(radio->received)) QVERIFY2(Ts990Cat::allowed(c), c.constData());
        QVERIFY(!radio->received.contains("AG0000;"));  // nothing is set before the mute exists
        engine.shutdown();
    }

    // ── The AF, turned down while this app plays ────────────────────────────

    // Read first, then 0, and back to exactly where it was before the port closes.
    void theAfIsTurnedDownWhileThisAppPlaysAndGivenBack()
    {
        Station st;
        AppConfig config = catConfig();
        config.usbAfMute = true;
        st.start(config);
        QTRY_COMPARE_WITH_TIMEOUT(st.radio->af, 0, kTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(st.status.mutePhase, MuteController::Phase::Holding, kTimeout);
        QCOMPARE(st.status.mutePriorLevel, 120);
        const auto& sent = st.radio->received;
        QVERIFY(sent.indexOf("AG0;") >= 0);
        QVERIFY(sent.indexOf("AG0;") < sent.indexOf("AG0000;"));
        // Owed, on disk, before a crash could lose it.
        QVERIFY(st.owed().owned);
        QCOMPARE(st.owed().priorLevel, 120);

        st.engine->disconnectRadio();
        QTRY_COMPARE_WITH_TIMEOUT(st.status.connection, RadioBackend::Link::Disconnected, kTimeout);
        QCOMPARE(st.radio->af, 120);
        QVERIFY(sent.contains("AG0120;"));
        QVERIFY(!st.radio->open);
        QVERIFY(!st.owed().owned);
        for (const QByteArray& c : sent) QVERIFY2(Ts990Cat::allowed(c), c.constData());
        st.engine->shutdown();
    }

    // Off unless chosen: the AF is read, and never set.
    void theAfIsLeftAloneUnlessAskedFor()
    {
        Station st;
        st.start(catConfig());
        QTRY_VERIFY_WITH_TIMEOUT(st.status.audioFlowing && st.status.catReady, kTimeout);
        QTest::qWait(1500);
        QCOMPARE(st.radio->af, 120);
        for (const QByteArray& c : std::as_const(st.radio->received)) {
            QVERIFY2(!(c.startsWith("AG0") && c.size() == 7), c.constData());
        }
        QCOMPARE(st.status.mutePhase, MuteController::Phase::Idle);
        // Chosen while playing: down at once. Unchosen: back.
        st.engine->setUsbAfMute(true);
        QTRY_COMPARE_WITH_TIMEOUT(st.radio->af, 0, kTimeout);
        st.engine->setUsbAfMute(false);
        QTRY_COMPARE_WITH_TIMEOUT(st.radio->af, 120, kTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(st.status.mutePhase, MuteController::Phase::Idle, kTimeout);
        st.engine->shutdown();
    }

    // The operator turns the knob up while it is down: theirs from then on.
    void turningTheKnobUpHandsItBack()
    {
        Station st;
        AppConfig config = catConfig();
        config.usbAfMute = true;
        st.start(config);
        QTRY_COMPARE_WITH_TIMEOUT(st.status.mutePhase, MuteController::Phase::Holding, kTimeout);
        st.radio->af = 50;
        QTRY_COMPARE_WITH_TIMEOUT(st.status.mutePhase, MuteController::Phase::OperatorOverride, kTimeout);
        const qsizetype sets = st.radio->received.count("AG0000;");
        // The radio's audio goes silent long enough to count as no radio, then comes back: the
        // knob still wins.
        st.codec->left = 0.0f;
        st.codec->right = 0.0f;
        QTRY_VERIFY_WITH_TIMEOUT(!st.status.audioFlowing, kTimeout);
        st.codec->left = 0.25f;
        st.codec->right = -0.5f;
        QTRY_VERIFY_WITH_TIMEOUT(st.status.audioFlowing, kTimeout);
        QTest::qWait(1000);
        QCOMPARE(st.status.mutePhase, MuteController::Phase::OperatorOverride);
        QCOMPARE(st.radio->af, 50);
        st.engine->disconnectRadio();
        QTRY_COMPARE_WITH_TIMEOUT(st.status.connection, RadioBackend::Link::Disconnected, kTimeout);
        QCOMPARE(st.radio->af, 50);
        QCOMPARE(st.radio->received.count("AG0000;"), sets);
        QVERIFY(!st.radio->received.contains("AG0120;"));
        QVERIFY(!st.owed().owned);
        st.engine->shutdown();
    }

    // Quitting while it is down gives it back before the app goes.
    void quittingWhileDownGivesItBack()
    {
        Station st;
        AppConfig config = catConfig();
        config.usbAfMute = true;
        st.radio->af = 200;
        st.start(config);
        QTRY_COMPARE_WITH_TIMEOUT(st.radio->af, 0, kTimeout);
        st.engine->shutdown();
        QCOMPARE(st.radio->af, 200);
        QVERIFY(!st.owed().owned);
    }

    // The app was killed while the AF was down: the next connection gives it back, even with the
    // choice since turned off.
    void aCrashWhileDownIsGivenBackNextTime()
    {
        Station st;
        // Owed under the radio's model, whichever port name it had then.
        Settings(st.settingsPath()).saveMuteOwnership(QStringLiteral("usb:ts990s"), {true, false, 90});
        st.radio->af = 0;
        st.start(catConfig());
        QTRY_COMPARE_WITH_TIMEOUT(st.radio->af, 90, kTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(!st.owed().owned, kTimeout);
        st.engine->shutdown();
        QCOMPARE(st.radio->af, 90);
    }

    // The radio stops answering while the AF is down, and comes back: still down, and still owed.
    void aLostCatLinkKeepsWhatIsOwed()
    {
        Station st;
        AppConfig config = catConfig();
        config.usbAfMute = true;
        st.start(config);
        QTRY_COMPARE_WITH_TIMEOUT(st.status.mutePhase, MuteController::Phase::Holding, kTimeout);
        st.radio->answers = false;
        QTRY_VERIFY_WITH_TIMEOUT(!st.status.catReady, Ts990Cat::kMissesToLose * Ts990Cat::kReplyTimeoutMs + kTimeout);
        QVERIFY(st.owed().owned);
        st.radio->answers = true;
        QTRY_COMPARE_WITH_TIMEOUT(st.status.mutePhase, MuteController::Phase::Holding,
                                  Ts990Cat::kRetryMs + kTimeout);
        QCOMPARE(st.radio->af, 0);
        st.engine->disconnectRadio();
        QTRY_COMPARE_WITH_TIMEOUT(st.status.connection, RadioBackend::Link::Disconnected, kTimeout);
        QCOMPARE(st.radio->af, 120);
        st.engine->shutdown();
    }

    void theRadioPagesTestSendsIdAlone()
    {
        QTemporaryDir dir;
        auto radio = std::make_shared<Radio>();
        Engine engine(dir.filePath(QStringLiteral("s.ini")));
        engine.setCatTransportFactory(mockRadio(radio));
        QSignalSpy tested(&engine, &Engine::catTested);
        engine.testCat(kPort, 57600);
        QTRY_COMPARE_WITH_TIMEOUT(tested.count(), 1, kTimeout);
        QCOMPARE(tested.first().at(0).toBool(), true);
        QVERIFY(tested.first().at(1).toString().contains(QStringLiteral("TS-990S")));
        QCOMPARE(radio->lastBaud, 57600);
        QTRY_VERIFY_WITH_TIMEOUT(!radio->open, kTimeout);
        QTest::qWait(3 * Ts990Cat::kPollMs);
        QCOMPARE(radio->received, (QList<QByteArray>{"ID;"}));
        QCOMPARE(tested.count(), 1);

        radio->id = "021";
        engine.testCat(kPort, 57600);
        QTRY_COMPARE_WITH_TIMEOUT(tested.count(), 2, kTimeout);
        QCOMPARE(tested.at(1).at(0).toBool(), false);
        QVERIFY(tested.at(1).at(1).toString().contains(QStringLiteral("ID021")));

        engine.testCat(QString(), 57600);
        QTRY_COMPARE_WITH_TIMEOUT(tested.count(), 3, kTimeout);
        QCOMPARE(tested.at(2).at(0).toBool(), false);
        engine.shutdown();
    }

    // Under the sandbox the Test is refused at once, not after its timeout.
    void theTestIsRefusedAtOnceUnderTheSandbox()
    {
        QTemporaryDir dir;
        Engine engine(dir.filePath(QStringLiteral("s.ini")));
        QSignalSpy tested(&engine, &Engine::catTested);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Test sandbox: refused to open a serial port")));
        engine.testCat(QStringLiteral("/dev/esa-test-no-such-port"), 115200);
        QCOMPARE(tested.count(), 1);
        QCOMPARE(tested.first().at(0).toBool(), false);
        QVERIFY(tested.first().at(1).toString().contains(QStringLiteral("ESA_TEST_SANDBOX")));
        engine.shutdown();
    }

    // The connected session has the port: the Test does not open it a second time.
    void theTestWaitsForTheSessionToLetGo()
    {
        QTemporaryDir dir;
        auto radio = std::make_shared<Radio>();
        auto probe = std::make_shared<Radio>();
        auto codec = std::make_shared<Codec>();
        auto played = std::make_shared<std::atomic<quint64>>(0);
        Engine engine(dir.filePath(QStringLiteral("s.ini")),
                      [played](const AppConfig&, QString*) { return std::make_unique<CountingSink>(played); });
        engine.setBackendFactory([codec, radio](const QString& kind, QObject* parent) -> RadioBackend* {
            if (kind == QLatin1String("usb")) return new UsbBackend(fakeCodec(codec), parent, mockRadio(radio));
            return nullptr;
        });
        engine.setCatTransportFactory(mockRadio(probe));
        EngineStatus status;
        QObject::connect(&engine, &Engine::statusChanged, &engine, [&](const EngineStatus& s) { status = s; });
        engine.configure(catConfig());
        engine.connectRadio();
        QTRY_VERIFY_WITH_TIMEOUT(status.catReady, kTimeout);
        QSignalSpy tested(&engine, &Engine::catTested);
        engine.testCat(kPort, 115200);
        QCOMPARE(tested.count(), 1);
        QCOMPARE(tested.first().at(0).toBool(), false);
        QVERIFY(tested.first().at(1).toString().contains(QStringLiteral("disconnect")));
        QCOMPARE(probe->opens, 0);
        engine.disconnectRadio();
        QTRY_COMPARE_WITH_TIMEOUT(status.connection, RadioBackend::Link::Disconnected, kTimeout);
        engine.shutdown();
    }
};

QTEST_GUILESS_MAIN(TestTs990)
#include "test_ts990.moc"
