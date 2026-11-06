#include "app/Engine.h"
#include "app/Settings.h"
#include "audio/PortAudioSink.h"
#include "dsp/NrProcessor.h"
#include "dsp/RxChain.h"
#include "gui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStyleFactory>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace esa;

namespace {

std::atomic<bool> g_stopRequested{false};
void onSignal(int) { g_stopRequested.store(true); }

QTextStream& out()
{
    static QTextStream stream(stdout);
    return stream;
}

void printDevices()
{
    const auto devices = audio::listOutputDevices();
    if (devices.isEmpty()) {
        out() << "No audio output devices found.\n";
        return;
    }
    out() << "Audio output devices:\n";
    for (const AudioDeviceInfo& d : devices) {
        out() << (d.isDefault ? "  * " : "    ") << d.name << "\n"
              << "      " << d.hostApi << ", " << d.maxOutputChannels << " ch, "
              << int(d.defaultSampleRate) << " Hz\n"
              << "      --device \"" << d.key << "\"\n";
    }
    out() << "(* = system default)\n";
}

void printParams()
{
    out() << "NR methods (--nr):\n  off\n";
    for (const NrMethod& m : nrMethods()) out() << "  " << m.id << "  " << m.label << "\n";
    out() << "\nParameters (--set key=value):\n";
    for (const ParamSpec& s : allParamSpecs()) {
        out() << "  " << s.key << "  [" << s.min << " .. " << s.max << "] default " << s.def;
        if (!s.unit.isEmpty()) out() << " " << s.unit;
        if (!s.choices.isEmpty()) out() << "  (" << s.choices.join(QStringLiteral(", ")) << ")";
        out() << "\n";
    }
}

// A key, an exact name or a unique part of a name, so `--device "USB"` works.
QString resolveDevice(const QString& wanted, QString* error)
{
    if (wanted == kNullDeviceKey) return wanted;
    const auto devices = audio::listOutputDevices();
    QStringList partial;
    for (const AudioDeviceInfo& d : devices) {
        if (d.key == wanted || d.name == wanted) return d.key;
        if (d.name.contains(wanted, Qt::CaseInsensitive)) partial.append(d.key);
    }
    if (partial.size() == 1) return partial.first();
    *error = partial.isEmpty()
                 ? QStringLiteral("No output device matches \"%1\". Try --list-devices.").arg(wanted)
                 : QStringLiteral("\"%1\" matches several devices: %2").arg(wanted, partial.join(QStringLiteral(", ")));
    return {};
}

const char* muteName(MuteController::Phase phase)
{
    switch (phase) {
    case MuteController::Phase::Idle: return "idle";
    case MuteController::Phase::Engaging: return "engaging";
    case MuteController::Phase::Holding: return "holding";
    case MuteController::Phase::OperatorOverride: return "operator-override";
    case MuteController::Phase::Restoring: return "restoring";
    case MuteController::Phase::Failed: return "failed";
    }
    return "?";
}

const char* connectionName(TciClient::State state)
{
    switch (state) {
    case TciClient::State::Disconnected: return "disconnected";
    case TciClient::State::Connecting: return "connecting";
    case TciClient::State::Initializing: return "initializing";
    case TciClient::State::Ready: return "ready";
    }
    return "?";
}

// Starts the engine on its own thread and stops it cleanly, restoring ExpertSDR3's
// mute, on destruction or stop().
class EngineHost {
public:
    explicit EngineHost(const QString& settingsPath) : m_engine(new Engine(settingsPath))
    {
        m_engine->moveToThread(&m_thread);
        m_thread.setObjectName(QStringLiteral("engine"));
        m_thread.start(QThread::HighPriority);
    }
    ~EngineHost() { stop(); }

    Engine* engine() const { return m_engine; }

