#include "app/Engine.h"
#include "app/Profiles.h"
#include "app/Settings.h"
#include "audio/PortAudioSink.h"
#include "dsp/NrProcessor.h"
#include "dsp/RxChain.h"
#include "gui/MainWindow.h"
#include "gui/aether/Appearance.h"
#include "gui/pages/Pages.h"
#include "tci/TciDiscovery.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSize>
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

QString pageIdList()
{
    QStringList ids;
    for (const PageEntry& page : pageTable()) ids.append(page.id);
    return ids.join(QStringLiteral(", "));
}

int runDiscovery()
{
    const TciDiscovery::Options options = TciDiscovery::localNetworkOptions();
    out() << "Looking for TCI radios on " << options.hosts.size() << " addresses...\n";
    out().flush();

    TciDiscovery discovery;
    QObject::connect(&discovery, &TciDiscovery::radioFound, qApp, [](const DiscoveredRadio& radio) {
        out() << "  " << radio.title() << "\n      --host " << radio.host << " --port " << radio.port
              << (radio.isThisComputer() ? "   (this computer)" : "") << "\n";
        out().flush();
    });
    QObject::connect(&discovery, &TciDiscovery::finished, qApp, &QCoreApplication::quit, Qt::QueuedConnection);
    discovery.start(options);
    QCoreApplication::exec();

    if (discovery.radios().isEmpty()) {
        out() << "No radios found. Check that ExpertSDR3 is running with TCI enabled, and that its firewall allows the port.\n";
    }
    return 0;
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

// --record's argument: a .wav file to record into, or else a folder to name the file in.
bool recordsToFile(const QString& target)
{
    return target.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive) && !QFileInfo(target).isDir();
}

