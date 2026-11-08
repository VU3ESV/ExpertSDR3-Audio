// The sync-safety contract for AetherSDR's vendored AetherRX pages.
//
// First the pages against our shims alone: no engine, no radio, no window of ours. This is
// where a sync that breaks the pages, the theme or the fonts shows up. Then the real window
// against a real engine that never connects, on this thread: every object the window looks
// up, every key a page edits, every correction made from outside. A sync that renames an
// object, adds a forced write or changes a behaviour we correct fails here.
// Set ESA_GUI_SHOT_DIR to also save a PNG of each page, for comparing by eye.

#include "MockExpert.h"

#include "app/Engine.h"
#include "app/Profiles.h"
#include "app/Settings.h"
#include "dsp/RxChain.h"
#include "dsp/StageSet.h"
#include "gui/DetailsWindow.h"
#include "gui/MainWindow.h"
#include "gui/ParamWidgets.h"
#include "gui/aether/Appearance.h"
#include "gui/aether/Guards.h"
#include "gui/binding/StageBinding.h"
#include "gui/pages/StageHost.h"
#include "gui/shell/WindowChrome.h"

#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ThemeManager.h"

#include "ClientCompKnob.h"
#include "ClientCompMeter.h"
#include "ClientCompThresholdFader.h"
#include "ClientEqEditorCanvas.h"
#include "ClientEqOutputFader.h"
#include "ClientGateCurveWidget.h"
#include "ClientGateLevelView.h"
#include "EditorFramelessTitleBar.h"
#include "NrGainStrip.h"
#include "PooDooLogo.h"
#include "StagePage.h"
#include "StripCompPanel.h"
#include "StripEqPanel.h"
#include "StripGatePanel.h"
#include "StripPuduPanel.h"
#include "StripRxOutputPanel.h"
#include "StripTubePanel.h"
#include "StripWaveformPanel.h"
#include "Theme.h"
#include "WaveformWidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGridLayout>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSlider>
#include <QStandardPaths>
#include <QStyle>
#include <QTemporaryDir>
#include <QThread>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtTest>

#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

using namespace AetherSDR;

namespace {

// The page area of the reference screenshots: AetherRX's 726x751 window less its
// header, tab column and margins.
constexpr QSize kPageSize(509, 684);

QColor canonGround()
{
    return QColor(0x06, 0x0b, 0x13);  // #060b13, default-dark.json's color.canon.ground
}

// What the window will put around a page: AetherRX's theme scope, upstream's app sheet
// (Inter at 13 px among the rest), the canon body's sheet over that, and the ground
// under it all, here flat where the window paints a gradient.
class Ground final : public QWidget {
public:
    explicit Ground(QWidget* page)
    {
        theme::setContainer(this, QStringLiteral("canon/aetherRx"));
        applyAppTheme(this);
        auto* body = new QWidget(this);
        body->setObjectName(QStringLiteral("canonBody"));
        ThemeManager::instance().applyStyleSheet(body, canonBodyStyleSheet());
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->addWidget(body);
        auto* inner = new QVBoxLayout(body);
        inner->setContentsMargins(0, 0, 0, 0);
        page->setParent(body);
        inner->addWidget(page);
        // Fixed, as the page column is in the window: a page must fit the room it is
        // given (StagePage shrinks its graphics to do so), not grow the window.
        setFixedSize(kPageSize);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), ThemeManager::instance().color(this, QStringLiteral("color.canon.ground")));
    }
};

// The fraction of pixels well away from the ground colour. Near zero means the page
// drew nothing over the ground: a panel that never laid out, or never became visible.
double paintedFraction(const QImage& image)
{
    const QColor ground = canonGround();
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    qint64 painted = 0;
    for (int y = 0; y < rgb.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
        for (int x = 0; x < rgb.width(); ++x) {
            const QRgb c = line[x];
            if (std::abs(qRed(c) - ground.red()) > 24 || std::abs(qGreen(c) - ground.green()) > 24
                || std::abs(qBlue(c) - ground.blue()) > 24) {
                ++painted;
            }
        }
    }
    return double(painted) / double(qint64(rgb.width()) * rgb.height());
}

double roundedTo(double value, int decimals)
{
    const double scale = std::pow(10.0, decimals);
    return std::round(value * scale) / scale;
}

// A value of `spec` other than its default, on the table's own decimals.
double nonDefault(const esa::ParamSpec& spec)
{
    if (spec.toggle) return spec.def != 0.0 ? 0.0 : 1.0;
    if (!spec.choices.isEmpty()) return double((int(spec.def) + 1) % int(spec.choices.size()));
    for (double t : {0.37, 0.61}) {
        const double v = roundedTo(spec.min + t * (spec.max - spec.min), spec.decimals);
        if (std::abs(v - spec.def) > 1e-9) return v;
    }
    return spec.max;
}

// One wheel step as a trackpad or a mouse sends it, straight to `w`.
void wheel(QWidget* w, int angleDeltaY)
{
    const QPointF at = QRectF(w->rect()).center();
    QWheelEvent event(at, w->mapToGlobal(at), QPoint(), QPoint(0, angleDeltaY), Qt::NoButton, Qt::NoModifier,
                      Qt::ScrollUpdate, false);
    QApplication::sendEvent(w, &event);
}

