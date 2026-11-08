// The vendored AetherRX pages against our shims alone: no engine, no radio, no window
// of ours. This is where a sync that breaks the pages, the theme or the fonts shows up.
// Set ESA_GUI_SHOT_DIR to also save a PNG of each page, for comparing by eye.

#include "dsp/StageSet.h"
#include "gui/aether/Appearance.h"

#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ThemeManager.h"

#include "NrGainStrip.h"
#include "StagePage.h"
#include "StripCompPanel.h"
#include "StripEqPanel.h"
#include "StripGatePanel.h"
#include "StripPuduPanel.h"
#include "StripRxOutputPanel.h"
#include "StripTubePanel.h"
#include "StripWaveformPanel.h"
#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStyle>
#include <QVBoxLayout>
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

} // namespace

class TestAetherGui : public QObject {
    Q_OBJECT

private:
    // The pages draw from the same five stages the window's display mirror holds
    // (LiveTaps::view), set up as a fresh chain sets up its own.
    esa::StageSet m_stages;
    std::unique_ptr<AudioEngine> m_engine;

    // Hosts `page` the way the window will, binds it once it has its parent (as AetherRX
    // does), lets its timers and layouts settle, checks it drew, and saves it when asked.
    void render(QWidget* page, const QString& name, const std::function<void()>& bind)
    {
        Ground ground(page);
        bind();
        ground.show();
        QVERIFY(QTest::qWaitForWindowExposed(&ground));
        QTest::qWait(150);  // several panel ticks, so meters and curves have drawn

        const QImage shot = ground.grab().toImage();
        QCOMPARE(shot.size() / shot.devicePixelRatio(), kPageSize);
        const double painted = paintedFraction(shot);
        QVERIFY2(painted > 0.01, qPrintable(QStringLiteral("%1 drew %2% of its page")
                                                .arg(name)
                                                .arg(painted * 100.0, 0, 'f', 2)));

        const QString shots = qEnvironmentVariable("ESA_GUI_SHOT_DIR");
        if (!shots.isEmpty()) {
            QDir().mkpath(shots);
            QVERIFY(shot.save(QDir(shots).filePath(QStringLiteral("aether-%1.png").arg(name))));
        }
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
        render(page, name, bind);
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
        render(page, QStringLiteral("final-output"), [output, waveform] {
            output->showForRx();
            waveform->showForRx();
            tidyEmbeddedPanel(output, /*keepTitle=*/true);
            tidyEmbeddedPanel(waveform, /*keepTitle=*/true);
        });
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
};

QTEST_MAIN(TestAetherGui)
#include "test_aether_gui.moc"