    void stop()
    {
        if (!m_engine) return;
        QMetaObject::invokeMethod(m_engine, &Engine::shutdown, Qt::BlockingQueuedConnection);
        // Deleted on its own thread, because it owns a socket and timers.
        QMetaObject::invokeMethod(m_engine, [engine = m_engine] { delete engine; },
                                  Qt::BlockingQueuedConnection);
        m_engine = nullptr;
        m_thread.quit();
        m_thread.wait();
    }

private:
    QThread m_thread;
    Engine* m_engine;
};

int runHeadless(EngineHost& host, const AppConfig& config, const QString& capture, double durationSeconds)
{
    Engine* engine = host.engine();
    auto last = std::make_shared<EngineStatus>();
    QObject::connect(engine, &Engine::statusChanged, qApp,
                     [last](const EngineStatus& status) { *last = status; }, Qt::QueuedConnection);

    QMetaObject::invokeMethod(engine, [=] {
        engine->configure(config);
        if (!capture.isEmpty()) engine->setCapture(capture);
        engine->connectRadio();
    });

    QTimer reporter;
    QObject::connect(&reporter, &QTimer::timeout, qApp, [last] {
        const EngineStatus& s = *last;
        out() << connectionName(s.connection) << "  " << s.serverName << " " << s.serverVersion
              << "  rx" << s.receiver << " " << s.frequencyHz << " Hz " << s.modulation
              << "  in " << QString::number(s.inputPeakDb, 'f', 1) << " dB"
              << "  nr " << (s.nrMethod.isEmpty() ? QStringLiteral("off") : s.nrMethod)
              << (s.nrBypassedByMode ? " (bypassed)" : "")
              << "  dsp " << QString::number(s.dspLoad * 100.0, 'f', 0) << "%"
              << (s.dspLoad > 0.85 ? " (OVERLOADED: try --nr nr2)" : "")
              << "  buffer " << QString::number(s.sink.bufferedMs, 'f', 0) << " ms"
              << "  drift " << QString::number(s.sink.driftPpm, 'f', 0) << " ppm"
              << "  underruns " << s.sink.underruns
              << "  mute " << muteName(s.mutePhase) << "\n";
        out().flush();
    });
    reporter.start(2000);

    QTimer poll;
    QElapsedTimer elapsed;
    elapsed.start();
    QObject::connect(&poll, &QTimer::timeout, qApp, [&] {
        if (g_stopRequested.load() || (durationSeconds > 0 && elapsed.elapsed() >= durationSeconds * 1000.0)) {
            qApp->quit();
        }
    });
    poll.start(50);

    qApp->exec();
    host.stop();

    // Machine-readable summary for scripts.
    const EngineStatus& s = *last;
    QJsonObject summary{
        {QStringLiteral("connection"), QString::fromLatin1(connectionName(s.connection))},
        {QStringLiteral("server"), s.serverName + QLatin1Char(' ') + s.serverVersion},
        {QStringLiteral("device"), s.device},
        {QStringLiteral("port"), s.connectedPort},
        {QStringLiteral("modulation"), s.modulation},
        {QStringLiteral("blocks"), double(s.blocks)},
        {QStringLiteral("blocksPerSecond"), s.blocksPerSecond},
        {QStringLiteral("droppedBlocks"), double(s.droppedBlocks)},
        {QStringLiteral("malformedBlocks"), double(s.malformedBlocks)},
        {QStringLiteral("nrMethod"), s.nrMethod},
        {QStringLiteral("nrFailed"), s.nrFailed},
        {QStringLiteral("nrPreparing"), s.nrPreparing},
        {QStringLiteral("nrBypassedByMode"), s.nrBypassedByMode},
        {QStringLiteral("nrGainDb"), s.nrGainDb},
        {QStringLiteral("dspLoad"), s.dspLoad},
        {QStringLiteral("outputDevice"), s.outputDevice},
        {QStringLiteral("deviceRate"), s.sink.deviceRate},
        {QStringLiteral("bufferedMs"), s.sink.bufferedMs},
        {QStringLiteral("targetMs"), s.sink.targetMs},
        {QStringLiteral("driftPpm"), s.sink.driftPpm},
        {QStringLiteral("deviceLatencyMs"), s.sink.reportedLatencyMs},
        {QStringLiteral("framesPlayed"), double(s.sink.framesPlayed)},
        {QStringLiteral("underruns"), double(s.sink.underruns)},
        {QStringLiteral("overruns"), double(s.sink.overruns)},
        {QStringLiteral("mutePhase"), QString::fromLatin1(muteName(s.mutePhase))},
    };
    out() << "SUMMARY " << QJsonDocument(summary).toJson(QJsonDocument::Compact) << "\n";
    out().flush();
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    // The application type must exist before the parser, and a display-less machine
    // such as a headless Raspberry Pi cannot create a QApplication at all.
    bool consoleOnly = false;
    for (int i = 1; i < argc; ++i) {
        for (const char* flag : {"--headless", "--list-devices", "--list-params", "--help", "-h", "--version", "-v"}) {
            if (std::strcmp(argv[i], flag) == 0) consoleOnly = true;
        }
    }

#ifdef Q_OS_WIN
    // A GUI-subsystem program has no console of its own, so without this --list-devices
    // and --headless would print nothing. Borrow the one it was started from.
    // Not when output is already going somewhere: that would break `... | findstr`.
    if (consoleOnly && GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN
        && AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif

    std::unique_ptr<QCoreApplication> app;
    if (consoleOnly) {
        app = std::make_unique<QCoreApplication>(argc, argv);
    } else {
        app = std::make_unique<QApplication>(argc, argv);
    }
    QCoreApplication::setOrganizationName(QStringLiteral("ExpertSDR3Audio"));
    QCoreApplication::setApplicationName(QStringLiteral("ExpertSDR3Audio"));
    QCoreApplication::setApplicationVersion(QStringLiteral(ESA_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Plays ExpertSDR3 receive audio through the AetherRX noise-reduction chain."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption headlessOpt(QStringLiteral("headless"), QStringLiteral("Run without a window."));
    const QCommandLineOption listDevicesOpt(QStringLiteral("list-devices"), QStringLiteral("List audio output devices and exit."));
    const QCommandLineOption listParamsOpt(QStringLiteral("list-params"), QStringLiteral("List NR methods and chain parameters and exit."));
    const QCommandLineOption hostOpt(QStringLiteral("host"), QStringLiteral("TCI server host."), QStringLiteral("host"));
    const QCommandLineOption portOpt(QStringLiteral("port"), QStringLiteral("TCI server port."), QStringLiteral("port"));
    const QCommandLineOption rxOpt(QStringLiteral("rx"), QStringLiteral("Receiver number."), QStringLiteral("n"));
    const QCommandLineOption deviceOpt(QStringLiteral("device"), QStringLiteral("Output device: key, name or part of a name. \"null\" discards audio."), QStringLiteral("device"));
    const QCommandLineOption nrOpt(QStringLiteral("nr"), QStringLiteral("Noise-reduction method, or \"off\". See --list-params."), QStringLiteral("id"));
    const QCommandLineOption setOpt(QStringLiteral("set"), QStringLiteral("Set a chain parameter; repeatable. See --list-params."), QStringLiteral("key=value"));
    const QCommandLineOption gainOpt(QStringLiteral("gain"), QStringLiteral("Output gain in dB."), QStringLiteral("dB"));
    const QCommandLineOption bufferOpt(QStringLiteral("buffer"), QStringLiteral("Output buffer in ms."), QStringLiteral("ms"));
    const QCommandLineOption deviceRateOpt(QStringLiteral("device-rate"), QStringLiteral("Force the output device's sample rate, converting to it if needed."), QStringLiteral("hz"));
    const QCommandLineOption rateOpt(QStringLiteral("rate"), QStringLiteral("Processing rate: 48000 (default) or 24000 for low-power machines."), QStringLiteral("hz"));
    const QCommandLineOption noMuteOpt(QStringLiteral("no-mute"), QStringLiteral("Leave ExpertSDR3's own audio alone."));
    const QCommandLineOption captureOpt(QStringLiteral("capture"), QStringLiteral("Record to <base>.before.wav and <base>.after.wav."), QStringLiteral("base"));
    const QCommandLineOption durationOpt(QStringLiteral("duration"), QStringLiteral("Headless: stop after this many seconds."), QStringLiteral("s"));
    const QCommandLineOption settingsOpt(QStringLiteral("settings"), QStringLiteral("Use this ini file instead of the normal settings store."), QStringLiteral("file"));
    const QCommandLineOption connectOpt(QStringLiteral("connect"), QStringLiteral("Connect immediately on startup."));
    const QCommandLineOption screenshotOpt(QStringLiteral("screenshot"), QStringLiteral("Save a PNG of the window after 2 s and exit (development)."), QStringLiteral("file"));
    parser.addOptions({headlessOpt, listDevicesOpt, listParamsOpt, hostOpt, portOpt, rxOpt, deviceOpt, nrOpt,
                       setOpt, gainOpt, bufferOpt, deviceRateOpt, rateOpt, noMuteOpt, captureOpt, durationOpt, settingsOpt,
                       connectOpt, screenshotOpt});
    parser.process(*app);

    if (parser.isSet(listDevicesOpt)) {
        printDevices();
        return 0;
    }
    if (parser.isSet(listParamsOpt)) {
        printParams();
        return 0;
    }

    const QString settingsPath = parser.value(settingsOpt);
    Settings settings(settingsPath);
    AppConfig config = settings.load();

    if (parser.isSet(hostOpt)) config.host = parser.value(hostOpt);
    if (parser.isSet(portOpt)) {
        config.port = parser.value(portOpt).toInt();
        config.fallbackPorts.clear();
    }
    if (parser.isSet(rxOpt)) config.receiver = parser.value(rxOpt).toInt();
    if (parser.isSet(gainOpt)) config.outputGainDb = parser.value(gainOpt).toDouble();
    if (parser.isSet(bufferOpt)) config.bufferMs = parser.value(bufferOpt).toInt();
    if (parser.isSet(deviceRateOpt)) config.deviceRate = parser.value(deviceRateOpt).toInt();
    if (parser.isSet(noMuteOpt)) config.muteExpert = false;
    if (parser.isSet(rateOpt)) {
        const int rate = parser.value(rateOpt).toInt();
        if (rate != 24000 && rate != 48000) {
            qCritical("--rate must be 48000 or 24000");
            return 2;
        }
        config.chainRate = rate;
    }
    if (parser.isSet(deviceOpt)) {
        QString error;
        config.deviceKey = resolveDevice(parser.value(deviceOpt), &error);
        if (!error.isEmpty()) {
            qCritical("%s", qPrintable(error));
            return 2;
        }
    }
    if (parser.isSet(nrOpt)) {
        const QString id = parser.value(nrOpt);
        if (id == QLatin1String("off")) {
            config.nrMethod.clear();
        } else if (findNrMethod(id)) {
            config.nrMethod = id;
        } else {
            qCritical("Unknown or unavailable NR method \"%s\". See --list-params.", qPrintable(id));
            return 2;
        }
    }
    const QStringList assignments = parser.values(setOpt);
    for (const QString& assignment : assignments) {
        const QString key = assignment.section(QLatin1Char('='), 0, 0);
        bool ok = false;
        const double value = assignment.section(QLatin1Char('='), 1).toDouble(&ok);
        if (!ok || !findParamSpec(key)) {
            qCritical("Bad --set \"%s\". See --list-params.", qPrintable(assignment));
            return 2;
        }
        config.params.insert(key, value);
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    EngineHost host(settingsPath);

    if (parser.isSet(headlessOpt)) {
        return runHeadless(host, config, parser.value(captureOpt),
                           parser.isSet(durationOpt) ? parser.value(durationOpt).toDouble() : 0.0);
    }

    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/ExpertSDR3Audio-256.png")));
    MainWindow window(host.engine(), &settings, config);
    window.show();
    if (parser.isSet(connectOpt)) window.connectNow();

    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &window, [&window] {
        if (g_stopRequested.load()) window.close();
    });
    poll.start(100);

    if (parser.isSet(screenshotOpt)) {
        const QString path = parser.value(screenshotOpt);
        QTimer::singleShot(2000, &window, [&window, path] {
            window.grab().save(path);
            window.close();
        });
    }

    const int result = QApplication::exec();
    host.stop();
    return result;
}