// A press, a drag of `dy` pixels (up is negative) and a release, as a hand on the knob.
void drag(QWidget* w, qreal dy)
{
    const QPointF from = QRectF(w->rect()).center();
    const QPointF to = from + QPointF(0, dy);
    QMouseEvent press(QEvent::MouseButtonPress, from, w->mapToGlobal(from), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QMouseEvent move(QEvent::MouseMove, to, w->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, to, w->mapToGlobal(to), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

// Types `value` into a knob's value field and presses Return, as the operator would: into
// the field once it has the keyboard, which is when it shows the bare number to type over.
void type(AetherSDR::ClientCompKnob* knob, double value)
{
    auto* field = knob->findChild<QLineEdit*>();
    QVERIFY(field);
    knob->window()->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(knob->window()));
    field->setFocus(Qt::MouseFocusReason);
    QTRY_VERIFY(field->hasFocus());
    field->selectAll();
    QTest::keyClicks(field, QString::number(value, 'g', 10));
    QTest::keyClick(field, Qt::Key_Return);
}

// AetherSDR's title plate on a panel, which tidyEmbeddedPanel() hides, or keeps when asked.
// It has no Q_OBJECT, so findChild() cannot find it.
EditorFramelessTitleBar* titlePlate(QWidget* panel)
{
    for (QObject* child : panel->children()) {
        if (auto* bar = dynamic_cast<EditorFramelessTitleBar*>(child)) return bar;
    }
    return nullptr;
}

} // namespace

class TestAetherGui : public QObject {
    Q_OBJECT

private:
    // The pages draw from the same five stages the window's display mirror holds
    // (LiveTaps::view), set up as a fresh chain sets up its own.
    esa::StageSet m_stages;
    std::unique_ptr<AudioEngine> m_engine;

    // The real window, on a real engine that is never connected unless a test hands it a
    // MockExpert, and then only to that.
    QTemporaryDir m_dir;
    std::unique_ptr<esa::Settings> m_settings;
    std::unique_ptr<esa::ProfileLibrary> m_profiles;
    std::unique_ptr<esa::Engine> m_core;
    std::unique_ptr<esa::MainWindow> m_window;

    void make(esa::AppConfig config = {})
    {
        config.deviceKey = QStringLiteral("null");  // never the speakers
        config.host = QStringLiteral("192.0.2.1");  // TEST-NET: nothing is there
        config.fallbackPorts.clear();
        config.muteExpert = false;
        const QString path = m_dir.filePath(QStringLiteral("aether.ini"));
        m_window.reset();
        if (m_core) m_core->shutdown();
        m_core = std::make_unique<esa::Engine>(path);
        m_settings = std::make_unique<esa::Settings>(path);
        // Whatever a test presses, REC never reaches the real Documents folder.
        m_settings->setRecordingFolder(m_dir.filePath(QStringLiteral("Recordings")));
        m_profiles = std::make_unique<esa::ProfileLibrary>(esa::ProfileLibrary::defaultPath(path));
        m_window = std::make_unique<esa::MainWindow>(m_core.get(), m_core->taps(), m_settings.get(), m_profiles.get(),
                                                     config);
        m_window->setQuitHandler([] {});
        // Nor does anything open a file manager or a folder dialog.
        m_window->setRevealHandler([](const QString&) {});
        m_window->setFolderChooser([](const QString&) { return QString(); });
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        QCoreApplication::processEvents();  // the engine's configure, which the window queued
    }

    esa::ParamControl* control(const QString& key) const
    {
        const auto controls = m_window->findChildren<esa::ParamControl*>();
        for (esa::ParamControl* c : controls) {
            if (c->spec().key == key) return c;
        }
        return nullptr;
    }

    esa::StageBinding* binding() const { return m_window ? m_window->findChild<esa::StageBinding*>() : nullptr; }
    // The window's stand-in for upstream's engine, which its pages are built on.
    AudioEngine* shim() const { return m_window->findChild<AudioEngine*>(); }
    esa::StageSet& view() const { return m_core->taps()->view; }

    esa::StageHost* host(const QString& stage) const
    {
        const auto hosts = m_window->findChildren<esa::StageHost*>();
        for (esa::StageHost* h : hosts) {
            if (h->stage() == stage) return h;
        }
        return nullptr;
    }

    // What the window holds for a key: what it saves and what Modified compares.
    double held(const QString& key) const
    {
        const esa::ParamSpec* spec = esa::findParamSpec(key);
        return spec ? esa::paramValue(m_window->config(), *spec) : std::nan("");
    }

    // What the chain the audio runs through holds, once the window's queued calls have run.
    double inChain(const QString& key) const
    {
        QCoreApplication::processEvents();
        double value = std::nan("");
        m_core->chainParam(key, &value);
        return value;
    }

    QCheckBox* railBox(const QString& slug) const
    {
        return m_window->findChild<QCheckBox*>(QStringLiteral("aetherRxEnable") + slug);
    }

    // Hosts `page` the way the window will, binds it once it has its parent (as AetherRX
    // does), lets its timers and layouts settle, checks it drew, and saves it when asked.
    // What it must draw is checked piece by piece, each in its own rectangle: `knobs` knobs
    // shown, each drawn, and for each of `drawn` the first shown widget of that class, with
    // more than that share of its rectangle painted. A whole-page share would pass with a
    // piece missing, since the rest of the page outweighs it.
    // `check`, if given, runs last, while the page is still on show.
    void render(QWidget* page, const QString& name, const std::function<void()>& bind, int knobs,
                const QList<QPair<const char*, double>>& drawn, const std::function<void()>& check = {})
    {
        Ground ground(page);
        bind();
        ground.show();
        QVERIFY(QTest::qWaitForWindowExposed(&ground));
        QTest::qWait(150);  // several panel ticks, so meters and curves have drawn

        const QImage shot = ground.grab().toImage();
        QCOMPARE(shot.size() / shot.devicePixelRatio(), kPageSize);
        const auto share = [&](QWidget* w) {
            const qreal dpr = shot.devicePixelRatio();
            const QRect r(w->mapTo(&ground, QPoint()), w->size());
            return paintedFraction(shot.copy(QRect(r.topLeft() * dpr, r.size() * dpr)));
        };

        // A knob draws its ring, arc and reading over about a quarter of its square.
        int shown = 0;
        const auto allKnobs = ground.findChildren<ClientCompKnob*>();
        for (ClientCompKnob* knob : allKnobs) {
            if (!knob->isVisible()) continue;
            ++shown;
            const double painted = share(knob);
            QVERIFY2(painted > 0.10, qPrintable(QStringLiteral("%1: knob %2 drew %3% of itself")
                                                    .arg(name, knob->accessibleName())
                                                    .arg(painted * 100.0, 0, 'f', 2)));
        }
        QCOMPARE(shown, knobs);

        for (const auto& [cls, least] : drawn) {
            QWidget* found = nullptr;
            const auto all = ground.findChildren<QWidget*>();
            for (QWidget* w : all) {
                if (w->isVisible() && w->inherits(cls)) {
                    found = w;
                    break;
                }
            }
            QVERIFY2(found, qPrintable(QStringLiteral("%1 shows no %2").arg(name, QLatin1String(cls))));
            const double painted = share(found);
            qInfo("%s: %s drew %.2f%% of itself", qPrintable(name), cls, painted * 100.0);
            QVERIFY2(painted > least, qPrintable(QStringLiteral("%1: %2 drew %3% of itself")
                                                     .arg(name, QLatin1String(cls))
                                                     .arg(painted * 100.0, 0, 'f', 2)));
        }

        const QString shots = qEnvironmentVariable("ESA_GUI_SHOT_DIR");
        if (!shots.isEmpty()) {
            QDir().mkpath(shots);
            QVERIFY(shot.save(QDir(shots).filePath(QStringLiteral("aether-%1.png").arg(name))));
        }
        if (check) check();
    }

private slots:
    void initTestCase()
    {
        // ThemeManager scans the config folder for user themes, and the pages cache
        // images; keep both away from real folders.
        QStandardPaths::setTestModeEnabled(true);
        // What main() does before any window: the bundled Inter, Fusion and the canon palette.
        esa::Appearance::install();

        m_stages.prepare(48000);
        m_stages.initialiseBands();
        m_engine = std::make_unique<AudioEngine>();
        AudioEngine::Hooks hooks;
        hooks.gate = &m_stages.gate;
        hooks.eq = &m_stages.eq;
        hooks.comp = &m_stages.comp;
        hooks.tube = &m_stages.tube;
        hooks.pudu = &m_stages.pudu;
        m_engine->setHooks(hooks);
    }

    void cleanupTestCase()
    {
        m_engine.reset();
    }

    void cleanup()
    {
        if (m_window) {
            // Whatever a test did, the pages never show a value the settings do not hold.
            const QString drift = binding()->firstMismatch();
            m_window.reset();
            QVERIFY2(drift.isEmpty(), qPrintable(drift));
        }
        if (m_core) m_core->shutdown();
        m_core.reset();
        m_settings.reset();
        m_profiles.reset();
        QDir dir(m_dir.path());
        const QStringList files = dir.entryList(QDir::Files);
        for (const QString& file : files) dir.remove(file);
    }

    void theThemeAndFontsAreBuiltIn()
    {
        // Resources in a static library reach the executable only through the object
        // library Qt 6 makes for them. If this fails on some Qt, add them to each
        // executable instead.
        QFile theme(QStringLiteral(":/themes/default-dark.json"));
        QVERIFY(theme.open(QIODevice::ReadOnly));
        const QJsonDocument doc = QJsonDocument::fromJson(theme.readAll());
        QCOMPARE(doc.object().value(QStringLiteral("name")).toString(), QStringLiteral("Default Dark"));

        for (const char* path : {":/fonts/Inter-Regular.ttf", ":/fonts/Inter-Bold.ttf"}) {
            QFile font(QString::fromLatin1(path));
            QVERIFY2(font.open(QIODevice::ReadOnly), path);
            QVERIFY(font.size() > 100000);
        }
        // Registered by Appearance::install(), from those resources.
        QVERIFY(QFontDatabase::families().contains(QStringLiteral("Inter")));
        QCOMPARE(QApplication::style()->name().toLower(), QStringLiteral("fusion"));
    }

    void theThemeManagerReadsTheDarkTheme()
    {
        auto& tm = ThemeManager::instance();
        // Only a theme found under :/themes/ is listed, so this says the file loaded,
        // not just the compiled-in seed.
        QVERIFY(tm.availableThemes().contains(QStringLiteral("Default Dark")));
        QCOMPARE(tm.activeTheme(), QStringLiteral("Default Dark"));
        QCOMPARE(tm.color(QStringLiteral("color.canon.ground")), canonGround());

        QWidget scoped;
        theme::setContainer(&scoped, QStringLiteral("canon/aetherRx"));
        QCOMPARE(tm.color(&scoped, QStringLiteral("color.canon.ground")), canonGround());
        // The canon scope's own knob colour (root's is blue): the page's scope resolves.
        QCOMPARE(tm.color(&scoped, QStringLiteral("color.knob.foreground")), QColor(QStringLiteral("#5de3ff")));
    }

    void theEngineShimForwardsToItsHooks()
    {
        esa::StageSet stages;
        QStringList saved;
        bool muted = false;
        bool boost = false;
        float outputDb = 0.0f;

        AudioEngine engine;
        AudioEngine::Hooks hooks;
        hooks.gate = &stages.gate;
        hooks.eq = &stages.eq;
        hooks.comp = &stages.comp;
        hooks.tube = &stages.tube;
        hooks.pudu = &stages.pudu;
        hooks.saveStage = [&](const QString& stage) { saved.append(stage); };
        hooks.muted = [&] { return muted; };
        hooks.setMuted = [&](bool on) { muted = on; };
        hooks.boost = [&] { return boost; };
        hooks.setBoost = [&](bool on) { boost = on; };
        hooks.outputDb = [&] { return outputDb; };
        hooks.setOutputDb = [&](float db) { outputDb = db; };
        hooks.copyEqSamples = [](float* out, int count) {
            for (int i = 0; i < count; ++i) out[i] = float(i);
            return true;
        };
        engine.setHooks(hooks);

        QCOMPARE(engine.clientGateRx(), &stages.gate);
        QCOMPARE(engine.clientEqRx(), &stages.eq);
        QCOMPARE(engine.clientCompRx(), &stages.comp);
        QCOMPARE(engine.clientTubeRx(), &stages.tube);
        QCOMPARE(engine.clientPuduRx(), &stages.pudu);
        // This app only receives: there is no TX stage to hand out.
        QVERIFY(!engine.clientGateTx());
        QVERIFY(!engine.clientEqTx());
        QVERIFY(!engine.clientCompTx());
        QVERIFY(!engine.clientTubeTx());
        QVERIFY(!engine.clientPuduTx());

        engine.saveClientGateRxSettings();
        engine.saveClientGateSettings();
        engine.saveClientEqSettings();
        engine.saveClientCompRxSettings();
        engine.saveClientCompSettings();
        engine.saveClientTubeRxSettings();
        engine.saveClientTubeSettings();
        engine.saveClientPuduRxSettings();
        engine.saveClientPuduSettings();
        QCOMPARE(saved, QStringList({QStringLiteral("gate"), QStringLiteral("gate"), QStringLiteral("eq"),
                                     QStringLiteral("comp"), QStringLiteral("comp"), QStringLiteral("tube"),
                                     QStringLiteral("tube"), QStringLiteral("pudu"), QStringLiteral("pudu")}));

        engine.setMuted(true);
        QVERIFY(muted && engine.isMuted());
        engine.setRxBoost(true);
        QVERIFY(boost && engine.rxBoost());
        engine.setRxOutputTrimDb(-12.5f);
        QCOMPARE(outputDb, -12.5f);
        QCOMPARE(engine.rxOutputTrimDb(), -12.5f);

        std::vector<float> samples(8, -1.0f);
        QVERIFY(engine.copyRecentClientEqRxSamples(samples.data(), int(samples.size())));
        QCOMPARE(samples[7], 7.0f);
        QVERIFY(!engine.copyRecentClientEqTxSamples(samples.data(), int(samples.size())));

        engine.setRn2TxEnabled(true);
        QVERIFY(!engine.rn2TxEnabled());

        QSignalSpy shared(&engine, &AudioEngine::scopeSamplesReady);
        QSignalSpy rx(&engine, &AudioEngine::rxPostChainScopeReady);
        QSignalSpy tx(&engine, &AudioEngine::txPostChainScopeReady);
        const QByteArray pcm(64, '\0');
        engine.publishRxScope(pcm, 24000);
        engine.publishRxPostChainScope(pcm, 24000);
        QCOMPARE(shared.count(), 1);
        QCOMPARE(shared.at(0).at(1).toInt(), 24000);
        QCOMPARE(shared.at(0).at(2).toBool(), false);
        QCOMPARE(rx.count(), 1);
        QCOMPARE(tx.count(), 0);
    }

    void anEmptyEngineShimIsSafe()
    {
        AudioEngine engine;
        QVERIFY(!engine.clientGateRx());
        QVERIFY(!engine.clientEqRx());
        engine.saveClientEqSettings();
        engine.setMuted(true);
        engine.setRxBoost(true);
        engine.setRxOutputTrimDb(6.0f);
        QVERIFY(!engine.isMuted());
        QVERIFY(!engine.rxBoost());
        QCOMPARE(engine.rxOutputTrimDb(), 0.0f);
        float sample = 0.0f;
        QVERIFY(!engine.copyRecentClientEqRxSamples(&sample, 1));

        // A panel with nothing behind it must still build and draw.
        StripGatePanel panel(&engine);
        panel.showForRx();
        QCoreApplication::processEvents();
        QVERIFY(!panel.grab().isNull());
    }

    void appSettingsKeepsOnlyItsAllowlist()
    {
        auto& settings = AppSettings::instance();
        const QString ref = QStringLiteral("ClientEqReferenceCurve");
        const QString geometry = QStringLiteral("StripGatePanelGeometry");

        // Without a store everything lives in memory.
        settings.setValue(ref, QStringLiteral("Pink"));
        QCOMPARE(settings.value(ref).toString(), QStringLiteral("Pink"));
        settings.remove(ref);
        QVERIFY(!settings.contains(ref));
        QCOMPARE(settings.value(ref, QStringLiteral("Off")).toString(), QStringLiteral("Off"));

        QHash<QString, QVariant> disk;
        // The settings are process-wide: never leave them calling into this frame.
        struct Detach {
            ~Detach() { AppSettings::instance().setStore({}); }
        } detach;
        AppSettings::Store store;
        store.read = [&](const QString& key) { return disk.value(key); };
        store.write = [&](const QString& key, const QVariant& value) { disk.insert(key, value); };
        store.remove = [&](const QString& key) { disk.remove(key); };
        settings.setStore(store);

        for (const char* key : {"ClientEqReferenceCurve", "ClientEqSmoothingFraction",
                                "AetherialStripWaveformRxWindowSec"}) {
            QVERIFY2(AppSettings::persists(QString::fromLatin1(key)), key);
        }
        settings.setValue(ref, QStringLiteral("Pink"));
        QCOMPARE(disk.value(ref).toString(), QStringLiteral("Pink"));
        QVERIFY(settings.contains(ref));
        QCOMPARE(settings.value(ref).toString(), QStringLiteral("Pink"));
        settings.save();
        settings.remove(ref);
        QVERIFY(!disk.contains(ref));

        // Geometry, the output trim and the rest stay in memory: our window owns those.
        for (const QString& key : {geometry, QStringLiteral("RxOutputTrimDb")}) {
            QVERIFY(!AppSettings::persists(key));
            settings.setValue(key, QStringLiteral("x"));
            QVERIFY(!disk.contains(key));
            QCOMPARE(settings.value(key).toString(), QStringLiteral("x"));
            settings.remove(key);
        }

        // There is one theme, whatever ThemeManager tries to save.
        settings.setValue(QStringLiteral("ActiveTheme"), QStringLiteral("Default Light"));
        QCOMPARE(settings.value(QStringLiteral("ActiveTheme")).toString(), QStringLiteral("Default Dark"));
        QVERIFY(disk.isEmpty());
    }

    void everyPanelRenders_data()
    {
        QTest::addColumn<QString>("name");
        for (const char* name : {"gate", "eq", "comp", "tube", "exciter", "output", "waveform"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    void everyPanelRenders()
    {
        QFETCH(QString, name);
        AudioEngine* engine = m_engine.get();

        // Built as AetherRX builds its pages: the panel, then the page that fits it;
        // once hosted, the binding to the RX stage, then the chrome taken off.
        QWidget* page = nullptr;
        std::function<void()> bind;
        const auto stage = [&](auto* panel, auto showForRx) {
            page = makeStagePage(panel);
            bind = [panel, showForRx] {
                showForRx(panel);
                tidyEmbeddedPanel(panel);
            };
        };
        const auto titled = [&](auto* panel) {
            page = panel;
            bind = [panel] {
                panel->showForRx();
                tidyEmbeddedPanel(panel, /*keepTitle=*/true);
            };
        };
        const auto rx = [](auto* panel) { panel->showForRx(); };

        if (name == QLatin1String("gate")) stage(new StripGatePanel(engine), rx);
        else if (name == QLatin1String("eq"))
            stage(new StripEqPanel(engine), [](StripEqPanel* p) { p->showForPath(ClientEqApplet::Path::Rx); });
        else if (name == QLatin1String("comp")) stage(new StripCompPanel(engine), rx);
        else if (name == QLatin1String("tube")) stage(new StripTubePanel(engine), rx);
        else if (name == QLatin1String("exciter")) stage(new StripPuduPanel(engine), rx);
        else if (name == QLatin1String("output")) titled(new StripRxOutputPanel(engine));
        else if (name == QLatin1String("waveform")) titled(new StripWaveformPanel(engine));
        QVERIFY(page);

        // What each page must show, from renders at this size, each well under the share drawn
        // and well above an empty ground's none. The level view, the tube's curve and the
        // waveform are mostly dark plot with thin lines and text: offscreen, on Qt 6.4 and
        // 6.10 alike, they draw 0.97 %, 0.73 % and 2.86 %, so a third of that is asked.
        int knobs = 0;
        QList<QPair<const char*, double>> drawn;
        if (name == QLatin1String("gate")) {
            knobs = 6;
            drawn = {{"AetherSDR::ClientGateLevelView", 0.003}};
        } else if (name == QLatin1String("eq")) {
            drawn = {{"AetherSDR::ClientEqEditorCanvas", 0.03}, {"AetherSDR::ClientEqOutputFader", 0.10}};
        } else if (name == QLatin1String("comp")) {
            knobs = 4;
            drawn = {{"AetherSDR::ClientCompMeter", 0.005}, {"AetherSDR::ClientCompThresholdFader", 0.005}};
        } else if (name == QLatin1String("tube")) {
            knobs = 6;
            drawn = {{"AetherSDR::ClientLevelMeter", 0.02}, {"AetherSDR::ClientTubeCurveWidget", 0.0025}};
        } else if (name == QLatin1String("exciter")) {
            knobs = 6;
            drawn = {{"AetherSDR::PooDooLogo", 0.05}};
        } else if (name == QLatin1String("output")) {
            knobs = 1;
        } else if (name == QLatin1String("waveform")) {
            drawn = {{"AetherSDR::WaveformWidget", 0.01}};
        }

        // The objects on these pages the window will look up by name and type, and what it will
        // expect of them: a sync that renames or drops one fails here, before a page that
        // relies on it is hosted.
        const auto contract = [&name, page] {
            if (name == QLatin1String("eq")) {
                QVERIFY(page->findChild<ClientEqEditorCanvas*>(QStringLiteral("stripRxEqCanvas")));
            } else if (name == QLatin1String("comp")) {
                auto* ceiling = page->findChild<QSlider*>(QStringLiteral("compCeilingSlider"));
                QVERIFY(ceiling && ceiling->isVisible());
                // The Out meter, which carries the makeup fader the makeup guard watches.
                int makeupMeters = 0;
                for (ClientCompMeter* meter : page->findChildren<ClientCompMeter*>()) {
                    if (meter->mode() == ClientCompMeter::Mode::Level && meter->makeupControlEnabled()) ++makeupMeters;
                }
                QCOMPARE(makeupMeters, 1);
            } else if (name == QLatin1String("exciter")) {
                QCOMPARE(page->findChildren<PooDooLogo*>().size(), 1);
            } else if (name == QLatin1String("output")) {
                // The one knob, which becomes Volume, under its kept title plate.
                QCOMPARE(page->findChildren<ClientCompKnob*>().size(), 1);
                EditorFramelessTitleBar* plate = titlePlate(page);
                QVERIFY(plate && plate->isVisible());
            } else if (name == QLatin1String("waveform")) {
                QCOMPARE(page->findChildren<WaveformWidget*>().size(), 1);
                EditorFramelessTitleBar* plate = titlePlate(page);
                QVERIFY(plate && plate->isVisible());
            }
        };
        render(page, name, bind, knobs, drawn, contract);
    }

    // Every guarded value field on every panel, where '.' groups thousands: given the
    // keyboard, Return and taken away again with nothing typed, none of them commits.
    // Unguarded, each knob's bare "20.000" read back as 20000.
    void noValueFieldCommitsWhatNobodyTyped_data()
    {
        QTest::addColumn<QString>("name");
        for (const char* name : {"gate", "eq", "comp", "tube", "exciter", "output"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }
    void noValueFieldCommitsWhatNobodyTyped()
    {
        QFETCH(QString, name);
        const QLocale before;
        const auto restore = qScopeGuard([before] { QLocale::setDefault(before); });
        QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));

        AudioEngine* engine = m_engine.get();
        QWidget window;
        auto* column = new QVBoxLayout(&window);
        auto* elsewhere = new QLineEdit(&window);
        column->addWidget(elsewhere);
        QWidget* panel = nullptr;
        std::function<void()> bind;
        const auto rx = [&](auto* p) {
            panel = p;
            bind = [p] { p->showForRx(); };
        };
        if (name == QLatin1String("gate")) rx(new StripGatePanel(engine));
        else if (name == QLatin1String("eq")) {
            auto* p = new StripEqPanel(engine);
            panel = p;
            bind = [p] { p->showForPath(ClientEqApplet::Path::Rx); };
        } else if (name == QLatin1String("comp")) rx(new StripCompPanel(engine));
        else if (name == QLatin1String("tube")) rx(new StripTubePanel(engine));
        else if (name == QLatin1String("exciter")) rx(new StripPuduPanel(engine));
        else if (name == QLatin1String("output")) rx(new StripRxOutputPanel(engine));
        QVERIFY(panel);
        column->addWidget(makeStagePage(panel));
        bind();
        tidyEmbeddedPanel(panel);
        esa::ValueFieldGuard* guard = esa::ValueFieldGuard::install(panel);
        window.resize(kPageSize);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));

        const auto stages = [this] {
            QHash<QString, double> values;
            for (const esa::ParamSpec& spec : esa::RxChain::stageParams()) {
                double v = 0.0;
                if (esa::RxChain::readStageParam(m_stages, spec.key, &v)) values.insert(spec.key, v);
            }
            return values;
        };
        const QHash<QString, double> stagesBefore = stages();
        QList<QPair<QString, QSignalSpy*>> spies;
        std::vector<std::unique_ptr<QSignalSpy>> owned;
        const auto spy = [&](const QString& what, QSignalSpy* s) {
            owned.emplace_back(s);
            spies.append({what, s});
        };
        for (auto* k : panel->findChildren<ClientCompKnob*>())
            spy(QStringLiteral("knob ") + k->accessibleName(), new QSignalSpy(k, &ClientCompKnob::valueChanged));
        for (auto* f : panel->findChildren<ClientCompThresholdFader*>())
            spy(QStringLiteral("threshold fader"), new QSignalSpy(f, &ClientCompThresholdFader::thresholdChanged));
        for (auto* f : panel->findChildren<ClientEqOutputFader*>())
            spy(QStringLiteral("output fader"), new QSignalSpy(f, &ClientEqOutputFader::gainChanged));

        int tried = 0;
        for (QLineEdit* field : guard->fields()) {
            if (!field->isVisible() || !field->isEnabled()) continue;
            ++tried;
            field->setFocus(Qt::MouseFocusReason);
            QTRY_VERIFY(field->hasFocus());
            QTest::keyClick(field, Qt::Key_Return);
            field->setFocus(Qt::MouseFocusReason);
            QTRY_VERIFY(field->hasFocus());
            elsewhere->setFocus(Qt::MouseFocusReason);
            QTRY_VERIFY(elsewhere->hasFocus());
        }
        QVERIFY2(!guard->fields().isEmpty(), qPrintable(name));
        // On RX the EQ's output fader only reports, with its field hidden.
        QVERIFY2(tried > 0 || name == QLatin1String("eq"), qPrintable(name));
        for (const auto& [what, s] : spies) QVERIFY2(s->isEmpty(), qPrintable(what + QStringLiteral(" committed")));
        QCOMPARE(stages(), stagesBefore);
    }

    // AetherRX's last tab stacks the output meter over the waveform.
    void theFinalOutputPageStacksTwoPanels()
    {
        auto* page = new QWidget;
        auto* col = new QVBoxLayout(page);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(8);
        auto* output = new StripRxOutputPanel(m_engine.get());
        auto* waveform = new StripWaveformPanel(m_engine.get());
        col->addWidget(output, 0, Qt::AlignTop);
        col->addWidget(waveform, 1);
        render(
            page, QStringLiteral("final-output"),
            [output, waveform] {
                output->showForRx();
                waveform->showForRx();
                tidyEmbeddedPanel(output, /*keepTitle=*/true);
                tidyEmbeddedPanel(waveform, /*keepTitle=*/true);
            },
            1, {{"AetherSDR::WaveformWidget", 0.01}},
            [output, waveform] {
                // Both title plates are kept, as AetherRX keeps them on this tab.
                for (QWidget* panel : {static_cast<QWidget*>(output), static_cast<QWidget*>(waveform)}) {
                    EditorFramelessTitleBar* plate = titlePlate(panel);
                    QVERIFY(plate && plate->isVisible());
                }
            });
    }

    // The ticks are images painted once, in colours written out as fixed values: they must
    // still be the theme's, or a ticked box would show a tick of another colour than upstream's.
    void theCheckTicksAreInTheThemesColours()
    {
        QWidget scoped;
        theme::setContainer(&scoped, QStringLiteral("canon/aetherRx"));
        const QList<QPair<QString, QString>> ticks = {
            {QStringLiteral("check-tick"), QStringLiteral("color.canon.onAccent")},
            {QStringLiteral("check-tick-disabled"), QStringLiteral("color.canon.control")},
        };
        for (const auto& [name, token] : ticks) {
            const QRgb want = ThemeManager::instance().color(&scoped, token).rgb() & 0xffffff;
            for (const QString& scale : {QString(), QStringLiteral("@2x"), QStringLiteral("@3x")}) {
                const QString file = QStringLiteral(":/canon/%1%2.png").arg(name, scale);
                const QImage image = QImage(file).convertToFormat(QImage::Format_ARGB32);
                QVERIFY2(!image.isNull(), qPrintable(file));
                int opaque = 0;
                for (int y = 0; y < image.height(); ++y) {
                    const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
                    for (int x = 0; x < image.width(); ++x) {
                        if (qAlpha(line[x]) != 255) continue;
                        ++opaque;
                        QVERIFY2((line[x] & 0xffffff) == want,
                                 qPrintable(QStringLiteral("%1 has #%2 at %3,%4 where %5 is #%6")
                                                .arg(file)
                                                .arg(line[x] & 0xffffff, 6, 16, QLatin1Char('0'))
                                                .arg(x)
                                                .arg(y)
                                                .arg(token)
                                                .arg(want, 6, 16, QLatin1Char('0'))));
                    }
                }
                QVERIFY2(opaque > 0, qPrintable(file + QStringLiteral(" has no solid pixel")));
            }
        }
    }

    // Forked for Qt 6.4 and not shown yet; this keeps it building and running.
    void theForkedNrGainStripRuns()
    {
        NrGainStrip strip;
        strip.resize(300, 40);
        strip.setGain(0.8f, true);
        strip.show();
        QVERIFY(QTest::qWaitForWindowExposed(&strip));
        QTest::qWait(100);
        strip.reset();
        QVERIFY(!strip.grab().isNull());
    }

    // ── In the window ─────────────────────────────────────────────────────────

    // The theme reaches the window and the pages in it, through the resources linked in
    // from the static library.
    void theWindowIsDrawnInTheCanonTheme()
    {
        make();
        auto& tm = ThemeManager::instance();
        QCOMPARE(tm.color(m_window.get(), QStringLiteral("color.canon.ground")), canonGround());
        esa::StageHost* gate = host(QStringLiteral("gate"));
        QVERIFY(gate);
        auto* knob = gate->panel()->findChild<ClientCompKnob*>();
        QVERIFY(knob);
        // The canon scope's own knob colour (the root theme's is blue).
        QCOMPARE(tm.color(knob, QStringLiteral("color.knob.foreground")), QColor(QStringLiteral("#5de3ff")));
    }

    // Every object the window looks up by name or type, and what it expects of each.
    void everyObjectTheWindowReliesOnIsThere()
    {
        make();
        for (const char* slug : {"Radio", "AetherNR", "Gate", "EQ", "Compressor", "Tube", "Exciter", "FinalOutput"}) {
            QVERIFY2(m_window->findChild<QPushButton*>(QStringLiteral("aetherRxTab") + QLatin1String(slug)), slug);
        }
        for (const char* slug : {"AetherNR", "Gate", "EQ", "Compressor", "Tube", "Exciter"}) {
            QVERIFY2(railBox(QLatin1String(slug)), slug);
        }
        for (const char* slug : {"Gate", "EQ", "Compressor", "Tube", "Exciter"}) {
            QVERIFY2(m_window->findChild<QWidget*>(QStringLiteral("aetherRxGrip") + QLatin1String(slug)), slug);
        }
        auto* bypass = m_window->findChild<QPushButton*>(QStringLiteral("aetherRxBypass"));
        QVERIFY(bypass && bypass->isCheckable());
        QVERIFY(m_window->findChild<QPushButton*>(QStringLiteral("aetherRxSettingsButton")));
        // REC and PLAY: checkable, named in whole words for a screen reader, and PLAY greyed
        // until something has been recorded.
        auto* record = m_window->findChild<QPushButton*>(QStringLiteral("aetherRxRecord"));
        QVERIFY(record && record->isCheckable() && !record->isChecked() && record->isEnabled());
        QCOMPARE(record->text(), QStringLiteral("REC"));
        QCOMPARE(record->accessibleName(), QStringLiteral("Record"));
        auto* play = m_window->findChild<QPushButton*>(QStringLiteral("aetherRxPlay"));
        QVERIFY(play && play->isCheckable() && !play->isChecked() && !play->isEnabled());
        QCOMPARE(play->text(), QStringLiteral("PLAY"));
        QCOMPARE(play->accessibleName(), QStringLiteral("Play"));
        QCOMPARE(record->parentWidget(), play->parentWidget());

        // The Gate page.
        esa::StageHost* gate = host(QStringLiteral("gate"));
        QVERIFY(gate);
        QCOMPARE(gate->objectName(), QStringLiteral("page_gate"));
        QWidget* panel = gate->panel();
        QVERIFY(qobject_cast<StripGatePanel*>(panel));
        for (const char* name : {"gateModeGate", "gateModeExpander", "gateViewLevel", "gateViewCurve"}) {
            auto* button = panel->findChild<QPushButton*>(QLatin1String(name));
            QVERIFY2(button && button->isCheckable(), name);
        }
        auto* peek = panel->findChild<QSlider*>(QStringLiteral("gatePeekSlider"));
        QVERIFY(peek);
        QCOMPARE(peek->minimum(), 0);
        QCOMPARE(peek->maximum(), 4);
        QCOMPARE(panel->findChildren<ClientGateLevelView*>().size(), 1);
        QCOMPARE(panel->findChildren<ClientGateCurveWidget*>().size(), 1);
        // Its knobs, one for each key the spec names, by the labels they draw.
        const auto knobs = panel->findChildren<ClientCompKnob*>();
        QCOMPARE(knobs.size(), gate->spec().knobs.size());
        for (const auto& [label, key] : gate->spec().knobs) {
            ClientCompKnob* knob = gate->knob(key);
            QVERIFY2(knob, qPrintable(key));
            QCOMPARE(knob->accessibleName(), label);
        }
        // The floating editor's title bar is taken off: the rail names the page.
        bool titleBar = false;
        for (QObject* child : panel->children()) {
            if (auto* bar = dynamic_cast<EditorFramelessTitleBar*>(child)) {
                titleBar = true;
                QVERIFY(!bar->isVisible());
            }
        }
        QVERIFY(titleBar);
    }

    // Each named knob edits the key it is named for, and the Peek slider's stops are the
    // ones the window counts as shown exactly.
    void eachKnobDrivesItsOwnKey()
    {
        make();
        m_window->showPage(QStringLiteral("gate"));
        esa::StageHost* gate = host(QStringLiteral("gate"));
        QVERIFY(gate);
        for (const auto& [label, key] : gate->spec().knobs) {
            const esa::ParamSpec* spec = esa::findParamSpec(key);
            QVERIFY2(spec, qPrintable(key));
            const double value = roundedTo(spec->min + 0.3 * (spec->max - spec->min), spec->decimals);
            QVERIFY(std::abs(value - held(key)) > 1e-9);
            type(gate->knob(key), value);
            QVERIFY2(std::abs(held(key) - value) < 1e-9,
                     qPrintable(QStringLiteral("%1 (%2) holds %3, typed %4").arg(label, key).arg(held(key)).arg(value)));
            QCOMPARE(control(key)->value(), value);
        }

        auto* peek = gate->panel()->findChild<QSlider*>(QStringLiteral("gatePeekSlider"));
        QVERIFY(peek);
        QList<double> stops;
        for (int i = peek->maximum(); i >= peek->minimum(); --i) {
            peek->setValue(i);
            stops.prepend(held(QStringLiteral("gate.lookahead")));
            QVERIFY(gate->spec().showsExactly(QStringLiteral("gate.lookahead"), stops.first()));
        }
        QCOMPARE(stops, (QList<double>{0.0, 1.0, 1.5, 3.0, 5.0}));
        QVERIFY(!gate->spec().showsExactly(QStringLiteral("gate.lookahead"), 2.3));
    }

    // Every key the page owns: set in the mirror with upstream's setter, then saved as a
    // panel saves, it reaches the control, the settings and the chain, on the table's
    // decimals.
    void everyOwnedKeyIsReadBackFromThePage_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<double>("set");       // what the panel would write
        QTest::addColumn<double>("expected");  // on the table's decimals
        QTest::newRow("threshold") << QStringLiteral("gate.threshold") << -33.37 << -33.4;
        QTest::newRow("ratio") << QStringLiteral("gate.ratio") << 3.73 << 3.7;
        QTest::newRow("release") << QStringLiteral("gate.release") << 123.46 << 123.5;
        QTest::newRow("hold") << QStringLiteral("gate.hold") << 45.66 << 45.7;
        QTest::newRow("floor") << QStringLiteral("gate.floor") << -27.34 << -27.3;
        QTest::newRow("return") << QStringLiteral("gate.return") << 3.456 << 3.46;
        QTest::newRow("lookahead") << QStringLiteral("gate.lookahead") << 2.34 << 2.3;
        QTest::newRow("mode") << QStringLiteral("gate.mode") << 1.0 << 1.0;
    }
    void everyOwnedKeyIsReadBackFromThePage()
    {
        QFETCH(QString, key);
        QFETCH(double, set);
        QFETCH(double, expected);
        make();
        QVERIFY(binding()->ownedKeys(QStringLiteral("gate")).contains(key));

        ClientGate& g = view().gate;
        const float v = float(set);
        if (key == QLatin1String("gate.threshold")) g.setThresholdDb(v);
        else if (key == QLatin1String("gate.ratio")) g.setRatio(v);
        else if (key == QLatin1String("gate.release")) g.setReleaseMs(v);
        else if (key == QLatin1String("gate.hold")) g.setHoldMs(v);
        else if (key == QLatin1String("gate.floor")) g.setFloorDb(v);
        else if (key == QLatin1String("gate.return")) g.setReturnDb(v);
        else if (key == QLatin1String("gate.lookahead")) g.setLookaheadMs(v);
        else if (key == QLatin1String("gate.mode")) g.setMode(ClientGate::Mode::Gate);
        shim()->saveClientGateRxSettings();

        QCOMPARE(control(key)->value(), expected);
        QCOMPARE(held(key), expected);
        QVERIFY2(std::abs(inChain(key) - expected) < 1e-4, qPrintable(QString::number(inChain(key))));
        if (key == QLatin1String("gate.mode")) {
            // The mode's preset came along with it.
            QCOMPARE(held(QStringLiteral("gate.ratio")), 10.0);
            QCOMPARE(held(QStringLiteral("gate.floor")), -40.0);
            QVERIFY(std::abs(inChain(QStringLiteral("gate.ratio")) - 10.0) < 1e-4);
        }
        // The mirror took the rounded value back.
        double mirrored = 0.0;
        QVERIFY(esa::RxChain::readStageParam(view(), key, &mirrored));
        QVERIFY(std::abs(mirrored - expected) < 1e-4);
    }

    // Only half a step or more is a change: what reads back within that is the value held,
    // passed through a float.
    void lessThanHalfAStepIsNoChange()
    {
        make();
        QSignalSpy sent(control(QStringLiteral("gate.threshold")), &esa::ParamControl::valueChanged);
        view().gate.setThresholdDb(-40.04f);
        shim()->saveClientGateRxSettings();
        QCOMPARE(sent.count(), 0);
        QCOMPARE(held(QStringLiteral("gate.threshold")), -40.0);
        view().gate.setThresholdDb(-40.06f);
        shim()->saveClientGateRxSettings();
        QCOMPARE(sent.count(), 1);
        QCOMPARE(held(QStringLiteral("gate.threshold")), -40.1);
    }

    // A page's keys are its own: a save never takes a key the page does not edit, and what
    // the page wrote there on its own is put back from the settings, as upstream's forced
    // writes would be.
    void aSaveNeverTakesAKeyThePageDoesNotOwn()
    {
        make();
        QVERIFY(!binding()->ownedKeys(QStringLiteral("gate")).contains(QStringLiteral("gate.attack")));
        QVERIFY(!binding()->ownedKeys(QStringLiteral("gate")).contains(QStringLiteral("gate.enabled")));
        QSignalSpy sent(control(QStringLiteral("gate.attack")), &esa::ParamControl::valueChanged);
        view().gate.setAttackMs(42.0f);
        view().gate.setEnabled(true);
        QVERIFY(binding()->firstMismatch().startsWith(QStringLiteral("gate.enabled")));
        shim()->saveClientGateRxSettings();
        QCOMPARE(sent.count(), 0);
        QCOMPARE(held(QStringLiteral("gate.attack")), double(ClientGate::kAttackMs));
        QCOMPARE(held(QStringLiteral("gate.enabled")), 0.0);
        QCOMPARE(binding()->firstMismatch(), QString());
        QVERIFY(!view().gate.isEnabled());
        QVERIFY(!m_window->chainModified());
    }

    // The done-when case: a knob turned on the page changes the settings, the control in
    // the gate's exact-values window and the chain the audio runs through.
    void aKnobTurnedOnThePageReachesEverything()
    {
        make();
        m_window->showPage(QStringLiteral("gate"));
        esa::StageHost* gate = host(QStringLiteral("gate"));
        ClientCompKnob* thresh = gate->knob(QStringLiteral("gate.threshold"));
        QVERIFY(thresh);
        QCOMPARE(thresh->value(), -40.0f);
        drag(thresh, -20);  // a fifth of its 200 px travel up: -40 dB + 0.1 x 80 dB

        const QString key = QStringLiteral("gate.threshold");
        QCOMPARE(held(key), -32.0);
        esa::ParamControl* exact = control(key);
        QCOMPARE(exact->value(), -32.0);
        auto* details = m_window->findChild<esa::DetailsWindow*>(QStringLiteral("details_gate"));
        QVERIFY(details && details->controls().contains(exact));
        QVERIFY(std::abs(inChain(key) - -32.0) < 1e-4);
        QVERIFY(m_window->chainModified());
    }

    // Opening every page, in order, writes nothing: a sync that adds a forced write
    // shows here.
    void openingEveryPageChangesNothing()
    {
        make();
        // Somewhere other than the factory settings, saved so Modified starts clear.
        control(QStringLiteral("gate.threshold"))->setValue(-59.5);
        control(QStringLiteral("gate.ratio"))->setValue(3.7);
        control(QStringLiteral("gate.attack"))->setValue(7.5);
        control(QStringLiteral("gate.lookahead"))->setValue(2.3);
        control(QStringLiteral("comp.drive"))->setValue(3.0);
        control(QStringLiteral("eq.master"))->setValue(-2.0);
        QVERIFY(m_window->saveProfileAs(QStringLiteral("Before")));
        const QHash<QString, double> before = m_window->config().params;
        QVERIFY(!m_window->chainModified());

        for (const QString& id : m_window->pageIds()) {
            m_window->showPage(id);
            QTest::qWait(60);  // several panel ticks
        }
        QCOMPARE(m_window->config().params, before);
        QVERIFY(!m_window->chainModified());
    }

    // A profile with every stage key away from its default: the mirror equals the
    // settings for every key, and the knobs show the values.
    void aProfileLoadReachesThePages()
    {
        make();
        esa::ChainSettings chain;
        for (const esa::ParamSpec& spec : esa::RxChain::stageParams()) chain.params.insert(spec.key, nonDefault(spec));
        QVERIFY(m_profiles->save(QStringLiteral("Everything"), chain));
        m_window->loadProfile(QStringLiteral("Everything"));

        QCOMPARE(binding()->firstMismatch(), QString());
        for (const esa::ParamSpec& spec : esa::RxChain::stageParams()) {
            QCOMPARE(control(spec.key)->value(), chain.params.value(spec.key));
        }
        // The Gate mode is a preset, and the saved ratio still wins over it.
        QCOMPARE(chain.params.value(QStringLiteral("gate.mode")), 1.0);
        QVERIFY(std::abs(view().gate.ratio() - chain.params.value(QStringLiteral("gate.ratio"))) < 1e-4);

        m_window->showPage(QStringLiteral("gate"));
        esa::StageHost* gate = host(QStringLiteral("gate"));
        for (const auto& [label, key] : gate->spec().knobs) {
            const double value = chain.params.value(key);
            QVERIFY2(std::abs(gate->knob(key)->value() - value) < 1e-3,
                     qPrintable(QStringLiteral("%1 shows %2, not %3").arg(label).arg(gate->knob(key)->value()).arg(value)));
        }
        QVERIFY(gate->panel()->findChild<QPushButton*>(QStringLiteral("gateModeGate"))->isChecked());
        QVERIFY(!gate->panel()->findChild<QPushButton*>(QStringLiteral("gateModeExpander"))->isChecked());
        QVERIFY(!m_window->chainModified());
    }

    // A click on Gate sends mode, ratio and floor, in that order; a click on the
    // Expander already lit leaves a tuned ratio alone.
    void theModeButtonsSendModeThenItsPresetAndTheLitOneIsGuarded()
    {
        make();
        m_window->showPage(QStringLiteral("gate"));
        QWidget* panel = host(QStringLiteral("gate"))->panel();
        auto* gateButton = panel->findChild<QPushButton*>(QStringLiteral("gateModeGate"));
        auto* expander = panel->findChild<QPushButton*>(QStringLiteral("gateModeExpander"));
        QVERIFY(gateButton && expander);
        QVERIFY(expander->isChecked());

        QStringList sent;
        for (esa::ParamControl* c : m_window->findChildren<esa::ParamControl*>()) {
            connect(c, &esa::ParamControl::valueChanged, this, [&sent](const QString& key) { sent << key; });
        }
        QTest::mouseClick(gateButton, Qt::LeftButton);
        QCOMPARE(sent, (QStringList{QStringLiteral("gate.mode"), QStringLiteral("gate.ratio"), QStringLiteral("gate.floor")}));
        QCOMPARE(held(QStringLiteral("gate.mode")), 1.0);
        QCOMPARE(held(QStringLiteral("gate.ratio")), 10.0);
        QCOMPARE(held(QStringLiteral("gate.floor")), -40.0);
        QVERIFY(std::abs(inChain(QStringLiteral("gate.ratio")) - 10.0) < 1e-4);

        QTest::mouseClick(expander, Qt::LeftButton);
        QCOMPARE(held(QStringLiteral("gate.mode")), 0.0);
        QCOMPARE(held(QStringLiteral("gate.ratio")), 2.0);
        type(host(QStringLiteral("gate"))->knob(QStringLiteral("gate.ratio")), 3.7);
        QCOMPARE(held(QStringLiteral("gate.ratio")), 3.7);

        sent.clear();
        QTest::mouseClick(expander, Qt::LeftButton);
        QTest::mouseDClick(expander, Qt::LeftButton);
        expander->setFocus();
        QTest::keyClick(expander, Qt::Key_Space);
        // From code, a shortcut or an accessibility press, none of which pass as events.
        expander->click();
        expander->animateClick();
        QTest::qWait(300);  // the animated click's own timer
        QVERIFY2(sent.isEmpty(), qPrintable(sent.join(u' ')));
        QVERIFY(expander->isChecked());
        QCOMPARE(held(QStringLiteral("gate.ratio")), 3.7);
        QVERIFY(std::abs(inChain(QStringLiteral("gate.ratio")) - 3.7) < 1e-4);
        // The ratio's field lost the keyboard to the button without committing its own
        // "3.7:1" reading, which upstream's knob reads back as 3.71.
        QVERIFY2(std::abs(view().gate.ratio() - 3.7f) < 1e-4f, qPrintable(QString::number(view().gate.ratio())));

        // Once a click is over the lit button is itself again: unlit, it changes the mode.
        gateButton->click();
        QCOMPARE(held(QStringLiteral("gate.mode")), 1.0);
        QTest::qWait(20);
        expander->animateClick();
        QTRY_COMPARE(held(QStringLiteral("gate.mode")), 0.0);
        QCOMPARE(held(QStringLiteral("gate.ratio")), 2.0);
    }

    // A value field commits only what the operator typed in it, once. Without the guard,
    // leaving a Release field nobody typed in rounded 182.1 ms to its "182 ms" reading.
    void aValueFieldCommitsOnlyWhatWasTyped()
    {
        make();
        m_window->showPage(QStringLiteral("gate"));
        esa::StageHost* gate = host(QStringLiteral("gate"));
        const QString key = QStringLiteral("gate.release");
        ClientCompKnob* release = gate->knob(key);
        QVERIFY(release);
        auto* field = release->findChild<QLineEdit*>();
        auto* elsewhere = gate->knob(QStringLiteral("gate.threshold"))->findChild<QLineEdit*>();
        QVERIFY(field && elsewhere);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        const auto focus = [](QLineEdit* f) {
            f->setFocus(Qt::MouseFocusReason);
            QTRY_VERIFY(f->hasFocus());
        };
        // Everything that holds the value: the settings, the control, the mirror, the chain.
        const auto everywhere = [&](double expected) {
            double mirrored = 0.0;
            QVERIFY(esa::RxChain::readStageParam(view(), key, &mirrored));
            const QString where = QStringLiteral("held %1, control %2, mirror %3, chain %4, wanted %5")
                                      .arg(held(key)).arg(control(key)->value()).arg(mirrored).arg(inChain(key)).arg(expected);
            QVERIFY2(std::abs(held(key) - expected) < 1e-9 && std::abs(control(key)->value() - expected) < 1e-9
                         && std::abs(mirrored - expected) < 1e-3 && std::abs(inChain(key) - expected) < 1e-3,
                     qPrintable(where));
        };

        // A fraction above 100 ms, where the reading shows none, from a drag.
        drag(release, -20);  // a tenth of its travel up from 100 ms: 5 x 400^0.6
        QCOMPARE(held(key), 182.1);
        QCOMPARE(field->text(), QStringLiteral("182 ms"));
        QVERIFY(m_window->saveProfileAs(QStringLiteral("Dragged")));
        QVERIFY(!m_window->chainModified());

        // Its number clicked and left; then Return in it, and left.
        focus(field);
        focus(elsewhere);
        everywhere(182.1);
        focus(field);
        QTest::keyClick(field, Qt::Key_Return);
        everywhere(182.1);
        focus(elsewhere);
        everywhere(182.1);
        QVERIFY(!m_window->chainModified());
        QCOMPARE(field->text(), QStringLiteral("182 ms"));

        // Typed with Return, then left: the typed value stays.
        type(release, 150.5);
        everywhere(150.5);
        focus(elsewhere);
        everywhere(150.5);
        QVERIFY(m_window->chainModified());

        // Typed and left without Return: leaving commits it, as upstream means it to.
        focus(field);
        field->selectAll();
        QTest::keyClicks(field, QStringLiteral("160.5"));
        focus(elsewhere);
        everywhere(160.5);

        // Typed, then Escape: thrown away, and nothing when the field is left.
        focus(field);
        field->selectAll();
        QTest::keyClicks(field, QStringLiteral("170"));
        QTest::keyClick(field, Qt::Key_Escape);
        focus(elsewhere);
        everywhere(160.5);
    }

    // Where '.' groups thousands, the bare "20.000" a field shows is still 20, and what the
    // operator types in the locale's own form is read in it.
    void aValueFieldReadsItsOwnNumberInAnyLocale()
    {
        const QLocale before;
        const auto restore = qScopeGuard([before] { QLocale::setDefault(before); });
        QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
        QCOMPARE(esa::ValueFieldGuard::readAsWritten(QStringLiteral("20.000")), QStringLiteral("20"));
        QCOMPARE(esa::ValueFieldGuard::readAsWritten(QStringLiteral("182.100")), QStringLiteral("182,1"));
        QCOMPARE(esa::ValueFieldGuard::readAsWritten(QStringLiteral("150.5")), QStringLiteral("150.5"));  // falls back to C
        QCOMPARE(esa::ValueFieldGuard::readAsWritten(QStringLiteral("40,5")), QStringLiteral("40,5"));
        QCOMPARE(esa::ValueFieldGuard::readAsWritten(QStringLiteral("-18.0 dB")), QStringLiteral("-18.0 dB"));

        make();
        m_window->showPage(QStringLiteral("gate"));
        const QString key = QStringLiteral("gate.hold");
        auto* field = host(QStringLiteral("gate"))->knob(key)->findChild<QLineEdit*>();
        auto* elsewhere = host(QStringLiteral("gate"))->knob(QStringLiteral("gate.threshold"))->findChild<QLineEdit*>();
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        field->setFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(field->hasFocus());
        QCOMPARE(field->text(), QStringLiteral("20.000"));
        QTest::keyClick(field, Qt::Key_Return);
        QCOMPARE(held(key), 20.0);
        QVERIFY(std::abs(inChain(key) - 20.0) < 1e-4);

        field->selectAll();
        QTest::keyClicks(field, QStringLiteral("35.000"));
        QTest::keyClick(field, Qt::Key_Return);
        QCOMPARE(held(key), 35.0);
        field->selectAll();
        QTest::keyClicks(field, QStringLiteral("40,5"));
        QTest::keyClick(field, Qt::Key_Return);
        QCOMPARE(held(key), 40.5);
        elsewhere->setFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(elsewhere->hasFocus());
        QCOMPARE(held(key), 40.5);
        QVERIFY(std::abs(inChain(key) - 40.5) < 1e-4);
    }

    // A trackpad's small wheel steps add up to a knob's notch, and a mouse's notch is
    // still one.
    void smallWheelStepsAddUpToANotch()
    {
        make();
        m_window->showPage(QStringLiteral("gate"));
        const QString key = QStringLiteral("gate.threshold");
        ClientCompKnob* knob = host(QStringLiteral("gate"))->knob(key);
        QVERIFY(knob);
        QSignalSpy turned(knob, &ClientCompKnob::valueChanged);

        for (int i = 0; i < 7; ++i) wheel(knob, 15);
        QCOMPARE(turned.count(), 0);
        QCOMPARE(held(key), -40.0);
        wheel(knob, 15);  // the eighth: 120, one notch, 1 % of the knob's travel
        QCOMPARE(turned.count(), 1);
        QCOMPARE(held(key), -39.2);

        // Over its value field, which hands its wheel steps to the knob.
        auto* field = knob->findChild<QLineEdit*>();
        QVERIFY(field);
        for (int i = 0; i < 8; ++i) wheel(field, 15);
        QCOMPARE(held(key), -38.4);

        wheel(knob, 120);
        QCOMPARE(held(key), -37.6);
        wheel(knob, -240);
        QCOMPARE(held(key), -39.2);

        // Turning back starts afresh rather than paying off what was left the other way.
        for (int i = 0; i < 4; ++i) wheel(knob, 15);
        for (int i = 0; i < 8; ++i) wheel(knob, -15);
        QCOMPARE(held(key), -40.0);
    }

    // BYPASS is the engine's, for the session: the rail's boxes grey out and keep their
    // ticks, the settings do not change, and the radio is sent nothing.
    void bypassGreysTheRailAndChangesNothing()
    {
        MockExpert radio;
        QVERIFY(radio.listen());
        make();
        esa::DiscoveredRadio picked;
        picked.host = QStringLiteral("127.0.0.1");
        picked.port = radio.port();
        m_window->connectToRadio(picked);
        QTRY_VERIFY_WITH_TIMEOUT(radio.audioStartCount() == 1, 6000);

        control(QStringLiteral("gate.enabled"))->setValue(1);
        control(QStringLiteral("comp.enabled"))->setValue(1);
        QVERIFY(m_window->saveProfileAs(QStringLiteral("Before")));
        const QHash<QString, double> before = m_window->config().params;
        const QStringList slugs{QStringLiteral("AetherNR"), QStringLiteral("Gate"), QStringLiteral("EQ"),
                                QStringLiteral("Compressor"), QStringLiteral("Tube"), QStringLiteral("Exciter")};
        QHash<QString, bool> ticks;
        for (const QString& slug : slugs) {
            QVERIFY(railBox(slug)->isEnabled());
            ticks.insert(slug, railBox(slug)->isChecked());
        }
        QVERIFY(ticks.value(QStringLiteral("Gate")) && !ticks.value(QStringLiteral("EQ")));
        QTest::qWait(300);  // the link settles
        const qsizetype commandsBefore = radio.commands.size();

        auto* bypass = m_window->findChild<QPushButton*>(QStringLiteral("aetherRxBypass"));
        bypass->click();
        QTRY_VERIFY(!railBox(QStringLiteral("Gate"))->isEnabled());
        for (const QString& slug : slugs) {
            QVERIFY2(!railBox(slug)->isEnabled(), qPrintable(slug));
            QCOMPARE(railBox(slug)->isChecked(), ticks.value(slug));
        }
        QVERIFY(bypass->isChecked());
        const QString shots = qEnvironmentVariable("ESA_GUI_SHOT_DIR");
        if (!shots.isEmpty()) QVERIFY(m_window->grab().save(QDir(shots).filePath(QStringLiteral("window-bypassed.png"))));
        railBox(QStringLiteral("Gate"))->click();  // greyed: nothing happens
        QCOMPARE(held(QStringLiteral("gate.enabled")), 1.0);
        QCOMPARE(m_window->config().params, before);
        QVERIFY(!m_window->chainModified());

        bypass->click();
        QTRY_VERIFY(railBox(QStringLiteral("Gate"))->isEnabled());
        QVERIFY(!bypass->isChecked());
        for (const QString& slug : slugs) QCOMPARE(railBox(slug)->isChecked(), ticks.value(slug));

        // The button shows what the engine reports, however BYPASS was set.
        m_core->setRxBypassed(true);
        QTRY_VERIFY(bypass->isChecked());
        QTRY_VERIFY(!railBox(QStringLiteral("Tube"))->isEnabled());
        m_core->setRxBypassed(false);
        QTRY_VERIFY(!bypass->isChecked());

        QTest::qWait(300);
        QVERIFY2(radio.commands.size() == commandsBefore, qPrintable(radio.commands.mid(commandsBefore).join(u' ')));
        QCOMPARE(m_window->config().params, before);
    }

    // The window as main() runs it, with the engine on a thread of its own where every other
    // test here keeps it on this one: the Gate page drawing meters the engine copies in while
    // audio flows, REC, BYPASS, a gate mode change, PLAY, every page, the custom frame, and
    // teardown in the app's order. The CI's ThreadSanitizer job runs this suite.
    void theWindowRunsWithTheEngineOnItsOwnThread()
    {
        esa::WindowChrome::overrideCustomMode(esa::WindowChrome::Mode::Frameless);
        const auto restoreFrame = qScopeGuard([] { esa::WindowChrome::overrideCustomMode(std::nullopt); });
        MockExpert radio;
        QVERIFY(radio.listen());
        QTemporaryDir dir;
        const QString ini = dir.filePath(QStringLiteral("threaded.ini"));
        auto* engine = new esa::Engine(ini);
        const std::shared_ptr<esa::LiveTaps> taps = engine->taps();
        QThread thread;
        engine->moveToThread(&thread);
        thread.start();
        // However the test ends, the engine goes after the window, as in main(): stopped and
        // deleted on its own thread, which is then joined.
        auto stopEngine = qScopeGuard([&] {
            QMetaObject::invokeMethod(engine, &esa::Engine::shutdown, Qt::BlockingQueuedConnection);
            QMetaObject::invokeMethod(engine, [engine] { delete engine; }, Qt::BlockingQueuedConnection);
            thread.quit();
            thread.wait();
        });

        esa::Settings settings(ini);
        settings.setRecordingFolder(dir.filePath(QStringLiteral("Recordings")));
        esa::ProfileLibrary profiles(esa::ProfileLibrary::defaultPath(ini));
        esa::AppConfig config;
        config.deviceKey = QStringLiteral("null");  // never the speakers
        config.host = QStringLiteral("127.0.0.1");
        config.port = radio.port();
        config.fallbackPorts.clear();  // never the real radio's ports
        config.muteExpert = false;
        for (const QString& stage : esa::RxChain::stageIds()) config.params.insert(stage + QStringLiteral(".enabled"), 1);
        config.params.insert(QStringLiteral("gate.threshold"), -10);
        auto window = std::make_unique<esa::MainWindow>(engine, taps, &settings, &profiles, config);
        window->setQuitHandler([] {});
        window->setRevealHandler([](const QString&) {});
        window->setFolderChooser([](const QString&) { return QString(); });
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        window->showPage(QStringLiteral("gate"));

        esa::DiscoveredRadio picked;
        picked.host = QStringLiteral("127.0.0.1");
        picked.port = radio.port();
        window->connectToRadio(picked);
        QTRY_VERIFY_WITH_TIMEOUT(radio.audioStartCount() >= 1, 8000);
        // The page's meters, as the engine's thread copies them in.
        QTRY_VERIFY_WITH_TIMEOUT(taps->view.gate.inputPeakDb() > -100.0f, 8000);
        QTest::qWait(1000);

        auto* record = window->findChild<QPushButton*>(QStringLiteral("aetherRxRecord"));
        auto* play = window->findChild<QPushButton*>(QStringLiteral("aetherRxPlay"));
        auto* bypass = window->findChild<QPushButton*>(QStringLiteral("aetherRxBypass"));
        QVERIFY(record && play && bypass);
        record->click();
        QTRY_VERIFY_WITH_TIMEOUT(record->isChecked(), 3000);
        QTest::qWait(1200);
        bypass->click();
        QTRY_VERIFY_WITH_TIMEOUT(bypass->isChecked(), 3000);
        for (esa::ParamControl* c : window->findChildren<esa::ParamControl*>()) {
            if (c->spec().key == QLatin1String("gate.mode")) c->setValue(1);
        }
        QTest::qWait(300);
        bypass->click();
        QTRY_VERIFY_WITH_TIMEOUT(!bypass->isChecked(), 3000);
        record->click();
        QTRY_VERIFY_WITH_TIMEOUT(!record->isChecked() && play->isEnabled(), 5000);
        play->click();
        QTRY_VERIFY_WITH_TIMEOUT(play->isChecked(), 3000);
        QTest::qWait(300);
        for (const QString& id : window->pageIds()) {
            window->showPage(id);
            QTest::qWait(80);
        }
        window->showPage(QStringLiteral("gate"));

        // The custom frame, chosen from the gear's Window frame menu while PLAY plays.
        auto* gear = window->findChild<QMenu*>(QStringLiteral("gearMenu"));
        QVERIFY(gear);
        QAction* custom = nullptr;
        const std::function<void(const QMenu*)> find = [&](const QMenu* menu) {
            for (QAction* action : menu->actions()) {
                if (action->menu()) find(action->menu());
                else if (action->text() == QLatin1String("Custom")) custom = action;
            }
        };
        find(gear);
        QVERIFY(custom);
        custom->trigger();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(!play->isChecked(), 8000);  // the recording played to its end
        const QStringList recorded = QDir(dir.filePath(QStringLiteral("Recordings"))).entryList({QStringLiteral("*.wav")});
        QCOMPARE(recorded.size(), 1);

        window.reset();
        stopEngine.dismiss();
        QMetaObject::invokeMethod(engine, &esa::Engine::shutdown, Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(engine, [engine] { delete engine; }, Qt::BlockingQueuedConnection);
        thread.quit();
        QVERIFY(thread.wait(5000));
        QCOMPARE(taps.use_count(), 1);
        QStringList changing;
        for (const QString& c : std::as_const(radio.commands)) {
            if (c.startsWith(QLatin1String("trx")) || c.startsWith(QLatin1String("volume"))
                || c.startsWith(QLatin1String("mute"))) {
                changing << c;
            }
        }
        QVERIFY2(changing.isEmpty(), qPrintable(changing.join(u' ')));
    }

    // With no radio the stage meters rest, rather than holding the last block's reading.
    void theMetersRestWithNoRadio()
    {
        make();
        m_window->showPage(QStringLiteral("gate"));
        // Meters as the engine copies them from a gate that has just run on a loud tone.
        ClientGate loud;
        loud.prepare(48000);
        loud.setEnabled(true);
        std::vector<float> tone(2 * 4800);
        for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.5f * float(std::sin(double(i / 2) * 0.06));
        loud.process(tone.data(), 4800, 2);
        view().gate.copyMeteringFrom(loud);
        QVERIFY(view().gate.inputPeakDb() > -20.0f);

        QTRY_COMPARE_WITH_TIMEOUT(view().gate.inputPeakDb(), -120.0f, 1000);
        QCOMPARE(view().gate.outputPeakDb(), -120.0f);
        QCOMPARE(view().gate.gainReductionDb(), 0.0f);
    }

    // ── The machinery under every stage page ──────────────────────────────────

    // One control per key, so whatever changes a parameter goes through the one path; the
    // gate's live in its exact-values window, those its page shows first.
    void everyKeyHasExactlyOneControl()
    {
        make();
        QHash<QString, int> count;
        for (esa::ParamControl* c : m_window->findChildren<esa::ParamControl*>()) ++count[c->spec().key];
        for (const esa::ParamSpec& spec : esa::allParamSpecs()) QCOMPARE(count.value(spec.key), 1);
        QCOMPARE(count.size(), esa::allParamSpecs().size());

        QStringList onPage;
        for (const esa::ParamSpec& spec : esa::RxChain::stageParams()) {
            if (esa::StageBinding::stageOf(spec.key) == QLatin1String("gate")) onPage << spec.key;
        }
        const QStringList notOnPage = esa::DetailsWindow::keysNotOnPage(QStringLiteral("gate"));
        QCOMPARE(notOnPage, (QStringList{QStringLiteral("gate.attack"), QStringLiteral("gate.lookahead")}));
        for (const QString& key : notOnPage) onPage.removeAll(key);
        // The stage's on/off is the rail's box, which the page does not show.
        const QStringList onRail{QStringLiteral("gate.enabled")};
        onPage.removeAll(onRail.first());
        auto* details = m_window->findChild<esa::DetailsWindow*>(QStringLiteral("details_gate"));
        QVERIFY(details && !details->isVisible());
        QStringList shown;
        for (esa::ParamControl* c : details->controls()) shown << c->spec().key;
        QCOMPARE(shown, onRail + onPage + notOnPage);
        QStringList headings;
        for (QLabel* label : details->findChildren<QLabel*>(QStringLiteral("SectionLabel"))) headings << label->text();
        QCOMPARE(headings, (QStringList{QStringLiteral("ON THE RAIL"), QStringLiteral("ON THE PAGE"),
                                        QStringLiteral("NOT ON THE PAGE")}));

        // Every stage has its window; the stages still on today's grids show their controls
        // there instead, so theirs hold none yet.
        for (const QString& stage : esa::RxChain::stageIds()) {
            auto* window = m_window->findChild<esa::DetailsWindow*>(QStringLiteral("details_") + stage);
            QVERIFY2(window, qPrintable(stage));
            QCOMPARE(window->parent(), m_window.get());
            QCOMPARE(window->hasControls(), stage == QLatin1String("gate"));
            for (const QString& key : esa::DetailsWindow::keysNotOnPage(stage)) {
                QVERIFY2(esa::StageBinding::stageOf(key) == stage, qPrintable(key));
            }
        }
    }

    // The binding on its own: saves before it goes live are ignored, and owned keys are
    // pulled in table order whatever order they were given in.
    void savesBeforeTheBindingGoesLiveAreIgnored()
    {
        esa::StageSet stages;
        stages.prepare(48000);
        stages.initialiseBands();
        QHash<QString, double> held;
        QHash<QString, esa::ParamControl*> controls;
        QWidget host;
        auto* grid = new QGridLayout(&host);
        int row = 0;
        for (const esa::ParamSpec& spec : esa::RxChain::stageParams()) {
            if (esa::StageBinding::stageOf(spec.key) != QLatin1String("gate")) continue;
            auto* control = new esa::ParamControl(spec, spec.def, grid, row++, &host);
            controls.insert(spec.key, control);
        }
        esa::StageBinding::Host bindingHost;
        bindingHost.value = [&](const QString& key) { return held.value(key, esa::findParamSpec(key)->def); };
        bindingHost.control = [&](const QString& key) { return controls.value(key); };
        esa::StageBinding binding(&stages, bindingHost);
        QStringList order;
        for (esa::ParamControl* control : std::as_const(controls)) {
            connect(control, &esa::ParamControl::valueChanged, &binding, [&](const QString& key, double value) {
                held.insert(key, value);
                order << key;
                binding.apply(key, value);
                binding.pull(QStringLiteral("gate"));
            });
        }
        binding.own(QStringLiteral("gate"), {QStringLiteral("gate.floor"), QStringLiteral("gate.ratio"),
                                             QStringLiteral("gate.mode"), QStringLiteral("eq.master")});
        QCOMPARE(binding.ownedKeys(QStringLiteral("gate")),
                 (QStringList{QStringLiteral("gate.mode"), QStringLiteral("gate.ratio"), QStringLiteral("gate.floor")}));
        binding.seed();

        AudioEngine::Hooks hooks;
        binding.fillHooks(&hooks);
        QCOMPARE(hooks.gate, &stages.gate);
        QCOMPARE(hooks.pudu, &stages.pudu);
        stages.gate.setMode(ClientGate::Mode::Gate);
        hooks.saveStage(QStringLiteral("gate"));
        QVERIFY(order.isEmpty());
        QVERIFY(held.isEmpty());

        binding.goLive();
        hooks.saveStage(QStringLiteral("gate"));
        QCOMPARE(order, (QStringList{QStringLiteral("gate.mode"), QStringLiteral("gate.ratio"), QStringLiteral("gate.floor")}));
        QCOMPARE(held.value(QStringLiteral("gate.ratio")), 10.0);
        QCOMPARE(binding.firstMismatch(), QString());
    }

    // The debug check: a mirror that drifts from the settings is logged, once.
    void aMirrorThatDriftsIsLogged()
    {
        make();
        QCOMPARE(binding()->firstMismatch(), QString());
        view().gate.setAttackMs(42.0f);  // as a page writing a key it does not own, unsaved
        QVERIFY(binding()->firstMismatch().startsWith(QStringLiteral("gate.attack: the pages show 42")));
        const QRegularExpression drift(QStringLiteral("display mirror.*gate\\.attack"));
        QTest::ignoreMessage(QtWarningMsg, drift);
        // The ignore takes the first; a second would fail the test, so once is what is checked.
        QTest::failOnWarning(drift);
        binding()->checkEvery(50);
        QTest::qWait(300);  // several checks, one message
        view().gate.setAttackMs(float(held(QStringLiteral("gate.attack"))));
        QCOMPARE(binding()->firstMismatch(), QString());
    }
};

QTEST_MAIN(TestAetherGui)
#include "test_aether_gui.moc"