int runHeadless(EngineHost& host, const AppConfig& config, const QString& capture, const QString& record,
                double durationSeconds)
{
    Engine* engine = host.engine();
    auto last = std::make_shared<EngineStatus>();
    // What the summary reports comes from the run itself, except the files: those are only
    // complete once the engine has stopped, and its last word on them arrives after the event
    // loop has ended (taken below, after host.stop()).
    auto stopped = std::make_shared<bool>(false);
    QObject::connect(engine, &Engine::statusChanged, qApp, [last, stopped](const EngineStatus& status) {
        if (!*stopped) {
            *last = status;
        } else {
            last->recording = status.recording;
            last->capture = status.capture;
        }
    }, Qt::QueuedConnection);

    QMetaObject::invokeMethod(engine, [=] {
        engine->configure(config);
        if (!capture.isEmpty()) engine->setCapture(capture);
        engine->connectRadio();
        // From the first audio: the file is named, and opened, once the radio is there.
        if (!record.isEmpty()) {
            if (recordsToFile(record)) {
                engine->startRecordingToFile(record);
            } else {
                engine->startRecording(record);
            }
        }
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
              << "  mute " << muteName(s.mutePhase);
        if (s.recording.active) {
            out() << "  rec " << QString::number(s.recording.seconds, 'f', 0) << " s";
            if (s.recording.droppedFrames > 0) out() << " (dropped " << s.recording.droppedFrames << " frames)";
        }
        out() << "\n";
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
    *stopped = true;
    host.stop();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);  // the engine's final status

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
        // The file's actual name: an existing file is never replaced, so it may end _1, _2...
        {QStringLiteral("recording"), QJsonObject{
            {QStringLiteral("path"), s.recording.path},
            {QStringLiteral("seconds"), s.recording.seconds},
            {QStringLiteral("droppedFrames"), double(s.recording.droppedFrames)},
            {QStringLiteral("error"), s.recording.error},
        }},
        {QStringLiteral("capture"), QJsonObject{
            {QStringLiteral("files"), QJsonArray::fromStringList(s.capture.files)},
            {QStringLiteral("error"), s.capture.error},
        }},
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
        for (const char* flag : {"--headless", "--list-devices", "--list-params", "--discover", "--list-profiles",
                                 "--import-profile", "--export-profile", "--help", "-h", "--version", "-v"}) {
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
    const QCommandLineOption discoverOpt(QStringLiteral("discover"), QStringLiteral("Look for TCI radios on the local network and exit."));
    const QCommandLineOption listProfilesOpt(QStringLiteral("list-profiles"), QStringLiteral("List saved audio chain profiles and exit."));
    const QCommandLineOption profileOpt(QStringLiteral("profile"), QStringLiteral("Use this saved audio chain profile. --nr and --set still apply on top."), QStringLiteral("name"));
    const QCommandLineOption importProfileOpt(QStringLiteral("import-profile"), QStringLiteral("Import profiles from a file and exit."), QStringLiteral("file"));
    const QCommandLineOption exportProfileOpt(QStringLiteral("export-profile"), QStringLiteral("Print a saved profile as JSON and exit. Redirect it to a file."), QStringLiteral("name"));
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
    const QCommandLineOption captureOpt(QStringLiteral("capture"), QStringLiteral("Headless: record the radio's audio to <base>.before.wav and the chain's output to <base>.after.wav (or the next free names; a rate change continues in _part2)."), QStringLiteral("base"));
    const QCommandLineOption recordOpt(QStringLiteral("record"), QStringLiteral("Headless: record the chain's output from the first audio until exit or --duration, into this folder, named from the time, frequency and mode, or into this .wav file. An existing file is never replaced."), QStringLiteral("folder|file.wav"));
    const QCommandLineOption durationOpt(QStringLiteral("duration"), QStringLiteral("Headless: stop after this many seconds."), QStringLiteral("s"));
    const QCommandLineOption settingsOpt(QStringLiteral("settings"), QStringLiteral("Use this ini file instead of the normal settings store."), QStringLiteral("file"));
    const QCommandLineOption connectOpt(QStringLiteral("connect"), QStringLiteral("Connect immediately on startup."));
    const QCommandLineOption screenshotOpt(QStringLiteral("screenshot"), QStringLiteral("Save a PNG of the window after 2 s and exit (development)."), QStringLiteral("file"));
    const QCommandLineOption pageOpt(QStringLiteral("page"), QStringLiteral("With --screenshot: the page to show (%1).").arg(pageIdList()), QStringLiteral("id"));
    const QCommandLineOption sizeOpt(QStringLiteral("size"), QStringLiteral("With --screenshot: the window's size, e.g. 724x752, instead of the saved one."), QStringLiteral("WxH"));
    parser.addOptions({headlessOpt, listDevicesOpt, listParamsOpt, discoverOpt, listProfilesOpt, profileOpt,
                       importProfileOpt, exportProfileOpt, hostOpt, portOpt, rxOpt, deviceOpt, nrOpt,
                       setOpt, gainOpt, bufferOpt, deviceRateOpt, rateOpt, noMuteOpt, captureOpt, recordOpt, durationOpt,
                       settingsOpt, connectOpt, screenshotOpt, pageOpt, sizeOpt});
    parser.process(*app);

    // For comparing pages with AetherRX's: only a screenshot run takes them, and it never
    // saves the window's state, so an ordinary start comes back where the operator left it.
    QString screenshotPage;
    QSize screenshotSize;
    if (parser.isSet(screenshotOpt)) {
        if (parser.isSet(pageOpt)) {
            screenshotPage = parser.value(pageOpt);
            if (!findPage(screenshotPage)) {
                qCritical("Unknown page \"%s\". Pages: %s", qPrintable(screenshotPage), qPrintable(pageIdList()));
                return 2;
            }
        }
        if (parser.isSet(sizeOpt)) {
            const QRegularExpressionMatch match =
                QRegularExpression(QStringLiteral("^(\\d+)x(\\d+)$")).match(parser.value(sizeOpt));
            screenshotSize = match.hasMatch() ? QSize(match.captured(1).toInt(), match.captured(2).toInt()) : QSize();
            if (screenshotSize.isEmpty()) {
                qCritical("--size takes a width and height such as 724x752");
                return 2;
            }
        }
    } else if (parser.isSet(pageOpt) || parser.isSet(sizeOpt)) {
        qWarning("--page and --size apply only with --screenshot");
    }

    if (parser.isSet(listDevicesOpt)) {
        printDevices();
        return 0;
    }
    if (parser.isSet(listParamsOpt)) {
        printParams();
        return 0;
    }

    if (parser.isSet(discoverOpt)) return runDiscovery();

    if (parser.isSet(recordOpt) && !parser.isSet(headlessOpt)) {
        qCritical("--record needs --headless");
        return 2;
    }
    if (parser.isSet(recordOpt) && parser.value(recordOpt).isEmpty()) {
        qCritical("--record needs a folder or a .wav file");
        return 2;
    }

    const QString settingsPath = parser.value(settingsOpt);
    Settings settings(settingsPath);
    AppConfig config = settings.load();

    ProfileLibrary profiles(ProfileLibrary::defaultPath(settingsPath));
    if (!profiles.loadWarning().isEmpty()) qWarning("%s", qPrintable(profiles.loadWarning()));
    if (parser.isSet(listProfilesOpt)) {
        const QStringList names = profiles.names();
        for (const QString& name : names) out() << name << "\n";
        return 0;
    }
    if (parser.isSet(importProfileOpt)) {
        const ImportResult result = profiles.importFile(parser.value(importProfileOpt));
        if (!result.error.isEmpty()) {
            qCritical("%s", qPrintable(result.error));
            return 2;
        }
        for (const QString& name : result.imported) out() << "Imported " << name << "\n";
        for (const QString& rename : result.renamed) out() << "Renamed " << rename << "\n";
        return 0;
    }
    if (parser.isSet(exportProfileOpt)) {
        const QByteArray document = profiles.exportDocument({parser.value(exportProfileOpt)});
        if (document.isEmpty()) {
            qCritical("No profile called \"%s\". See --list-profiles.", qPrintable(parser.value(exportProfileOpt)));
            return 2;
        }
        out() << document;
        return 0;
    }
    if (parser.isSet(profileOpt)) {
        const Profile* profile = profiles.find(parser.value(profileOpt));
        if (!profile) {
            qCritical("No profile called \"%s\". See --list-profiles.", qPrintable(parser.value(profileOpt)));
            return 2;
        }
        applyChain(config, profile->chain);
        config.profileName = profile->name;
    }

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
        return runHeadless(host, config, parser.value(captureOpt), parser.value(recordOpt),
                           parser.isSet(durationOpt) ? parser.value(durationOpt).toDouble() : 0.0);
    }

    Appearance::install();
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/ExpertSDR3Audio-256.png")));
    MainWindow window(host.engine(), host.engine()->taps(), &settings, &profiles, config);
    // The page and size a screenshot shows are its own, not where the operator left things.
    if (parser.isSet(screenshotOpt)) window.setSavesWindowState(false);
    if (!screenshotPage.isEmpty()) window.showPage(screenshotPage);
    if (screenshotSize.isValid()) {
        // Exactly this size: no saved geometry, and not maximised on a small screen.
        window.setWindowState(Qt::WindowNoState);
        window.resize(screenshotSize);
    }
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
