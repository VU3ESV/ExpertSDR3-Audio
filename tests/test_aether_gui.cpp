// The sync-safety contract for AetherSDR's vendored AetherRX pages.
//
// First the pages against our shims alone: no engine, no radio, no window of ours. This is
// where a sync that breaks the pages, the theme or the fonts shows up. Then the real window
// against a real engine that never connects, on this thread: every object the window looks
// up, every key a page edits, every correction made from outside. A sync that renames an
// object, adds a forced write or changes a behaviour we correct fails here.
// Set ESA_GUI_SHOT_DIR to also save a PNG of each page, for comparing by eye.

#include "MockExpert.h"

#include "app/AppInfo.h"
#include "app/Engine.h"
#include "app/LiveTaps.h"
#include "app/Profiles.h"
#include "app/Settings.h"
#include "audio/OutputShape.h"
#include "audio/PlaybackBuffer.h"
#include "dsp/RxChain.h"
#include "dsp/StageSet.h"
#include "gui/DetailsWindow.h"
#include "gui/MainWindow.h"
#include "gui/ParamWidgets.h"
#include "gui/aether/Appearance.h"
#include "gui/aether/Guards.h"
#include "gui/binding/EqFeed.h"
#include "gui/binding/OutputFeed.h"
#include "gui/binding/StageBinding.h"
#include "gui/pages/NrPage.h"
#include "gui/pages/OutputPage.h"
#include "gui/pages/RadioPage.h"
#include "gui/pages/StageHost.h"
#include "gui/pages/StationBand.h"
#include "gui/shell/HeaderBar.h"
#include "gui/shell/StatusChip.h"
#include "gui/shell/WindowChrome.h"

#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ThemeManager.h"

#include "ClientCompKnob.h"
#include "ClientCompLimiterButton.h"
#include "ClientCompMeter.h"
#include "ClientCompThresholdFader.h"
#include "ClientEqEditorCanvas.h"
#include "ClientEqIconRow.h"
#include "ClientEqOutputFader.h"
#include "ClientEqParamRow.h"
#include "ClientGateCurveWidget.h"
#include "ClientGateLevelView.h"
#include "EditorFramelessTitleBar.h"
#include "NrGainStrip.h"
#include "PanelTick.h"
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
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QDoubleSpinBox>
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
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTemporaryDir>
#include <QTextDocumentFragment>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>
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

// An output that plays nothing, for a test that changes the output device: whatever the
// window then asks for, no device opens.
class SilentSink final : public esa::AudioSink {
public:
    QString deviceName() const override { return QStringLiteral("silent"); }
    void write(const float*, int) override {}
    bool alive() const override { return true; }
    esa::SinkStats stats() override { return {}; }
    void setGainDb(double) override {}
    void setBoost(bool) override {}
    void setMuted(bool) override {}
    void setTargetLatencyMs(int) override {}
};

esa::Engine::SinkFactory silentSinks()
{
    return [](const esa::AppConfig&, QString*) -> std::unique_ptr<esa::AudioSink> {
        return std::make_unique<SilentSink>();
    };
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

// Types `text` into a knob's value field and presses Return, as the operator would: into
// the field once it has the keyboard, which is when it shows the bare number to type over.
void type(AetherSDR::ClientCompKnob* knob, const QString& text)
{
    auto* field = knob->findChild<QLineEdit*>();
    QVERIFY(field);
    knob->window()->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(knob->window()));
    field->setFocus(Qt::MouseFocusReason);
    QTRY_VERIFY(field->hasFocus());
    field->selectAll();
    QTest::keyClicks(field, text);
    QTest::keyClick(field, Qt::Key_Return);
}

void type(AetherSDR::ClientCompKnob* knob, double value)
{
    type(knob, QString::number(value, 'g', 10));
}

// The NR GAIN trace's pixels in columns [x0, x1) of the strip: green while a method runs,
// where its idle line, bed and border are grey and blue. `upperHalf` counts only those in
// the strip's upper half, where a gain near 1 is drawn; `bright` only the bright green of
// speech, not the dim green of the floor between syllables.
int tracePixels(QWidget* strip, int x0, int x1, bool upperHalf = false, bool bright = false)
{
    const QImage image = strip->grab().toImage().convertToFormat(QImage::Format_RGB32);
    const qreal dpr = image.devicePixelRatio();
    const int left = std::max(0, int(x0 * dpr));
    const int right = std::min(image.width(), int(x1 * dpr));
    const int bottom = upperHalf ? image.height() / 2 : image.height();
    int traced = 0;
    for (int y = 0; y < bottom; ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = left; x < right; ++x) {
            const QRgb c = line[x];
            // The floor's thin line falls between two rows, each at half strength.
            const bool green = qGreen(c) > qRed(c) + 25 && qGreen(c) > qBlue(c) + 10;
            if (green && (!bright || qGreen(c) > 150)) ++traced;
        }
    }
    return traced;
}

// The whole trace, leaving out the sweep cursor in the last few columns, which is drawn
// while a method runs whatever its gain.
int tracedPixels(QWidget* strip)
{
    return tracePixels(strip, 0, strip->width() - 4);
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

// The compressor's Out meter: the one of its two meters that carries the makeup handle.
ClientCompMeter* makeupMeter(QWidget* panel)
{
    for (ClientCompMeter* meter : panel->findChildren<ClientCompMeter*>()) {
        if (meter->mode() == ClientCompMeter::Mode::Level && meter->makeupControlEnabled()) return meter;
    }
    return nullptr;
}

// A panel's button by the text it shows, for those upstream gives no name.
QPushButton* buttonLabelled(QWidget* panel, const QString& text)
{
    for (QPushButton* button : panel->findChildren<QPushButton*>()) {
        if (button->text() == text) return button;
    }
    return nullptr;
}

// One mouse event at `at`, straight to `w`, as a hand on it sends it.
void mouse(QWidget* w, QEvent::Type type, QPoint at, Qt::MouseButton button = Qt::LeftButton)
{
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent event(type, QPointF(at), w->mapToGlobal(QPointF(at)), type == QEvent::MouseMove ? Qt::NoButton : button,
                      held, Qt::NoModifier);
    QApplication::sendEvent(w, &event);
}

// Where the EQ's curve draws a band's handle, by its own mapping: 20 Hz to 20 kHz on a log
// scale across, +18 dB to -18 dB down to the band plan, a high- or low-pass on 0 dB.
QPointF eqHandle(const ClientEqEditorCanvas* canvas, const ClientEq& eq, int band)
{
    const ClientEq::BandParams p = eq.band(band);
    const bool slope = p.type == ClientEq::FilterType::HighPass || p.type == ClientEq::FilterType::LowPass;
    const double plot = canvas->height() - ClientEqCurveWidget::kAudioBandStripPx;
    const double x = canvas->width() * std::log10(p.freqHz / 20.0) / 3.0;
    const double y = std::clamp((18.0 - (slope ? 0.0 : p.gainDb)) / 36.0 * plot, 0.0, plot);
    return {x, y};
}

// What the curve reads a point as: the inverse of the mapping above, before it rounds.
double eqFrequencyAt(const ClientEqEditorCanvas* canvas, double x)
{
    return 20.0 * std::pow(10.0, 3.0 * std::clamp(x / canvas->width(), 0.0, 1.0));
}
double eqGainAt(const ClientEqEditorCanvas* canvas, double y)
{
    const double plot = canvas->height() - ClientEqCurveWidget::kAudioBandStripPx;
    return std::clamp(18.0 - 36.0 * std::clamp(y / plot, 0.0, 1.0), -18.0, 18.0);
}

// One mouse event to `w`, as a hand on the curve sends it.
void mouse(QWidget* w, QEvent::Type type, const QPointF& at, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, at, w->mapToGlobal(at), button, held, modifiers);
    QApplication::sendEvent(w, &event);
}

// Chooses `path` in the popup menu about to open, a submenu at a time, with the keyboard:
// QMenu::exec() returns only what the menu itself activates.
void chooseInNextMenu(const QStringList& path, int depth = 0, int tries = 0)
{
    QTimer::singleShot(10, qApp, [path, depth, tries] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QAction* found = nullptr;
        if (menu) {
            for (QAction* action : menu->actions()) {
                if (action->isEnabled() && action->text().startsWith(path.at(depth))) found = action;
            }
        }
        if (!found) {
            if (tries < 200) {
                chooseInNextMenu(path, depth, tries + 1);
            } else {
                // Never left open: the test fails on what was not chosen instead of hanging.
                while (QWidget* popup = QApplication::activePopupWidget()) popup->close();
            }
            return;
        }
        menu->setActiveAction(found);
        if (depth + 1 < path.size()) {
            QTest::keyClick(menu, Qt::Key_Right);
            chooseInNextMenu(path, depth + 1);
        } else {
            QTest::keyClick(menu, Qt::Key_Return);
        }
    });
}

// The canvas counts what its analyser was handed, for upstream's own diagnostics.
quint64 analyserUpdates(ClientEqEditorCanvas* canvas)
{
    return canvas->eqstatsSnapshot(false).value(QStringLiteral("fftUpdateCount")).toULongLong();
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

    void make(esa::AppConfig config = {}, esa::Engine::SinkFactory sinks = {})
    {
        config.deviceKey = QStringLiteral("null");  // never the speakers
        config.host = QStringLiteral("192.0.2.1");  // TEST-NET: nothing is there
        config.fallbackPorts.clear();
        config.muteExpert = false;
        const QString path = m_dir.filePath(QStringLiteral("aether.ini"));
        m_window.reset();
        if (m_core) m_core->shutdown();
        m_core = std::make_unique<esa::Engine>(path, std::move(sinks));
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

    // The EQ page's panel and the parts of it the tests drive.
    StripEqPanel* eqPanel() const
    {
        esa::StageHost* eq = host(QStringLiteral("eq"));
        return eq ? qobject_cast<StripEqPanel*>(eq->panel()) : nullptr;
    }
    ClientEqEditorCanvas* eqCanvas() const
    {
        StripEqPanel* panel = eqPanel();
        return panel ? panel->findChild<ClientEqEditorCanvas*>(QStringLiteral("stripRxEqCanvas")) : nullptr;
    }
    // The combo for the high- and low-pass filter family, the one that offers Butterworth.
    QComboBox* eqFamilyCombo() const
    {
        for (QComboBox* combo : eqPanel()->findChildren<QComboBox*>()) {
            if (combo->findText(QStringLiteral("Butterworth")) >= 0) return combo;
        }
        return nullptr;
    }
    // One of a band's three fields under the curve: 0 its frequency, 1 its gain, 2 its Q. The
    // row builds them anew whenever a band changes, so they are looked up each time.
    QLineEdit* bandField(int band, int which) const
    {
        auto* row = eqPanel()->findChild<ClientEqParamRow*>();
        QLayoutItem* item = row ? row->layout()->itemAt(band) : nullptr;
        return item && item->widget() ? item->widget()->findChildren<QLineEdit*>().value(which) : nullptr;
    }

    QCheckBox* railBox(const QString& slug) const
    {
        return m_window->findChild<QCheckBox*>(QStringLiteral("aetherRxEnable") + slug);
    }

    esa::NrPage* nrPage() const { return m_window->findChild<esa::NrPage*>(); }
    QPushButton* nrTab(const QString& id) const
    {
        return m_window->findChild<QPushButton*>(QStringLiteral("nrTab_") + id);
    }
    // The AetherNR tabs lit, by method id.
    QStringList litNrTabs() const
    {
        QStringList lit;
        for (const QString& id : esa::NrPage::tabIds()) {
            if (QPushButton* tab = nrTab(id); tab && tab->isChecked()) lit << id;
        }
        return lit;
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

    // ── Final Output ──────────────────────────────────────────────────────────

    // The window's last page is AetherRX's: the output panel over the waveform, each under
    // its title plate, with this computer's output between them and no meter of today's left.
    void theFinalOutputPageHoldsBothPanelsAndThisComputersOutput()
    {
        make();
        m_window->showPage(QStringLiteral("output"));
        QCoreApplication::processEvents();
        auto* page = m_window->findChild<esa::OutputPage*>();
        QVERIFY(page && page->isVisible());
        QCOMPARE(page->objectName(), QStringLiteral("page_output"));
        QCOMPARE(page->outputPanel()->findChildren<ClientCompKnob*>().size(), 1);
        QCOMPARE(page->findChildren<WaveformWidget*>().size(), 1);
        for (QWidget* panel : {static_cast<QWidget*>(page->outputPanel()), static_cast<QWidget*>(page->waveformPanel())}) {
            EditorFramelessTitleBar* plate = titlePlate(panel);
            QVERIFY(plate && plate->isVisible());
        }

        // Top to bottom: the output panel, the band, the waveform, all of it on the page.
        const auto rectOn = [page](QWidget* w) { return QRect(w->mapTo(page, QPoint()), w->size()); };
        const QRect output = rectOn(page->outputPanel());
        const QRect band = rectOn(page->stationBand());
        const QRect waveform = rectOn(page->waveformPanel());
        QVERIFY(output.bottom() < band.top());
        QVERIFY(band.bottom() < waveform.top());
        auto* scope = page->findChild<WaveformWidget*>();
        QVERIFY(page->rect().contains(rectOn(scope)));
        for (const char* name : {"outputDevice", "outputRefresh", "outputBuffer", "outputStats"}) {
            auto* w = page->stationBand()->findChild<QWidget*>(QLatin1String(name));
            QVERIFY2(w && w->isVisible(), name);
        }

        // The meters are AetherSDR's now: no progress bar is left anywhere in the window.
        QVERIFY(m_window->findChildren<QProgressBar*>().isEmpty());
        // The scope promises nothing this app does not have, and runs at upstream's rate, but
        // for less on Linux on ARM (the Pi). An Apple Silicon Mac keeps upstream's.
        QVERIFY2(!scope->toolTip().contains(QLatin1String("WAVE")), qPrintable(scope->toolTip()));
#if defined(Q_PROCESSOR_ARM) && defined(Q_OS_LINUX)
        QCOMPARE(scope->refreshRateHz(), 30);
#else
        QCOMPARE(scope->refreshRateHz(), kPanelTickHz);
#endif
    }

    // At the window's narrowest the page is wider than its column, and MUTE, BOOST and Refresh
    // are cut off at the right. They are still in reach: the page scrolls across, on a bar
    // with a handle to see and take, as the bar down is.
    void finalOutputScrollsToWhatANarrowWindowCutsOff()
    {
        make();
        m_window->showPage(QStringLiteral("output"));
        auto* page = m_window->findChild<esa::OutputPage*>();
        QVERIFY(page);
        m_window->resize(1, 700);  // as narrow as it goes
        QTRY_VERIFY(m_window->width() < 700);
        QTest::qWait(50);
        auto* scroller = page->findChild<QScrollArea*>();
        QVERIFY(scroller);
        QPushButton* mute = buttonLabelled(page->outputPanel(), QStringLiteral("MUTE"));
        QVERIFY(mute);
        QWidget* viewport = scroller->viewport();
        const auto inView = [&](QWidget* w) {
            return viewport->rect().contains(QRect(w->mapTo(viewport, QPoint()), w->size()));
        };
        QScrollBar* across = scroller->horizontalScrollBar();
        if (!across->isVisible()) {
            // Fonts that make the page narrower here: then it fits, and nothing is cut.
            QVERIFY(inView(mute));
            return;
        }
        qInfo("window %d px wide: the page scrolls by %d px", m_window->width(), across->maximum());

        // The handle is drawn, filled in the canon's line colour across its middle row.
        const QImage bar = across->grab().toImage().convertToFormat(QImage::Format_RGB32);
        const QColor lineHi = ThemeManager::instance().color(across, QStringLiteral("color.canon.lineHi"));
        const auto* middle = reinterpret_cast<const QRgb*>(bar.constScanLine(bar.height() / 2));
        int filled = 0;
        for (int x = 0; x < bar.width(); ++x) {
            if (std::abs(qRed(middle[x]) - lineHi.red()) <= 12 && std::abs(qGreen(middle[x]) - lineHi.green()) <= 12
                && std::abs(qBlue(middle[x]) - lineHi.blue()) <= 12) {
                ++filled;
            }
        }
        QVERIFY2(filled >= 30 * bar.devicePixelRatio(), qPrintable(QStringLiteral("%1 of %2 px").arg(filled).arg(bar.width())));

        // Scrolled to its end, MUTE and BOOST are wholly in view.
        across->setValue(across->maximum());
        QCoreApplication::processEvents();
        QVERIFY(inView(mute));
        QVERIFY(inView(buttonLabelled(page->outputPanel(), QStringLiteral("BOOST"))));
    }

    // The knob upstream makes a ±12 dB Trim is the Volume, with today's range and meaning:
    // -60 to +30 dB, 0 dB at 12 o'clock, kept in tenths of a dB with this computer's settings.
    void theVolumeKnobIsTodaysVolumeWithZeroAtTwelve()
    {
        esa::AppConfig config;
        config.outputGainDb = -60;
        make(config);
        m_window->showPage(QStringLiteral("output"));
        auto* page = m_window->findChild<esa::OutputPage*>();
        QVERIFY(page);
        ClientCompKnob* volume = page->volumeKnob();
        QVERIFY(volume);
        QCOMPARE(volume, page->outputPanel()->findChild<ClientCompKnob*>());
        QCOMPARE(volume->accessibleName(), QStringLiteral("Volume"));
        QCOMPARE(volume->value(), -60.0f);

        // The travel, each half from its own end.
        QCOMPARE(esa::OutputPage::normFromVolume(0.0f), 0.5f);
        QCOMPARE(esa::OutputPage::volumeFromNorm(0.5f), 0.0f);
        QVERIFY(!std::signbit(esa::OutputPage::volumeFromNorm(0.5f)));  // "+0.0 dB", never "-0.0 dB"
        QCOMPARE(esa::OutputPage::volumeFromNorm(0.0f), -60.0f);
        QCOMPARE(esa::OutputPage::volumeFromNorm(0.25f), -30.0f);
        QCOMPARE(esa::OutputPage::volumeFromNorm(0.75f), 15.0f);
        QCOMPARE(esa::OutputPage::volumeFromNorm(1.0f), 30.0f);
        for (float db : {-60.0f, -42.5f, -12.0f, -0.1f, 0.0f, 0.8f, 12.0f, 30.0f}) {
            const float back = esa::OutputPage::volumeFromNorm(esa::OutputPage::normFromVolume(db));
            QVERIFY2(std::abs(back - db) < 1e-4f, qPrintable(QString::number(db)));
        }
        // Set from code, the knob stops at its ends and changes nothing.
        volume->setValue(-100.0f);
        QCOMPARE(volume->value(), -60.0f);
        volume->setValue(100.0f);
        QCOMPARE(volume->value(), 30.0f);
        volume->setValue(-60.0f);
        QCOMPARE(m_window->config().outputGainDb, -60.0);

        // By hand: half the knob's 200 px of travel up from the bottom is 0 dB.
        drag(volume, -100);
        QCOMPARE(volume->value(), 0.0f);
        QCOMPARE(m_window->config().outputGainDb, 0.0);
        QCOMPARE(volume->findChild<QLineEdit*>()->text(), QStringLiteral("+0.0 dB"));
        drag(volume, -50);
        QCOMPARE(volume->value(), 15.0f);
        QCOMPARE(m_window->config().outputGainDb, 15.0);
        // A drag lands between tenths; what is kept is the tenth the knob shows.
        volume->setValue(-60.0f);
        drag(volume, -33);
        QCOMPARE(m_window->config().outputGainDb, -40.2);
        // A notch of the wheel is 1 % of the travel: 1.2 dB below 0 dB.
        wheel(volume, 120);
        QCOMPARE(m_window->config().outputGainDb, -39.0);

        type(volume, -12);
        QCOMPARE(volume->value(), -12.0f);
        QCOMPARE(m_window->config().outputGainDb, -12.0);
        QTest::mouseDClick(volume, Qt::LeftButton, Qt::NoModifier, QPoint(38, 30));
        QCOMPARE(volume->value(), 0.0f);
        QCOMPARE(m_window->config().outputGainDb, 0.0);
        type(volume, 6.5);
        QCOMPARE(m_window->config().outputGainDb, 6.5);
        // Not part of the chain: nothing shows as Modified.
        QVERIFY(!m_window->chainModified());

        // Kept with this computer's settings when the window closes.
        m_window->close();
        QCOMPARE(esa::Settings(m_dir.filePath(QStringLiteral("aether.ini"))).load().outputGainDb, 6.5);
    }

    // A profile holds the sound, not this computer's volume: saving one leaves Volume out, and
    // loading one leaves Volume alone.
    void volumeStaysOutOfProfiles()
    {
        make();
        m_window->showPage(QStringLiteral("output"));
        ClientCompKnob* volume = m_window->findChild<esa::OutputPage*>()->volumeKnob();
        QVERIFY(volume);
        type(volume, -20);
        QVERIFY(m_window->saveProfileAs(QStringLiteral("Quiet")));
        type(volume, 4);
        QVERIFY2(!m_window->chainModified(), "Volume is not a change to the profile");
        m_window->loadProfile(QStringLiteral("Quiet"));
        QCOMPARE(m_window->config().outputGainDb, 4.0);
        QCOMPARE(volume->value(), 4.0f);
        m_window->loadProfile(QString());  // the factory settings
        QCOMPARE(m_window->config().outputGainDb, 4.0);
        QCOMPARE(volume->value(), 4.0f);
    }

    // "nan" and "inf" read as numbers, to Qt as to the knobs, and a NaN passes every clamp. Typed
    // into Volume it reached the speakers, stayed in the output's gain ramp until the output
    // was reopened, and was saved for the next launch. A value field refuses them, on Return
    // and on being left, and Volume refuses a NaN from anywhere else as well.
    void aValueFieldRefusesWhatIsNoNumber()
    {
        esa::AppConfig config;
        config.outputGainDb = -3;
        make(config);
        m_window->showPage(QStringLiteral("output"));
        auto* page = m_window->findChild<esa::OutputPage*>();
        QVERIFY(page);
        ClientCompKnob* volume = page->volumeKnob();
        QVERIFY(volume);
        auto* field = volume->findChild<QLineEdit*>();
        QSpinBox* elsewhere = page->stationBand()->bufferBox();
        QVERIFY(field && elsewhere);
        const auto unchanged = [&](const QString& typed) {
            const QString where = QStringLiteral("typed '%1': knob %2, kept %3")
                                      .arg(typed).arg(volume->value()).arg(m_window->config().outputGainDb);
            QVERIFY2(volume->value() == -3.0f && m_window->config().outputGainDb == -3.0, qPrintable(where));
        };
        for (const QString& typed : {QStringLiteral("nan"), QStringLiteral("NaN"), QStringLiteral(" nan "),
                                     QStringLiteral("inf"), QStringLiteral("-inf")}) {
            // With Return: put back as it was, still there to type over.
            type(volume, typed);
            unchanged(typed);
            QVERIFY(field->hasFocus());
            QCOMPARE(field->text(), QStringLiteral("-3.000"));
            // Typed, then left: nothing, and the reading is back.
            field->selectAll();
            QTest::keyClicks(field, typed);
            elsewhere->setFocus(Qt::MouseFocusReason);
            QTRY_VERIFY(elsewhere->hasFocus());
            unchanged(typed);
            QCOMPARE(field->text(), QStringLiteral("-3.0 dB"));
        }
        // From the hook the panel's knob reaches the window by, whatever sent it.
        shim()->setRxOutputTrimDb(std::nanf(""));
        QCOMPARE(m_window->config().outputGainDb, -3.0);
        // Still a knob that turns.
        drag(volume, -20);
        QVERIFY(std::isfinite(volume->value()) && volume->value() > -3.0f);
        const double turned = m_window->config().outputGainDb;
        QVERIFY(std::isfinite(turned) && turned > -3.0);

        // A stage's knob alike: its value, its control, the mirror and the chain all stay.
        m_window->showPage(QStringLiteral("gate"));
        const QString key = QStringLiteral("gate.release");
        ClientCompKnob* release = host(QStringLiteral("gate"))->knob(key);
        QVERIFY(release);
        const double before = held(key);
        type(release, QStringLiteral("nan"));
        QCOMPARE(held(key), before);
        QVERIFY(std::abs(inChain(key) - before) < 1e-3);
        QVERIFY(std::isfinite(release->value()));

        m_window->close();
        QCOMPARE(esa::Settings(m_dir.filePath(QStringLiteral("aether.ini"))).load().outputGainDb, turned);
    }

    // Volume, MUTE, BOOST, the buffer and the device each reach the engine's output, through
    // the one owner of each, and none of them sends ExpertSDR3 anything: MUTE silences this
    // app, and ExpertSDR3's own mute, held here by the mute controller, stays held.
    void theOutputControlsReachTheEngineAndNotTheRadio()
    {
        MockExpert radio;
        QVERIFY(radio.listen());

        // What the engine asks of its output, from a sink that plays nothing.
        struct Asked {
            QStringList devices;  // the device of each output opened, in order
            double gainDb{0.0};
            bool boost{false};
            bool muted{false};
            int bufferMs{0};
        };
        class ProbeSink final : public esa::AudioSink {
        public:
            explicit ProbeSink(std::shared_ptr<Asked> asked) : m_asked(std::move(asked)) {}
            QString deviceName() const override { return QStringLiteral("probe"); }
            void write(const float*, int) override {}
            bool alive() const override { return true; }
            esa::SinkStats stats() override { return {}; }
            void setGainDb(double db) override { m_asked->gainDb = db; }
            void setBoost(bool on) override { m_asked->boost = on; }
            void setMuted(bool muted) override { m_asked->muted = muted; }
            void setTargetLatencyMs(int ms) override { m_asked->bufferMs = ms; }

        private:
            std::shared_ptr<Asked> m_asked;
        };
        const auto asked = std::make_shared<Asked>();
        const QString ini = m_dir.filePath(QStringLiteral("probe.ini"));
        m_core = std::make_unique<esa::Engine>(ini, [asked](const esa::AppConfig& c, QString*) {
            asked->devices << c.deviceKey;
            asked->bufferMs = c.bufferMs;
            // What the old output was told is no answer for the new one: only what the engine
            // tells this one may show here.
            asked->gainDb = std::nan("");
            asked->boost = !asked->boost;
            asked->muted = !asked->muted;
            return std::make_unique<ProbeSink>(asked);
        });
        m_settings = std::make_unique<esa::Settings>(ini);
        m_settings->setRecordingFolder(m_dir.filePath(QStringLiteral("Recordings")));
        m_profiles = std::make_unique<esa::ProfileLibrary>(esa::ProfileLibrary::defaultPath(ini));
        esa::AppConfig config;
        config.deviceKey = QStringLiteral("null");
        config.host = QStringLiteral("127.0.0.1");
        config.port = radio.port();
        config.fallbackPorts.clear();  // never the real radio's ports
        config.muteExpert = true;  // the mute controller holds ExpertSDR3's mute meanwhile
        config.outputGainDb = -3;
        m_window = std::make_unique<esa::MainWindow>(m_core.get(), m_core->taps(), m_settings.get(), m_profiles.get(),
                                                     config);
        m_window->setQuitHandler([] {});
        m_window->setRevealHandler([](const QString&) {});
        m_window->setFolderChooser([](const QString&) { return QString(); });
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        m_window->showPage(QStringLiteral("output"));
        auto* page = m_window->findChild<esa::OutputPage*>();
        QVERIFY(page);

        esa::DiscoveredRadio picked;
        picked.host = QStringLiteral("127.0.0.1");
        picked.port = radio.port();
        m_window->connectToRadio(picked);
        QTRY_VERIFY_WITH_TIMEOUT(radio.audioStartCount() == 1 && !asked->devices.isEmpty(), 6000);
        QCOMPARE(asked->devices.constFirst(), QStringLiteral("null"));
        QCOMPARE(asked->gainDb, -3.0);
        QVERIFY(!asked->boost && !asked->muted);
        QTRY_VERIFY_WITH_TIMEOUT(radio.muted, 6000);
        QTest::qWait(300);  // the link settles
        const qsizetype commandsBefore = radio.commands.size();
        const int unmutesBefore = radio.muteCommandsSent(false);

        type(page->volumeKnob(), -12);
        QTRY_COMPARE(asked->gainDb, -12.0);
        QCOMPARE(m_window->config().outputGainDb, -12.0);

        QPushButton* mute = nullptr;
        QPushButton* boost = nullptr;
        for (QPushButton* b : page->outputPanel()->findChildren<QPushButton*>()) {
            if (b->text() == QLatin1String("MUTE")) mute = b;
            if (b->text() == QLatin1String("BOOST")) boost = b;
        }
        QVERIFY(mute && boost && mute->isCheckable() && boost->isCheckable());
        QVERIFY(!mute->isChecked() && !boost->isChecked());
        QTest::mouseClick(mute, Qt::LeftButton);
        QTRY_VERIFY(asked->muted);
        QVERIFY(shim()->isMuted());
        QTest::mouseClick(boost, Qt::LeftButton);
        QTRY_VERIFY(asked->boost);
        QVERIFY(m_window->config().boost);
        QTest::mouseClick(mute, Qt::LeftButton);
        QTRY_VERIFY(!asked->muted);
        QVERIFY(!shim()->isMuted());

        page->stationBand()->bufferBox()->setValue(140);
        QTRY_COMPARE(asked->bufferMs, 140);
        QCOMPARE(m_window->config().bufferMs, 140);

        // Another device: the engine moves its output there.
        esa::AudioDeviceInfo speakers;
        speakers.key = QStringLiteral("Test|Speakers");
        speakers.name = QStringLiteral("Speakers");
        speakers.hostApi = QStringLiteral("Test");
        page->showDevices({speakers});
        QComboBox* devices = page->stationBand()->deviceList();
        devices->setCurrentIndex(devices->findData(speakers.key));
        QTRY_COMPARE(asked->devices.constLast(), speakers.key);
        QCOMPARE(m_window->config().deviceKey, speakers.key);
        // The new output gets what the old one had.
        QCOMPARE(asked->gainDb, -12.0);
        QVERIFY(asked->boost && !asked->muted);

        QVERIFY2(!m_window->chainModified(), "none of these is part of a profile");
        QTest::qWait(300);
        // Nothing at all: not VOLUME, MUTE, RX_VOLUME or RX_MUTE, nor anything else.
        QVERIFY2(radio.commands.size() == commandsBefore, qPrintable(radio.commands.mid(commandsBefore).join(u' ')));
        QVERIFY(radio.muted);
        QCOMPARE(radio.muteCommandsSent(false), unmutesBefore);
    }

    // The meter and the scope get exactly what the speakers would: each channel through the
    // speaker path's own shape, at the window's Volume and BOOST. MUTE they read before, as
    // upstream's meter does, so under MUTE they still show what would play (design 6.5).
    void theMeterAndScopeShowWhatGoesToTheSpeakersBeforeMute()
    {
        // First against the speaker path itself, held steady: a mono device folds the two
        // channels as the feed does, so its samples are what the meter gets.
        std::vector<float> steady(2 * 4800);
        for (size_t i = 0; i < steady.size(); i += 2) {
            steady[i] = 0.3f;
            steady[i + 1] = -0.7f;
        }
        for (const double db : {-12.0, 0.0, 0.8, 9.0}) {
            for (const bool boost : {false, true}) {
                for (const bool muted : {false, true}) {
                    esa::PlaybackBuffer speakers(48000, 48000, 1);
                    speakers.setGainDb(db);
                    speakers.setBoost(boost);
                    speakers.setMuted(muted);
                    std::vector<float> out(512);
                    // Past the 60 ms it fills before playing, its fade-in and its gain ramp.
                    for (int block = 0; block < 12; ++block) {
                        speakers.write(steady.data(), 512);
                        speakers.render(out.data(), 512);
                    }
                    esa::OutputFeed::Shape shape;
                    shape.volumeDb = db;
                    shape.boost = boost;
                    float meter = 1.0f;
                    esa::OutputFeed::toMono(steady.data(), 1, shape, &meter);
                    // Muted, the speakers are silent and the meter reads what would play.
                    if (muted) {
                        QVERIFY2(out.back() == 0.0f, qPrintable(QString::number(out.back())));
                        esa::PlaybackBuffer unmuted(48000, 48000, 1);
                        unmuted.setGainDb(db);
                        unmuted.setBoost(boost);
                        for (int block = 0; block < 12; ++block) {
                            unmuted.write(steady.data(), 512);
                            unmuted.render(out.data(), 512);
                        }
                    }
                    QVERIFY2(std::abs(out.back() - meter) < 1e-5f,
                             qPrintable(QStringLiteral("%1 dB, boost %2, mute %3: speakers %4, meter %5")
                                            .arg(db).arg(boost).arg(muted).arg(out.back()).arg(meter)));
                }
            }
        }

        // Then in the window: what is handed to the speakers, through the page's own controls.
        esa::AppConfig config;
        config.outputGainDb = -6;
        config.boost = true;
        make(config);
        m_window->showPage(QStringLiteral("output"));
        auto* page = m_window->findChild<esa::OutputPage*>();
        QVERIFY(page && page->feed()->isRunning());
        QVERIFY(m_core->taps()->outWanted.load());
        QSignalSpy toScope(shim(), &AudioEngine::rxPostChainScopeReady);
        QSignalSpy toMeter(shim(), &AudioEngine::scopeSamplesReady);

        std::vector<float> block(2 * 480);
        for (size_t i = 0; i < 480; ++i) {
            block[2 * i] = 0.9f * float(std::sin(double(i) * 0.05));
            block[2 * i + 1] = 0.4f * float(std::cos(double(i) * 0.031));
        }
        // As the engine hands a block over, then one tick of the feed: what the scope was
        // given, and nothing unless the meter was given the same, once each, as RX.
        const int rate = m_core->taps()->chainRate.load();
        const auto handOver = [&] {
            toScope.clear();
            toMeter.clear();
            auto& taps = *m_core->taps();
            std::vector<float> mono;
            if (taps.outTap.write(block.data(), 480) != 480) return mono;
            taps.outBlocks.fetch_add(1);
            page->feed()->tick();
            if (toScope.count() != 1 || toMeter.count() != 1) return mono;
            const QByteArray pcm = toScope.at(0).at(0).toByteArray();
            if (toMeter.at(0).at(0).toByteArray() != pcm || toMeter.at(0).at(2).toBool()) return mono;
            if (toScope.at(0).at(1).toInt() != rate || toMeter.at(0).at(1).toInt() != rate) return mono;
            mono.resize(size_t(pcm.size()) / sizeof(float));
            std::memcpy(mono.data(), pcm.constData(), mono.size() * sizeof(float));
            return mono;
        };
        const auto expected = [&](double db, bool boost) {
            const float gain = esa::outputGain(db);
            std::vector<float> mono(480);
            for (size_t i = 0; i < 480; ++i) {
                const float left = esa::shapeOutput(block[2 * i], gain, boost);
                const float right = esa::shapeOutput(block[2 * i + 1], gain, boost);
                mono[i] = 0.5f * (left + right);
            }
            return mono;
        };

        QVERIFY(handOver() == expected(-6, true));
        type(page->volumeKnob(), 3);
        QVERIFY(handOver() == expected(3, true));
        QPushButton* mute = nullptr;
        QPushButton* boost = nullptr;
        for (QPushButton* b : page->outputPanel()->findChildren<QPushButton*>()) {
            if (b->text() == QLatin1String("MUTE")) mute = b;
            if (b->text() == QLatin1String("BOOST")) boost = b;
        }
        QVERIFY(mute && boost);
        QTest::mouseClick(boost, Qt::LeftButton);
        QVERIFY(handOver() == expected(3, false));
        // MUTE silences the speakers and leaves the meter and scope reading what would play.
        QTest::mouseClick(mute, Qt::LeftButton);
        QVERIFY(shim()->isMuted());
        const std::vector<float> muted = handOver();
        QVERIFY(muted == expected(3, false));
        QVERIFY(std::any_of(muted.begin(), muted.end(), [](float v) { return v != 0.0f; }));
        QTest::mouseClick(mute, Qt::LeftButton);
        QVERIFY(!shim()->isMuted());
        QVERIFY(handOver() == expected(3, false));

        // Audio reaching the output is not silence, whatever MUTE does to it.
        QVERIFY(!page->feed()->feedingSilence());
        QVERIFY(!page->scopeNote()->isVisible());
        // Off screen, the engine is told not to bother.
        m_window->showPage(QStringLiteral("gate"));
        QVERIFY(!page->feed()->isRunning());
        QVERIFY(!m_core->taps()->outWanted.load());
    }

    // With nothing reaching the output the meter and scope are fed silence at the real rate,
    // so the scope draws a flat line and never upstream's "Enable PC Audio", and a note over
    // it says why: no radio, or a radio sending nothing.
    // At both chain rates: at 48 kHz alone a feed that never read the chain's rate would pass,
    // and at 24 kHz alone a scope never told the rate would, showing its own default of 24 kHz.
    void nothingFlowingFeedsSilenceAndSaysWhy_data()
    {
        QTest::addColumn<int>("chainRate");
        QTest::newRow("48 kHz") << 48000;
        QTest::newRow("24 kHz") << 24000;
    }

    void nothingFlowingFeedsSilenceAndSaysWhy()
    {
        QFETCH(int, chainRate);
        esa::AppConfig config;
        config.chainRate = chainRate;
        make(config);
        QSignalSpy toScope(shim(), &AudioEngine::rxPostChainScopeReady);
        m_window->showPage(QStringLiteral("output"));
        auto* page = m_window->findChild<esa::OutputPage*>();
        QVERIFY(page);
        auto* scope = page->findChild<WaveformWidget*>();
        QVERIFY(scope && scope->isVisible());
        QCOMPARE(m_core->taps()->chainRate.load(), chainRate);

        QTRY_VERIFY_WITH_TIMEOUT(toScope.count() >= 10, 3000);  // 25 a second
        for (const QList<QVariant>& piece : std::as_const(toScope)) {
            const QByteArray pcm = piece.at(0).toByteArray();
            QCOMPARE(piece.at(1).toInt(), chainRate);
            QCOMPARE(pcm.size(), qsizetype(chainRate / 25 * sizeof(float)));  // 40 ms
            std::vector<float> samples(size_t(pcm.size()) / sizeof(float));
            std::memcpy(samples.data(), pcm.constData(), size_t(pcm.size()));
            QVERIFY(std::all_of(samples.begin(), samples.end(), [](float v) { return v == 0.0f; }));
        }
        QVERIFY(page->feed()->feedingSilence());
        // The footer gives the real rate.
        QCOMPARE(scope->activeSampleRate(), chainRate);

        // The scope says "Enable PC Audio" once its newest samples are a second old. Fed
        // silence, something reaches it in every half second, so they never are.
        scope->resetPerfStats();
        quint64 appends = 0;
        for (int i = 0; i < 4; ++i) {
            QTest::qWait(500);
            const quint64 now = scope->perfStats().appendCount;
            QVERIFY2(now > appends, qPrintable(QStringLiteral("nothing reached the scope in half a second, at %1").arg(i)));
            appends = now;
        }

        // The note, where upstream writes its own message, says why.
        QLabel* note = page->scopeNote();
        QVERIFY(note && note->isVisible());
        QCOMPARE(note->text(), QStringLiteral("Not connected"));
        QCOMPARE(note->parentWidget(), static_cast<QWidget*>(scope));
        QVERIFY(scope->rect().contains(note->geometry()));
        const QPoint plotCentre = scope->rect().adjusted(5, 18, -34, -17).center();
        QVERIFY(std::abs(note->geometry().center().x() - plotCentre.x()) <= 1);
        QVERIFY(std::abs(note->geometry().center().y() - plotCentre.y()) <= 1);
        // Drawn: its text is what the scope shows there, and without it the scope shows less.
        const QRect at(note->mapTo(m_window.get(), QPoint()), note->size());
        const auto grabNote = [&] {
            const QImage shot = m_window->grab().toImage();
            const qreal dpr = shot.devicePixelRatio();
            return shot.copy(QRect(at.topLeft() * dpr, at.size() * dpr)).convertToFormat(QImage::Format_RGB32);
        };
        const QImage withNote = grabNote();
        note->hide();
        const QImage without = grabNote();
        note->show();
        int differing = 0;
        for (int y = 0; y < withNote.height(); ++y) {
            for (int x = 0; x < withNote.width(); ++x) {
                const QRgb a = withNote.pixel(x, y);
                const QRgb b = without.pixel(x, y);
                if (std::abs(qGray(a) - qGray(b)) > 40) ++differing;
            }
        }
        QVERIFY2(differing > 30, qPrintable(QString::number(differing)));

        esa::EngineStatus status;
        status.connection = esa::TciClient::State::Ready;
        status.audioFlowing = false;
        page->showStatus(status);
        QCOMPARE(note->text(), QStringLiteral("No audio from radio"));
        QVERIFY(note->isVisible());
        status.connection = esa::TciClient::State::Connecting;
        page->showStatus(status);
        QCOMPARE(note->text(), QStringLiteral("Not connected"));

        // Audio reaching the output again: the note goes, and the feed passes it on.
        std::vector<float> block(2 * 480, 0.25f);
        auto& taps = *m_core->taps();
        taps.outTap.write(block.data(), 480);
        taps.outBlocks.fetch_add(1);
        page->feed()->tick();
        QVERIFY(!page->feed()->feedingSilence());
        QVERIFY(!note->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(page->feed()->feedingSilence(), 2000);  // and then nothing again
        QVERIFY(note->isVisible());
    }

    // The output device, Refresh and the buffer work as they always have: "System default"
    // first, the system's own device marked, a missing one kept, and each choice kept.
    void theDeviceListRefreshAndBufferWorkAsBefore()
    {
        esa::AppConfig config;
        config.bufferMs = 80;
        // It picks other devices, System default among them: none of them may ever open.
        make(config, silentSinks());
        m_window->showPage(QStringLiteral("output"));
        auto* page = m_window->findChild<esa::OutputPage*>();
        QVERIFY(page);
        esa::StationBand* band = page->stationBand();
        QComboBox* devices = band->deviceList();
        auto* note = band->findChild<QLabel*>(QStringLiteral("outputDevicesNote"));
        QVERIFY(devices && note);

        esa::AudioDeviceInfo built;
        built.key = QStringLiteral("Test|Built-in");
        built.name = QStringLiteral("Built-in");
        built.hostApi = QStringLiteral("Test");
        built.isDefault = true;
        esa::AudioDeviceInfo usb = built;
        usb.key = QStringLiteral("Test|USB");
        usb.name = QStringLiteral("USB");
        usb.isDefault = false;

        // The window's device ("null" here) is not among them: kept, and still chosen.
        page->showDevices({built, usb});
        QCOMPARE(devices->count(), 4);
        QCOMPARE(devices->itemText(0), QStringLiteral("System default"));
        QCOMPARE(devices->itemData(0).toString(), QString());
        QCOMPARE(devices->itemText(1), QStringLiteral("Built-in  (Test)  *"));
        QCOMPARE(devices->itemText(2), QStringLiteral("USB  (Test)"));
        QCOMPARE(devices->itemText(3), QStringLiteral("No output (null device)"));
        QCOMPARE(devices->currentIndex(), 3);
        QCOMPARE(m_window->config().deviceKey, QStringLiteral("null"));
        QVERIFY(note->isHidden());

        devices->setCurrentIndex(2);
        QCOMPARE(m_window->config().deviceKey, usb.key);
        page->showDevices({built, usb});  // a refresh keeps the choice
        QCOMPARE(devices->count(), 3);
        QCOMPARE(devices->currentIndex(), 2);
        page->showDevices({built});       // unplugged: kept by name
        QCOMPARE(devices->currentText(), QStringLiteral("USB (not found)"));
        QCOMPARE(m_window->config().deviceKey, usb.key);
        devices->setCurrentIndex(0);
        QCOMPARE(m_window->config().deviceKey, QString());
        page->showDevices({});
        QCOMPARE(devices->count(), 1);
        QVERIFY(note->isVisibleTo(band));

        // Refresh asks the engine to look again, and the list follows what it finds.
        QSignalSpy found(m_core.get(), &esa::Engine::devicesChanged);
        QTest::mouseClick(band->refreshButton(), Qt::LeftButton);
        QTRY_COMPARE(found.count(), 1);
        const auto listed = found.at(0).at(0).value<QList<esa::AudioDeviceInfo>>();
        QTRY_COMPARE(devices->count(), int(listed.size()) + 1);
        QCOMPARE(devices->itemText(0), QStringLiteral("System default"));
        QCOMPARE(note->isVisibleTo(band), listed.isEmpty());

        QSpinBox* buffer = band->bufferBox();
        QCOMPARE(buffer->value(), 80);
        QCOMPARE(buffer->minimum(), 10);
        QCOMPARE(buffer->maximum(), 500);
        buffer->setValue(250);
        QCOMPARE(m_window->config().bufferMs, 250);

        // What the open output reports, beside the buffer; why none is open, below it.
        esa::EngineStatus status;
        status.outputOpen = true;
        status.sink.bufferedMs = 61.4;
        status.sink.deviceRate = 44100;
        status.chainRate = 24000;
        status.dspLoad = 0.123;
        status.sink.underruns = 2;
        status.droppedBlocks = 5;
        page->showStatus(status);
        auto* buffered = band->findChild<QLabel*>(QStringLiteral("outputBuffered"));
        auto* error = band->findChild<QLabel*>(QStringLiteral("outputStatus"));
        auto* stats = band->findChild<QLabel*>(QStringLiteral("outputStats"));
        QVERIFY(buffered && error && stats);
        QVERIFY(buffered->text().contains(QLatin1String("61 ms buffered, 44100 Hz")));
        QVERIFY(error->isHidden());
        const QString figures = QTextDocumentFragment::fromHtml(stats->text()).toPlainText().replace(QChar(0x00A0), u' ');
        for (const char* figure : {"DSP 12.3%", "underruns 2", "lost blocks 5", "chain 24000 Hz", "ppm"}) {
            QVERIFY2(figures.contains(QLatin1String(figure)), qPrintable(figures));
        }
        status.outputOpen = false;
        status.outputError = QStringLiteral("Device unplugged");
        page->showStatus(status);
        QVERIFY(buffered->text().isEmpty());
        QVERIFY(error->isVisibleTo(band));
        QVERIFY(error->text().contains(QLatin1String("Device unplugged")));
    }

    // The null device (--device null) discards audio and is never listed. Chosen, it is named
    // for what it is, not shown as a device without a name that is not plugged in. A real
    // device that is not plugged in is still kept by its name, as before.
    void theNullDeviceIsNamedForWhatItIs()
    {
        make({}, silentSinks());  // the null device, as every test window has; nothing else opens
        auto* page = m_window->findChild<esa::OutputPage*>();
        QVERIFY(page);
        QComboBox* devices = page->stationBand()->deviceList();
        esa::AudioDeviceInfo built;
        built.key = QStringLiteral("Test|Built-in");
        built.name = QStringLiteral("Built-in");
        built.hostApi = QStringLiteral("Test");
        built.isDefault = true;
        esa::AudioDeviceInfo usb = built;
        usb.key = QStringLiteral("Test|USB Codec");
        usb.name = QStringLiteral("USB Codec");
        usb.isDefault = false;

        for (const QList<esa::AudioDeviceInfo>& found : {QList<esa::AudioDeviceInfo>{built, usb},
                                                         QList<esa::AudioDeviceInfo>{}}) {
            page->showDevices(found);
            QCOMPARE(devices->count(), int(found.size()) + 2);
            QCOMPARE(devices->currentText(), QStringLiteral("No output (null device)"));
            QCOMPARE(devices->currentData().toString(), QStringLiteral("null"));
            QCOMPARE(m_window->config().deviceKey, QStringLiteral("null"));
        }

        page->showDevices({built, usb});
        devices->setCurrentIndex(devices->findData(usb.key));
        QCOMPARE(m_window->config().deviceKey, usb.key);
        page->showDevices({built});  // unplugged
        QCOMPARE(devices->count(), 3);
        QCOMPARE(devices->currentText(), QStringLiteral("USB Codec (not found)"));
        QCOMPARE(devices->currentData().toString(), usb.key);
        QCOMPARE(m_window->config().deviceKey, usb.key);
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

    // Forked for Qt 6.4. Here on its own; the AetherNR page's tests drive it in the window.
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

        // The other stage pages, by what the window and its corrections look up on each.
        QWidget* eq = host(QStringLiteral("eq"))->panel();
        QVERIFY(qobject_cast<StripEqPanel*>(eq));
        QVERIFY(eq->findChild<ClientEqEditorCanvas*>(QStringLiteral("stripRxEqCanvas")));
        QWidget* comp = host(QStringLiteral("comp"))->panel();
        QVERIFY(qobject_cast<StripCompPanel*>(comp));
        QVERIFY(comp->findChild<QSlider*>(QStringLiteral("compCeilingSlider")));
        QVERIFY(makeupMeter(comp));  // in Level mode, with the makeup handle
        QVERIFY(qobject_cast<StripTubePanel*>(host(QStringLiteral("tube"))->panel()));
        QVERIFY(qobject_cast<StripPuduPanel*>(host(QStringLiteral("pudu"))->panel()));
        QCOMPARE(m_window->findChildren<PooDooLogo*>().size(), 1);

        // Final Output: one knob on the output panel, one scope, and both title plates kept.
        auto* output = m_window->findChild<esa::OutputPage*>();
        QVERIFY(output);
        QCOMPARE(output->objectName(), QStringLiteral("page_output"));
        QCOMPARE(m_window->findChildren<StripRxOutputPanel*>().size(), 1);
        QCOMPARE(output->outputPanel()->findChildren<ClientCompKnob*>().size(), 1);
        QCOMPARE(m_window->findChildren<StripWaveformPanel*>().size(), 1);
        QCOMPARE(m_window->findChildren<WaveformWidget*>().size(), 1);
        m_window->showPage(QStringLiteral("output"));
        for (QWidget* plated : {static_cast<QWidget*>(output->outputPanel()), static_cast<QWidget*>(output->waveformPanel())}) {
            EditorFramelessTitleBar* plate = titlePlate(plated);
            QVERIFY(plate && plate->isVisible());
        }

        // AetherNR: a tab for every method, the available ones or not, and the gain strip.
        auto* nr = m_window->findChild<esa::NrPage*>();
        QVERIFY(nr);
        QCOMPARE(nr->objectName(), QStringLiteral("page_nr"));
        for (const char* id : {"nr2", "nr4", "mnr", "dfnr", "rn2", "bnr", "nnr"}) {
            auto* tab = nr->findChild<QPushButton*>(QStringLiteral("nrTab_") + QLatin1String(id));
            QVERIFY2(tab && tab->isCheckable(), id);
        }
        QCOMPARE(nr->findChildren<NrGainStrip*>().size(), 1);
    }

    // Every row of the rail opens its own page, none of them today's grid any more, and the
    // header's exact-values button follows whichever shows: on for a stage, with its dot for
    // a value that page leaves out and only on that page, and off, saying why, elsewhere.
    void everyPageIsItsOwnAndTheHeaderFollowsIt()
    {
        make();
        auto* header = m_window->findChild<esa::HeaderBar*>();
        auto* button = m_window->findChild<QPushButton*>(QStringLiteral("exactValues"));
        auto* gear = m_window->findChild<QMenu*>(QStringLiteral("gearMenu"));
        QVERIFY(header && button && gear);
        QAction* gearEntry = nullptr;
        for (QAction* action : gear->actions()) {
            if (action->text() == QLatin1String("Exact values...")) gearEntry = action;
        }
        QVERIFY(gearEntry);

        const QHash<QString, QString> panels{{QStringLiteral("gate"), QStringLiteral("AetherSDR::StripGatePanel")},
                                             {QStringLiteral("eq"), QStringLiteral("AetherSDR::StripEqPanel")},
                                             {QStringLiteral("comp"), QStringLiteral("AetherSDR::StripCompPanel")},
                                             {QStringLiteral("tube"), QStringLiteral("AetherSDR::StripTubePanel")},
                                             {QStringLiteral("exciter"), QStringLiteral("AetherSDR::StripPuduPanel")}};
        QStringList ids = m_window->pageIds();
        std::sort(ids.begin(), ids.end());
        QCOMPARE(ids, (QStringList{QStringLiteral("comp"), QStringLiteral("eq"), QStringLiteral("exciter"),
                                   QStringLiteral("gate"), QStringLiteral("nr"), QStringLiteral("output"),
                                   QStringLiteral("radio"), QStringLiteral("tube")}));
        for (const QString& id : m_window->pageIds()) {
            m_window->showPage(id);
            QCOMPARE(m_window->currentPage(), id);
            auto* page = m_window->findChild<QWidget*>(QStringLiteral("page_") + id);
            QVERIFY2(page && page->isVisible(), qPrintable(id));
            const esa::PageEntry* entry = esa::findPage(id);
            QVERIFY(entry);
            if (entry->stage.isEmpty()) {
                if (id == QLatin1String("radio")) QVERIFY(qobject_cast<esa::RadioPage*>(page));
                else if (id == QLatin1String("nr")) QVERIFY(qobject_cast<esa::NrPage*>(page));
                else QVERIFY2(qobject_cast<esa::OutputPage*>(page), qPrintable(id));
                QVERIFY2(!button->isEnabled() && !gearEntry->isEnabled(), qPrintable(id));
                QCOMPARE(button->toolTip(), QStringLiteral("Exact values are for the stage pages."));
            } else {
                auto* stagePage = qobject_cast<esa::StageHost*>(page);
                QVERIFY2(stagePage, qPrintable(id));
                QCOMPARE(stagePage->stage(), entry->stage);
                QCOMPARE(QString::fromLatin1(stagePage->panel()->metaObject()->className()), panels.value(id));
                QVERIFY2(button->isEnabled() && gearEntry->isEnabled(), qPrintable(id));
                QVERIFY2(!header->exactValuesFlagged(), qPrintable(id));
                QVERIFY2(button->toolTip().startsWith(QLatin1String("Exact values: every setting of this stage")),
                         qPrintable(id));
                // The button opens the window of the page that shows.
                auto* details = m_window->findChild<esa::DetailsWindow*>(QStringLiteral("details_") + entry->stage);
                QVERIFY2(details && !details->isVisible(), qPrintable(id));
                button->click();
                QTRY_VERIFY2(details->isVisible(), qPrintable(id));
                details->close();
                QTRY_VERIFY(!details->isVisible());
            }
        }

        // What each page leaves out, away from its default, marks that page and no other.
        const QList<QPair<QString, QPair<QString, double>>> hidden{
            {QStringLiteral("gate"), {QStringLiteral("gate.attack"), 5.0}},
            {QStringLiteral("gate"), {QStringLiteral("gate.lookahead"), 2.3}},  // between the Peek stops
            {QStringLiteral("eq"), {QStringLiteral("eq.master"), -2.0}},
            {QStringLiteral("comp"), {QStringLiteral("comp.drive"), 2.0}},
            {QStringLiteral("comp"), {QStringLiteral("comp.rotator"), 4.0}},
            {QStringLiteral("tube"), {QStringLiteral("tube.attack"), 9.0}}};
        const QString changedNote = QStringLiteral("Some of those the page does not show are changed from their defaults.");
        for (const auto& [id, change] : hidden) {
            const auto& [key, value] = change;
            esa::ParamControl* c = control(key);
            QVERIFY2(c, qPrintable(key));
            const double def = c->spec().def;
            c->setValue(value);
            for (const QString& other : m_window->pageIds()) {
                m_window->showPage(other);
                const bool marked = other == id;
                QVERIFY2(header->exactValuesFlagged() == marked, qPrintable(key + QStringLiteral(" on ") + other));
                QVERIFY2(button->toolTip().contains(changedNote) == marked, qPrintable(key + QStringLiteral(" on ") + other));
            }
            c->setValue(def);
            m_window->showPage(id);
            QVERIFY2(!header->exactValuesFlagged(), qPrintable(key));
        }
    }

    // The status chip opens the page concerned, and that is the real page: Final Output for
    // an output error; AetherNR when the chain cannot keep up, or NR failed, is preparing or
    // is bypassed for the mode.
    void theStatusChipOpensThePageConcerned()
    {
        make();
        auto* chip = m_window->findChild<esa::StatusChip*>();
        QVERIFY(chip);
        esa::EngineStatus ready;
        ready.connection = esa::TciClient::State::Ready;
        ready.audioFlowing = true;
        struct Row {
            const char* what;
            esa::EngineStatus status;
            QString page;
        };
        QList<Row> rows;
        Row row{"output error", ready, QStringLiteral("output")};
        row.status.outputError = QStringLiteral("Device gone");
        rows << row;
        row = {"can't keep up", ready, QStringLiteral("nr")};
        row.status.dspLoad = 0.9;
        rows << row;
        row = {"NR failed", ready, QStringLiteral("nr")};
        row.status.nrFailed = true;
        rows << row;
        row = {"NR preparing", ready, QStringLiteral("nr")};
        row.status.nrPreparing = true;
        rows << row;
        row = {"NR bypassed", ready, QStringLiteral("nr")};
        row.status.nrBypassedByMode = true;
        row.status.modulation = QStringLiteral("CW");
        rows << row;
        for (const Row& r : rows) {
            m_window->showPage(QStringLiteral("gate"));
            chip->showStatus(r.status);
            QVERIFY2(chip->summary().page == r.page, r.what);
            chip->click();
            QVERIFY2(m_window->currentPage() == r.page, r.what);
            if (r.page == QLatin1String("output")) {
                auto* page = m_window->findChild<esa::OutputPage*>();
                QVERIFY2(page && page->isVisible(), r.what);
            } else {
                auto* page = m_window->findChild<esa::NrPage*>();
                QVERIFY2(page && page->isVisible(), r.what);
            }
        }
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
        // This computer's output away from its defaults too: Final Output shows it.
        esa::AppConfig config;
        config.outputGainDb = -12.3;
        config.boost = true;
        make(config);
        // Somewhere other than the factory settings, saved so Modified starts clear. Among
        // them what a page leaves out, which upstream's panels would reset as they show.
        control(QStringLiteral("gate.threshold"))->setValue(-59.5);
        control(QStringLiteral("gate.ratio"))->setValue(3.7);
        control(QStringLiteral("gate.attack"))->setValue(7.5);
        control(QStringLiteral("gate.lookahead"))->setValue(2.3);
        control(QStringLiteral("comp.drive"))->setValue(3.0);
        control(QStringLiteral("comp.rotator"))->setValue(4.0);
        control(QStringLiteral("tube.attack"))->setValue(9.0);
        control(QStringLiteral("eq.master"))->setValue(-2.0);
        QVERIFY(m_window->saveProfileAs(QStringLiteral("Before")));
        const QHash<QString, double> before = m_window->config().params;
        const esa::AppConfig was = m_window->config();
        QVERIFY(!m_window->chainModified());

        for (const QString& id : m_window->pageIds()) {
            m_window->showPage(id);
            QTest::qWait(60);  // several panel ticks
        }
        QCOMPARE(m_window->config().params, before);
        QVERIFY(!m_window->chainModified());
        const esa::AppConfig& now = m_window->config();
        QCOMPARE(now.outputGainDb, -12.3);
        QVERIFY(now.boost);
        QCOMPARE(now.nrMethod, was.nrMethod);
        QCOMPARE(now.stageOrder, was.stageOrder);
        QCOMPARE(now.deviceKey, was.deviceKey);
        QCOMPARE(now.bufferMs, was.bufferMs);
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
                || c.startsWith(QLatin1String("mute")) || c.startsWith(QLatin1String("rx_volume"))
                || c.startsWith(QLatin1String("rx_mute"))) {
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

    // ── The EQ page ───────────────────────────────────────────────────────────

    // Upstream's EQ panel, bound to the receive side, as the window relies on it: no filter
    // widths or edges (nothing reads the radio's filter), the band plan under the curve, ten
    // bands, an OUT strip that only reports, and its keys split between the page, the rail
    // and the exact-values window.
    void theEqPageIsUpstreamsReceivePanel()
    {
        make();
        esa::StageHost* eq = host(QStringLiteral("eq"));
        QVERIFY(eq);
        QCOMPARE(eq->objectName(), QStringLiteral("page_eq"));
        StripEqPanel* panel = eqPanel();
        QVERIFY(panel);
        m_window->showPage(QStringLiteral("eq"));
        QTest::qWait(50);  // the rows' columns, shown once the event loop runs
        ClientEqEditorCanvas* canvas = eqCanvas();
        QVERIFY(canvas && canvas->isVisible());
        QCOMPARE(canvas->filterLowCutHz(), 0);
        QCOMPARE(canvas->filterHighCutHz(), 0);
        for (QPushButton* button : panel->findChildren<QPushButton*>()) {
            QVERIFY2(!button->objectName().startsWith(QLatin1String("eqRxFilter")), qPrintable(button->objectName()));
        }
        QCOMPARE(view().eq.activeBandCount(), ClientEq::kDefaultBandCount);
        auto* icons = panel->findChild<ClientEqIconRow*>();
        auto* readings = panel->findChild<ClientEqParamRow*>();
        QVERIFY(icons && readings);
        QCOMPARE(icons->layout()->count(), ClientEq::kDefaultBandCount);
        QCOMPARE(readings->layout()->count(), ClientEq::kDefaultBandCount);
        const auto faders = panel->findChildren<ClientEqOutputFader*>();
        QCOMPARE(faders.size(), 1);
        QVERIFY(faders.first()->isVisible());
        QVERIFY(!faders.first()->gainControlEnabled());
        // The floating editor's title bar is taken off: the rail names the page.
        EditorFramelessTitleBar* plate = titlePlate(panel);
        QVERIFY(plate && !plate->isVisible());

        // The band plan: the SSB segment's own colour at 150 Hz, clear of its label.
        const QImage curve = canvas->grab().toImage();
        const qreal dpr = curve.devicePixelRatio();
        const QPoint at(int(canvas->width() * std::log10(150.0 / 20.0) / 3.0 * dpr),
                        int((canvas->height() - ClientEqCurveWidget::kAudioBandStripPx / 2) * dpr));
        const QColor ssb = curve.pixelColor(at);
        QVERIFY2(std::abs(ssb.red() - 0x6c) <= 3 && std::abs(ssb.green() - 0x39) <= 3 && std::abs(ssb.blue() - 0x0c) <= 3,
                 qPrintable(ssb.name()));

        // The page owns the family and every band's six keys; the EQ's on/off is the rail's,
        // and its output gain is in the exact values only.
        QStringList owned{QStringLiteral("eq.family")};
        for (int band = 0; band < ClientEq::kDefaultBandCount; ++band) {
            for (const char* leaf : {"on", "freq", "gain", "q", "type", "slope"}) {
                owned << QStringLiteral("eq.b%1.%2").arg(band).arg(QLatin1String(leaf));
            }
        }
        QCOMPARE(binding()->ownedKeys(QStringLiteral("eq")), owned);
        auto* details = m_window->findChild<esa::DetailsWindow*>(QStringLiteral("details_eq"));
        QVERIFY(details && details->hasControls() && !details->isVisible());
        QStringList shown;
        for (esa::ParamControl* c : details->controls()) shown << c->spec().key;
        QCOMPARE(shown, QStringList{QStringLiteral("eq.enabled")} + owned + QStringList{QStringLiteral("eq.master")});
        QCOMPARE(esa::DetailsWindow::keysNotOnPage(QStringLiteral("eq")), QStringList{QStringLiteral("eq.master")});
        // An output gain moved from unity is marked on the header's exact-values button.
        auto* header = m_window->findChild<esa::HeaderBar*>();
        QVERIFY(header && !header->exactValuesFlagged());
        control(QStringLiteral("eq.master"))->setValue(-2.0);
        QVERIFY(header->exactValuesFlagged());
        QCOMPARE(view().eq.masterGain(), float(std::pow(10.0, -2.0 / 20.0)));
    }

    // Every key the EQ page owns, both ways: set in the mirror as the curve, the icons and the
    // fields set it, then saved as they save, it reaches the controls, the settings and the
    // chain on the table's decimals; set from a control, the mirror and the page show it.
    void everyEqKeyGoesBothWays()
    {
        make();
        m_window->showPage(QStringLiteral("eq"));
        using Type = ClientEq::FilterType;
        // Upstream's types, and the place of each among eq.bN.type's choices.
        const QList<QPair<Type, double>> types{{Type::Peak, 2.0}, {Type::LowShelf, 1.0}, {Type::HighShelf, 3.0},
                                               {Type::LowPass, 4.0}, {Type::HighPass, 0.0}};
        const double freqs[] = {41.3, 133.6, 251.2, 433.7, 777.4, 1876.4, 2876.3, 4444.4, 7777.7, 11111.2};
        const double gains[] = {-7.37, 5.12, -3.33, 2.21, -1.11, 0.66, 9.94, -12.42, 15.53, -17.38};
        QHash<QString, double> expected;
        for (int band = 0; band < ClientEq::kDefaultBandCount; ++band) {
            ClientEq::BandParams p = view().eq.band(band);
            const auto key = [band](const char* leaf) { return QStringLiteral("eq.b%1.%2").arg(band).arg(QLatin1String(leaf)); };
            p.enabled = true;
            p.freqHz = float(freqs[band]);
            p.gainDb = float(gains[band]);
            p.q = 2.346f;
            const auto& type = types.at((band + 1) % types.size());
            p.type = type.first;
            p.slopeDbPerOct = 36;
            view().eq.setBand(band, p);
            expected.insert(key("on"), 1.0);
            expected.insert(key("freq"), roundedTo(freqs[band], 0));
            expected.insert(key("gain"), roundedTo(gains[band], 1));
            expected.insert(key("q"), 2.35);
            expected.insert(key("type"), type.second);
            expected.insert(key("slope"), 2.0);  // 36 dB/oct
        }
        view().eq.setFilterFamily(ClientEq::FilterFamily::Chebyshev);
        expected.insert(QStringLiteral("eq.family"), 1.0);
        shim()->saveClientEqSettings();

        const QStringList owned = binding()->ownedKeys(QStringLiteral("eq"));
        QCOMPARE(QSet<QString>(owned.begin(), owned.end()), QSet<QString>(expected.keyBegin(), expected.keyEnd()));
        for (const QString& key : owned) {
            const double want = expected.value(key);
            QVERIFY2(std::abs(control(key)->value() - want) < 1e-9,
                     qPrintable(QStringLiteral("%1: control %2, wanted %3").arg(key).arg(control(key)->value()).arg(want)));
            QVERIFY2(std::abs(held(key) - want) < 1e-9, qPrintable(key));
            QVERIFY2(std::abs(inChain(key) - want) < 1e-3, qPrintable(QStringLiteral("%1: chain %2").arg(key).arg(inChain(key))));
            double mirrored = 0.0;
            QVERIFY(esa::RxChain::readStageParam(view(), key, &mirrored));
            QVERIFY2(std::abs(mirrored - want) < 1e-3, qPrintable(key));
        }
        // Reset puts every band back as upstream defines it, keeping all ten, on the table's
        // decimals: upstream's Q of 0.707 is held as the 0.71 the page shows.
        QPushButton* reset = nullptr;
        for (QPushButton* b : eqPanel()->findChildren<QPushButton*>()) {
            if (b->text() == QLatin1String("Reset")) reset = b;
        }
        QVERIFY(reset);
        reset->click();
        QCOMPARE(view().eq.activeBandCount(), ClientEq::kDefaultBandCount);
        for (const QString& key : owned) {
            const esa::ParamSpec* spec = esa::findParamSpec(key);
            const double want = roundedTo(spec->def, spec->decimals);
            QVERIFY2(std::abs(held(key) - want) < 1e-9, qPrintable(QStringLiteral("%1: %2").arg(key).arg(held(key))));
            QVERIFY2(std::abs(inChain(key) - want) < 1e-3, qPrintable(key));
        }
        // So the window is back on its factory settings: 0.71 is the 0.707 it shows as.
        QVERIFY(!m_window->chainModified());

        // The other way: from the exact values to the mirror, the readings, the icons and the combo.
        control(QStringLiteral("eq.b3.freq"))->setValue(1500);
        control(QStringLiteral("eq.b3.gain"))->setValue(-4.5);
        control(QStringLiteral("eq.b3.on"))->setValue(1);
        QCOMPARE(view().eq.band(3).freqHz, 1500.0f);
        QCOMPARE(view().eq.band(3).gainDb, -4.5f);
        QTRY_COMPARE(bandField(3, 0)->text(), QStringLiteral("1.50 kHz"));
        QCOMPARE(bandField(3, 1)->text(), QStringLiteral("-4.5 dB"));
        control(QStringLiteral("eq.b3.type"))->setValue(0);  // a high-pass, which has no gain to set
        QCOMPARE(view().eq.band(3).type, Type::HighPass);
        QTRY_VERIFY(!bandField(3, 1)->isEnabled());
        control(QStringLiteral("eq.b3.slope"))->setValue(3);
        QCOMPARE(view().eq.band(3).slopeDbPerOct, 48);
        control(QStringLiteral("eq.family"))->setValue(2);
        QCOMPARE(view().eq.filterFamily(), ClientEq::FilterFamily::Bessel);
        QComboBox* family = eqFamilyCombo();
        QVERIFY(family);
        QTRY_COMPARE(family->currentText(), QStringLiteral("Bessel"));

        // Keys the page does not own go one way, from the settings: what the page writes there
        // on its own is put back, as upstream's forced unity master is.
        QVERIFY(m_window->saveProfileAs(QStringLiteral("Set")));
        view().eq.setMasterGain(0.5f);
        view().eq.setEnabled(true);
        shim()->saveClientEqSettings();
        QCOMPARE(held(QStringLiteral("eq.master")), 0.0);
        QCOMPARE(held(QStringLiteral("eq.enabled")), 0.0);
        QCOMPARE(view().eq.masterGain(), 1.0f);
        QVERIFY(!view().eq.isEnabled());
        QVERIFY(!m_window->chainModified());
    }

    // Opening the page, looking and clicking about, changes nothing, even for a gain past the
    // curve's ±18 dB, which the table keeps to ±24: it draws pinned, and only a drag of its own
    // brings it within the curve's range. The output gain, forced to unity by upstream on
    // receive, is never touched.
    void openingTheEqPageChangesNoBand()
    {
        make();
        esa::ChainSettings chain;
        for (const esa::ParamSpec& spec : esa::RxChain::stageParams()) {
            if (esa::StageBinding::stageOf(spec.key) == QLatin1String("eq")) chain.params.insert(spec.key, nonDefault(spec));
        }
        chain.params.insert(QStringLiteral("eq.b1.gain"), 22.0);
        chain.params.insert(QStringLiteral("eq.b1.freq"), 120.0);
        chain.params.insert(QStringLiteral("eq.b1.type"), 2.0);  // a peak, whose handle has a height
        chain.params.insert(QStringLiteral("eq.master"), -2.0);
        QVERIFY(m_profiles->save(QStringLiteral("Shaped"), chain));
        m_window->loadProfile(QStringLiteral("Shaped"));
        const QHash<QString, double> before = m_window->config().params;
        QVERIFY(!m_window->chainModified());

        m_window->showPage(QStringLiteral("eq"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        QTest::qWait(100);  // several analyser ticks
        ClientEqEditorCanvas* canvas = eqCanvas();
        QVERIFY(canvas);
        // The page shows what the profile holds: every band's readings, in upstream's formats,
        // and the family.
        for (int band = 0; band < ClientEq::kDefaultBandCount; ++band) {
            const auto value = [&](const char* leaf) {
                return chain.params.value(QStringLiteral("eq.b%1.%2").arg(band).arg(QLatin1String(leaf)));
            };
            const double hz = value("freq");
            const QString freqText = hz >= 1000.0 ? QString::number(hz / 1000.0, 'f', 2) + QStringLiteral(" kHz")
                                                  : QString::number(qRound(hz)) + QStringLiteral(" Hz");
            QCOMPARE(bandField(band, 0)->text(), freqText);
            QCOMPARE(bandField(band, 1)->text(), QString::asprintf("%+.1f dB", value("gain")));
            QCOMPARE(bandField(band, 2)->text(), QString::number(value("q"), 'f', 2));
        }
        QCOMPARE(bandField(1, 1)->text(), QStringLiteral("+22.0 dB"));
        QVERIFY(eqFamilyCombo());
        QCOMPARE(eqFamilyCombo()->currentIndex(), int(chain.params.value(QStringLiteral("eq.family"))));
        // A click on the curve away from every handle, a band's column, and each of a band's
        // fields given the keyboard and left.
        const QPointF empty(canvas->width() - 3.0, 3.0);
        mouse(canvas, QEvent::MouseButtonPress, empty);
        mouse(canvas, QEvent::MouseButtonRelease, empty);
        QWidget* column = bandField(5, 0)->parentWidget();
        QTest::mouseClick(column, Qt::LeftButton, Qt::NoModifier, QPoint(column->width() / 2, 1));
        for (int which = 0; which < 3; ++which) {
            QLineEdit* field = bandField(1, which);
            field->setFocus(Qt::MouseFocusReason);
            QTRY_VERIFY(field->hasFocus());
            QLineEdit* other = bandField(6, 0);
            other->setFocus(Qt::MouseFocusReason);
            QTRY_VERIFY(other->hasFocus());
        }
        QTest::qWait(100);
        QCOMPARE(m_window->config().params, before);
        QVERIFY(!m_window->chainModified());
        QCOMPARE(view().eq.band(1).gainDb, 22.0f);
        QCOMPARE(held(QStringLiteral("eq.master")), -2.0);
        QVERIFY(std::abs(20.0 * std::log10(view().eq.masterGain()) - -2.0) < 1e-3);
        QVERIFY(std::abs(inChain(QStringLiteral("eq.master")) - -2.0) < 1e-3);

        // Its first drag brings it within the curve's range: pinned at the top, a pixel down
        // is just under +18 dB.
        const QPointF pinned = eqHandle(canvas, view().eq, 1);
        QCOMPARE(pinned.y(), 0.0);
        mouse(canvas, QEvent::MouseButtonPress, pinned);
        mouse(canvas, QEvent::MouseMove, pinned + QPointF(0, 2));
        mouse(canvas, QEvent::MouseButtonRelease, pinned + QPointF(0, 2));
        const double gain = held(QStringLiteral("eq.b1.gain"));
        QVERIFY2(gain <= 18.0 && gain > 17.0, qPrintable(QString::number(gain)));
        QVERIFY(std::abs(inChain(QStringLiteral("eq.b1.gain")) - gain) < 1e-3);
    }

    // The done-when case: a drag on the curve is heard as it goes, every step reaching the
    // chain the audio runs through before the button is let go, a fast hand coalesced into a
    // step at a time, and upstream's save on release finding nothing left to send.
    void aDragOnTheCurveIsHeardAsItGoes()
    {
        make();
        m_window->showPage(QStringLiteral("eq"));
        ClientEqEditorCanvas* canvas = eqCanvas();
        QVERIFY(canvas && canvas->isVisible());
        // Counted on the way to the binding, and put back however the test ends.
        int saves = 0;
        const AudioEngine::Hooks original = shim()->hooks();
        const auto restore = qScopeGuard([this, original] { shim()->setHooks(original); });
        AudioEngine::Hooks counting = original;
        counting.saveStage = [&saves, save = original.saveStage](const QString& stage) {
            if (stage == QLatin1String("eq")) ++saves;
            save(stage);
        };
        shim()->setHooks(counting);

        const QString on = QStringLiteral("eq.b4.on");
        const QString freq = QStringLiteral("eq.b4.freq");
        const QString gain = QStringLiteral("eq.b4.gain");
        QSignalSpy freqSent(control(freq), &esa::ParamControl::valueChanged);
        QSignalSpy gainSent(control(gain), &esa::ParamControl::valueChanged);
        constexpr int kStep = 33;  // the page's drag step, in ms

        // Band 5 is off at 800 Hz: grabbing its handle turns it on, at once.
        const QPointF start = eqHandle(canvas, view().eq, 4);
        mouse(canvas, QEvent::MouseButtonPress, start);
        QCOMPARE(held(on), 1.0);
        QCOMPARE(inChain(on), 1.0);

        // Three steps, each after the last was taken: each is in the chain before the release.
        double last = held(gain);
        for (int i = 1; i <= 3; ++i) {
            QTest::qWait(kStep + 20);
            const QPointF at = start + QPointF(12.0 * i, -15.0 * i);
            mouse(canvas, QEvent::MouseMove, at);
            QTRY_VERIFY2(held(gain) > last, qPrintable(QStringLiteral("step %1 after %2").arg(i).arg(last)));
            const double g = held(gain);
            QVERIFY2(std::abs(g - eqGainAt(canvas, at.y())) <= 0.051, qPrintable(QString::number(g)));
            QVERIFY2(std::abs(held(freq) - eqFrequencyAt(canvas, at.x())) <= 0.51, qPrintable(QString::number(held(freq))));
            QVERIFY(std::abs(inChain(gain) - g) < 1e-3);
            QVERIFY(std::abs(inChain(freq) - held(freq)) < 1e-3);
            last = g;
        }
        QCOMPARE(saves, 0);  // upstream saves only on release

        // A fast hand: fifty moves at once are a step now and one when the step is up.
        QTest::qWait(kStep + 20);
        const int freqBefore = int(freqSent.count());
        QPointF at = start;
        for (int i = 1; i <= 50; ++i) {
            at = start + QPointF(36.0 + i, -45.0 - i);
            mouse(canvas, QEvent::MouseMove, at);
        }
        QTest::qWait(3 * kStep);
        const int burst = int(freqSent.count()) - freqBefore;
        QVERIFY2(burst >= 1 && burst <= 2, qPrintable(QString::number(burst)));
        QVERIFY2(std::abs(held(gain) - eqGainAt(canvas, at.y())) <= 0.051, qPrintable(QString::number(held(gain))));
        QVERIFY2(std::abs(held(freq) - eqFrequencyAt(canvas, at.x())) <= 0.51, qPrintable(QString::number(held(freq))));
        QVERIFY(std::abs(inChain(gain) - held(gain)) < 1e-3);
        QVERIFY(std::abs(inChain(freq) - held(freq)) < 1e-3);

        // Let go: upstream's save, once, and the value already taken is not sent again.
        const int sentBefore = int(freqSent.count() + gainSent.count());
        mouse(canvas, QEvent::MouseButtonRelease, at);
        QCOMPARE(saves, 1);
        QTest::qWait(3 * kStep);
        QCOMPARE(int(freqSent.count() + gainSent.count()), sentBefore);
        QCOMPARE(saves, 1);
        QVERIFY(m_window->chainModified());
        QCOMPARE(binding()->firstMismatch(), QString());
    }

    // A type picked on the icons and a slope picked from the curve's menu are kept in the
    // settings, saved with a profile and loaded back with it.
    void theTypeIconsAndTheSlopeMenuPersist()
    {
        make();
        m_window->showPage(QStringLiteral("eq"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        QTest::qWait(50);
        auto* icons = eqPanel()->findChild<ClientEqIconRow*>();
        QVERIFY(icons);
        const auto icon = [icons](int band) { return icons->layout()->itemAt(band)->widget(); };
        using Type = ClientEq::FilterType;

        // Band 3 is a peak: a click makes it a low shelf, and turns it on.
        QTest::mouseClick(icon(2), Qt::LeftButton);
        QCOMPARE(view().eq.band(2).type, Type::LowShelf);
        QCOMPARE(held(QStringLiteral("eq.b2.type")), 1.0);
        QCOMPARE(held(QStringLiteral("eq.b2.on")), 1.0);
        QCOMPARE(inChain(QStringLiteral("eq.b2.type")), 1.0);
        // Band 10 is a low-pass: Shift goes back one, to a high shelf.
        QTest::mouseClick(icon(9), Qt::LeftButton, Qt::ShiftModifier);
        QCOMPARE(view().eq.band(9).type, Type::HighShelf);
        QCOMPARE(held(QStringLiteral("eq.b9.type")), 3.0);
        QCOMPARE(inChain(QStringLiteral("eq.b9.type")), 3.0);

        // Band 1 is a high-pass at 12 dB/oct: its handle's menu makes it 24.
        ClientEqEditorCanvas* canvas = eqCanvas();
        const QPoint handle = eqHandle(canvas, view().eq, 0).toPoint();
        chooseInNextMenu({QStringLiteral("Slope"), QStringLiteral("24 dB/oct")});
        QContextMenuEvent menu(QContextMenuEvent::Mouse, handle, canvas->mapToGlobal(handle));
        QApplication::sendEvent(canvas, &menu);
        QCOMPARE(view().eq.band(0).slopeDbPerOct, 24);
        QCOMPARE(held(QStringLiteral("eq.b0.slope")), 1.0);
        QCOMPARE(held(QStringLiteral("eq.b0.on")), 1.0);
        QCOMPARE(inChain(QStringLiteral("eq.b0.slope")), 1.0);

        // Into a profile; gone with the factory settings; back with the profile.
        QVERIFY(m_window->saveProfileAs(QStringLiteral("Shaped")));
        m_window->loadProfile(QString());
        QCOMPARE(held(QStringLiteral("eq.b2.type")), 2.0);
        QCOMPARE(held(QStringLiteral("eq.b0.slope")), 0.0);
        QCOMPARE(view().eq.band(2).type, Type::Peak);
        QCOMPARE(view().eq.band(0).slopeDbPerOct, 12);
        m_window->loadProfile(QStringLiteral("Shaped"));
        QCOMPARE(held(QStringLiteral("eq.b2.type")), 1.0);
        QCOMPARE(held(QStringLiteral("eq.b9.type")), 3.0);
        QCOMPARE(held(QStringLiteral("eq.b0.slope")), 1.0);
        QCOMPARE(view().eq.band(2).type, Type::LowShelf);
        QCOMPARE(view().eq.band(9).type, Type::HighShelf);
        QCOMPARE(view().eq.band(0).slopeDbPerOct, 24);
        QCOMPARE(inChain(QStringLiteral("eq.b2.type")), 1.0);
        QCOMPARE(inChain(QStringLiteral("eq.b0.slope")), 1.0);
        QVERIFY(!m_window->chainModified());
        // As the next start reads it, from the file.
        const esa::ProfileLibrary again(m_profiles->path());
        const esa::Profile* shaped = again.find(QStringLiteral("Shaped"));
        QVERIFY(shaped);
        QCOMPARE(shaped->chain.params.value(QStringLiteral("eq.b2.type")), 1.0);
        QCOMPARE(shaped->chain.params.value(QStringLiteral("eq.b9.type")), 3.0);
        QCOMPARE(shaped->chain.params.value(QStringLiteral("eq.b0.slope")), 1.0);
    }

    // A band's fields under the curve commit only what was typed, as the knobs' do, and stay
    // guarded after the row builds them anew. Unguarded, clicking band 5's frequency and
    // leaving it read back its "1.23 kHz" as 1230 Hz and turned the band on.
    void aBandFieldCommitsOnlyWhatWasTyped()
    {
        make();
        control(QStringLiteral("eq.b4.freq"))->setValue(1234);
        QVERIFY(m_window->saveProfileAs(QStringLiteral("Set")));
        m_window->showPage(QStringLiteral("eq"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        QTest::qWait(50);
        enum { Freq, Gain, Q };
        const auto focus = [](QLineEdit* field) {
            QVERIFY(field);
            field->setFocus(Qt::MouseFocusReason);
            QTRY_VERIFY(field->hasFocus());
        };
        const auto elsewhere = [&] { focus(bandField(7, Freq)); };
        const auto untouched = [&] {
            QCOMPARE(held(QStringLiteral("eq.b4.on")), 0.0);
            QCOMPARE(held(QStringLiteral("eq.b4.freq")), 1234.0);
            QCOMPARE(held(QStringLiteral("eq.b4.q")), esa::findParamSpec(QStringLiteral("eq.b4.q"))->def);
            QVERIFY(!view().eq.band(4).enabled);
            QCOMPARE(view().eq.band(4).freqHz, 1234.0f);
            QVERIFY(!m_window->chainModified());
        };
        QCOMPARE(bandField(4, Freq)->text(), QStringLiteral("1.23 kHz"));
        auto* guard = eqPanel()->findChild<ClientEqParamRow*>()->findChild<esa::ValueFieldGuard*>();
        QVERIFY(guard);

        // Each of its fields given the keyboard and left; then Return in one, untouched.
        for (int which : {Freq, Gain, Q}) {
            QVERIFY(guard->fields().contains(bandField(4, which)));
            focus(bandField(4, which));
            elsewhere();
            untouched();
        }
        focus(bandField(4, Freq));
        QTest::keyClick(bandField(4, Freq), Qt::Key_Return);
        elsewhere();
        untouched();
        QCOMPARE(bandField(4, Freq)->text(), QStringLiteral("1.23 kHz"));

        // The row builds its columns anew on any change on the curve: the new fields are
        // guarded as well.
        const QPointer<QLineEdit> old = bandField(4, Q);
        emit eqCanvas()->bandsChanged();
        QTRY_VERIFY(old.isNull());
        QTRY_VERIFY(bandField(4, Q)->isVisible());
        QVERIFY(guard->fields().contains(bandField(4, Q)));
        focus(bandField(4, Q));
        elsewhere();
        untouched();

        // Typed, with Return: taken, and the band turns on, as upstream means an edit to.
        focus(bandField(4, Freq));
        bandField(4, Freq)->selectAll();
        QTest::keyClicks(bandField(4, Freq), QStringLiteral("1500"));
        QTest::keyClick(bandField(4, Freq), Qt::Key_Return);
        QCOMPARE(held(QStringLiteral("eq.b4.freq")), 1500.0);
        QCOMPARE(held(QStringLiteral("eq.b4.on")), 1.0);
        QVERIFY(std::abs(inChain(QStringLiteral("eq.b4.freq")) - 1500.0) < 1e-3);
        // Typed and left: taken.
        focus(bandField(4, Gain));
        bandField(4, Gain)->selectAll();
        QTest::keyClicks(bandField(4, Gain), QStringLiteral("-2.5"));
        elsewhere();
        QCOMPARE(held(QStringLiteral("eq.b4.gain")), -2.5);
        QVERIFY(std::abs(inChain(QStringLiteral("eq.b4.gain")) - -2.5) < 1e-3);
        // Typed, then Escape: dropped.
        focus(bandField(4, Q));
        bandField(4, Q)->selectAll();
        QTest::keyClicks(bandField(4, Q), QStringLiteral("3"));
        QTest::keyClick(bandField(4, Q), Qt::Key_Escape);
        elsewhere();
        QCOMPARE(held(QStringLiteral("eq.b4.q")), esa::findParamSpec(QStringLiteral("eq.b4.q"))->def);
    }

    // The analyser's samples: from the engine's post-EQ tap, as one channel, newest last, and
    // only while the page shows. Hidden, the engine writes nothing for it and the panel asks
    // for nothing.
    void theAnalyserRunsFromTheTapOnlyWhileTheEqShows()
    {
        {
            // The feed alone, on a tap of its own.
            auto taps = std::make_shared<esa::LiveTaps>();
            esa::EqFeed feed(taps);
            std::vector<float> out(2048, -1.0f);
            QVERIFY(!feed.copyRecent(out.data(), int(out.size())));
            QVERIFY(!taps->eqWanted.load());
            QWidget page;
            feed.follow(&page);
            page.show();
            QVERIFY(QTest::qWaitForWindowExposed(&page));
            QVERIFY(feed.isRunning() && taps->eqWanted.load());
            // Before the first block, silence: the analyser rests on its floor, as upstream's.
            QVERIFY(feed.copyRecent(out.data(), int(out.size())));
            QVERIFY(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }));

            std::vector<float> stereo(2 * 1000);
            const auto fill = [&stereo](int from) {
                for (int i = 0; i < 1000; ++i) {
                    stereo[size_t(2 * i)] = float(from + i) * 1e-4f;
                    stereo[size_t(2 * i + 1)] = -float(from + i) * 0.5e-4f;
                }
            };
            fill(0);
            QCOMPARE(taps->eqTap.write(stereo.data(), 1000), size_t(1000));
            QVERIFY(feed.copyRecent(out.data(), int(out.size())));
            for (int i = 0; i < 1048; ++i) QCOMPARE(out[size_t(i)], 0.0f);
            for (int i = 0; i < 1000; ++i) QCOMPARE(out[size_t(1048 + i)], 0.5f * (float(i) * 1e-4f - float(i) * 0.5e-4f));
            QCOMPARE(taps->eqTap.size(), size_t(0));
            // More than the history holds: the newest, in order.
            for (int block = 1; block < 6; ++block) {
                fill(1000 * block);
                QCOMPARE(taps->eqTap.write(stereo.data(), 1000), size_t(1000));
            }
            QVERIFY(feed.copyRecent(out.data(), int(out.size())));
            for (int i = 0; i < 2048; ++i) {
                const float n = float(6000 - 2048 + i);
                QCOMPARE(out[size_t(i)], 0.5f * (n * 1e-4f - n * 0.5e-4f));
            }
            // No samples for a while: silence again, so the trace falls away rather than freezing.
            QTest::qWait(esa::EqFeed::kStaleMs + 50);
            QVERIFY(feed.copyRecent(out.data(), int(out.size())));
            QVERIFY(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }));
            // The mirror's EQ, whose rate the panel places the bins by, follows the chain's.
            taps->chainRate.store(24000);
            QVERIFY(feed.copyRecent(out.data(), int(out.size())));
            QCOMPARE(taps->view.eq.sampleRate(), 24000.0);
            // Hidden: the engine is told to stop, and nothing is handed out.
            page.hide();
            QVERIFY(!feed.isRunning() && !taps->eqWanted.load());
            QVERIFY(!feed.copyRecent(out.data(), int(out.size())));
            // Shown again: what waited in the tap from before is dropped.
            QCOMPARE(taps->eqTap.write(stereo.data(), 1000), size_t(1000));
            page.show();
            QVERIFY(taps->eqWanted.load());
            QCOMPARE(taps->eqTap.size(), size_t(0));
        }

        // In the window, from the engine's own tap, with a radio's audio flowing.
        MockExpert radio;
        QVERIFY(radio.listen());
        make();
        const std::shared_ptr<esa::LiveTaps> taps = m_core->taps();
        m_window->showPage(QStringLiteral("gate"));
        esa::DiscoveredRadio picked;
        picked.host = QStringLiteral("127.0.0.1");
        picked.port = radio.port();
        m_window->connectToRadio(picked);
        QTRY_VERIFY_WITH_TIMEOUT(radio.audioStartCount() == 1, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(taps->blocks.load() > 10, 6000);
        QTest::qWait(300);  // the link settles
        const qsizetype commandsBefore = radio.commands.size();

        ClientEqEditorCanvas* canvas = eqCanvas();
        QVERIFY(canvas);
        auto* feed = eqPanel()->findChild<esa::EqFeed*>();
        QVERIFY(feed);
        // Hidden: nothing written for it, nothing asked.
        QVERIFY(!taps->eqWanted.load() && !feed->isRunning());
        const quint64 hidden = analyserUpdates(canvas);
        QTest::qWait(200);
        QCOMPARE(taps->eqTap.size(), size_t(0));
        QCOMPARE(analyserUpdates(canvas), hidden);

        // Shown: the analyser runs from the tap, which it keeps drained, on the radio's tone.
        m_window->showPage(QStringLiteral("eq"));
        QVERIFY(taps->eqWanted.load() && feed->isRunning());
        QTRY_VERIFY_WITH_TIMEOUT(analyserUpdates(canvas) > hidden + 10, 3000);
        QVERIFY2(taps->eqTap.size() < taps->eqTap.capacity() / 2, qPrintable(QString::number(taps->eqTap.size())));
        std::vector<float> heard(2048);
        QTRY_VERIFY_WITH_TIMEOUT(feed->copyRecent(heard.data(), int(heard.size()))
                                     && *std::max_element(heard.begin(), heard.end()) > 0.01f,
                                 3000);

        // Hidden again: the engine stops writing for it and the analyser stops.
        m_window->showPage(QStringLiteral("gate"));
        QVERIFY(!taps->eqWanted.load() && !feed->isRunning());
        const quint64 after = analyserUpdates(canvas);
        const size_t left = taps->eqTap.size();
        QTest::qWait(200);
        QCOMPARE(analyserUpdates(canvas), after);
        QCOMPARE(taps->eqTap.size(), left);
        QVERIFY2(radio.commands.size() == commandsBefore, qPrintable(radio.commands.mid(commandsBefore).join(u' ')));
    }

    // The EQ page as main() runs it, with the engine on a thread of its own: the analyser
    // draining the tap the engine's thread fills, and a drag on the curve heard as it goes,
    // with the engine's thread taking each step. The CI's ThreadSanitizer job runs this.
    void theEqPageRunsWithTheEngineOnItsOwnThread()
    {
        MockExpert radio;
        QVERIFY(radio.listen());
        QTemporaryDir dir;
        const QString ini = dir.filePath(QStringLiteral("threaded-eq.ini"));
        auto* engine = new esa::Engine(ini);
        const std::shared_ptr<esa::LiveTaps> taps = engine->taps();
        QThread thread;
        engine->moveToThread(&thread);
        thread.start();
        esa::Settings settings(ini);
        settings.setRecordingFolder(dir.filePath(QStringLiteral("Recordings")));
        esa::ProfileLibrary profiles(esa::ProfileLibrary::defaultPath(ini));
        // However the test ends, the window goes first and the engine after it, as in main(),
        // both while the settings they were made with are still there.
        std::unique_ptr<esa::MainWindow> window;
        const auto stopEngine = qScopeGuard([&] {
            window.reset();
            QMetaObject::invokeMethod(engine, &esa::Engine::shutdown, Qt::BlockingQueuedConnection);
            QMetaObject::invokeMethod(engine, [engine] { delete engine; }, Qt::BlockingQueuedConnection);
            thread.quit();
            thread.wait();
        });
        // Read on the engine's thread, handed back through an atomic: ThreadSanitizer does not
        // see the wait inside Qt, which is not built for it.
        const auto inChainNow = [engine](const QString& key) {
            std::atomic<double> value{std::nan("")};
            QMetaObject::invokeMethod(
                engine,
                [engine, key, &value] {
                    double read = std::nan("");
                    engine->chainParam(key, &read);
                    value.store(read);
                },
                Qt::BlockingQueuedConnection);
            return value.load();
        };

        esa::AppConfig config;
        config.deviceKey = QStringLiteral("null");  // never the speakers
        config.host = QStringLiteral("127.0.0.1");
        config.port = radio.port();
        config.fallbackPorts.clear();  // never the real radio's ports
        config.muteExpert = false;
        config.params.insert(QStringLiteral("eq.enabled"), 1);
        window = std::make_unique<esa::MainWindow>(engine, taps, &settings, &profiles, config);
        window->setQuitHandler([] {});
        window->setRevealHandler([](const QString&) {});
        window->setFolderChooser([](const QString&) { return QString(); });
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        window->showPage(QStringLiteral("eq"));

        esa::DiscoveredRadio picked;
        picked.host = QStringLiteral("127.0.0.1");
        picked.port = radio.port();
        window->connectToRadio(picked);
        QTRY_VERIFY_WITH_TIMEOUT(radio.audioStartCount() >= 1, 8000);

        esa::StageHost* eq = nullptr;
        for (esa::StageHost* h : window->findChildren<esa::StageHost*>()) {
            if (h->stage() == QLatin1String("eq")) eq = h;
        }
        QVERIFY(eq);
        auto* canvas = eq->panel()->findChild<ClientEqEditorCanvas*>(QStringLiteral("stripRxEqCanvas"));
        auto* feed = eq->panel()->findChild<esa::EqFeed*>();
        QVERIFY(canvas && feed && feed->isRunning() && taps->eqWanted.load());
        const quint64 before = analyserUpdates(canvas);
        std::vector<float> heard(2048);
        QTRY_VERIFY_WITH_TIMEOUT(feed->copyRecent(heard.data(), int(heard.size()))
                                     && *std::max_element(heard.begin(), heard.end()) > 0.01f,
                                 8000);
        QTRY_VERIFY_WITH_TIMEOUT(analyserUpdates(canvas) > before + 20, 3000);

        // Band 5's handle, up a step at a time while the audio flows.
        esa::ParamControl* gain = nullptr;
        for (esa::ParamControl* c : window->findChildren<esa::ParamControl*>()) {
            if (c->spec().key == QLatin1String("eq.b4.gain")) gain = c;
        }
        QVERIFY(gain);
        const QPointF start = eqHandle(canvas, taps->view.eq, 4);
        mouse(canvas, QEvent::MouseButtonPress, start);
        double last = gain->value();
        for (int i = 1; i <= 4; ++i) {
            QTest::qWait(60);
            mouse(canvas, QEvent::MouseMove, start + QPointF(8.0 * i, -12.0 * i));
            QTRY_VERIFY_WITH_TIMEOUT(gain->value() > last, 2000);
            last = gain->value();
            QTRY_VERIFY_WITH_TIMEOUT(std::abs(inChainNow(QStringLiteral("eq.b4.gain")) - last) < 1e-3, 2000);
        }
        mouse(canvas, QEvent::MouseButtonRelease, start + QPointF(32.0, -48.0));
        QTest::qWait(200);
        window->showPage(QStringLiteral("gate"));
        QVERIFY(!taps->eqWanted.load() && !feed->isRunning());
        QTest::qWait(200);

        QStringList changing;
        for (const QString& c : std::as_const(radio.commands)) {
            if (c.startsWith(QLatin1String("trx")) || c.startsWith(QLatin1String("volume"))
                || c.startsWith(QLatin1String("mute")) || c.startsWith(QLatin1String("rx_volume"))
                || c.startsWith(QLatin1String("rx_mute"))) {
                changing << c;
            }
        }
        QVERIFY2(changing.isEmpty(), qPrintable(changing.join(u' ')));
    }

    // ── The Compressor, Tube and Exciter pages ────────────────────────────────

    // Each is upstream's panel, hosted as the Gate's is, with every object the window and its
    // corrections look up.
    void theCompTubeAndExciterPagesAreUpstreamsPanels()
    {
        make();
        const QList<QPair<QString, QString>> pages = {{QStringLiteral("comp"), QStringLiteral("comp")},
                                                      {QStringLiteral("tube"), QStringLiteral("tube")},
                                                      {QStringLiteral("exciter"), QStringLiteral("pudu")}};
        for (const auto& [id, stage] : pages) {
            esa::StageHost* page = host(stage);
            QVERIFY2(page, qPrintable(stage));
            QCOMPARE(page->objectName(), QStringLiteral("page_") + id);
            QWidget* panel = page->panel();
            // Its knobs, one for each the spec names, by those names; one that edits nothing
            // on RX is not shown.
            const auto knobs = panel->findChildren<ClientCompKnob*>();
            QCOMPARE(knobs.size(), page->spec().knobs.size());
            QStringList names;
            for (ClientCompKnob* knob : knobs) names << knob->accessibleName();
            for (const auto& [label, key] : page->spec().knobs) {
                QVERIFY2(names.contains(label), qPrintable(label));
                if (key.isEmpty()) continue;
                ClientCompKnob* knob = page->knob(key);
                QVERIFY2(knob, qPrintable(key));
                QCOMPARE(knob->accessibleName(), label);
                QVERIFY2(knob->isVisibleTo(panel), qPrintable(key));
            }
            QCOMPARE(names.removeDuplicates(), 0);
            // The floating editor's title bar is taken off: the rail names the page.
            EditorFramelessTitleBar* plate = titlePlate(panel);
            QVERIFY(plate && !plate->isVisibleTo(panel));
        }

        QWidget* comp = host(QStringLiteral("comp"))->panel();
        QVERIFY(qobject_cast<StripCompPanel*>(comp));
        QVERIFY(comp->findChild<QSlider*>(QStringLiteral("compCeilingSlider")));
        QCOMPARE(comp->findChildren<ClientCompLimiterButton*>().size(), 1);
        QCOMPARE(comp->findChildren<ClientCompThresholdFader*>().size(), 1);
        ClientCompMeter* out = makeupMeter(comp);
        QVERIFY(out);
        QVERIFY(out->findChild<esa::MakeupHitGuard*>());
        // Upstream's transmit-only Drive and Phase are built on RX too, and hidden.
        QStringList hidden;
        for (ClientCompKnob* knob : comp->findChildren<ClientCompKnob*>()) {
            if (!knob->isVisibleTo(comp)) hidden << knob->accessibleName();
        }
        hidden.sort();
        QCOMPARE(hidden, (QStringList{QStringLiteral("Drive"), QStringLiteral("Phase")}));

        QWidget* tube = host(QStringLiteral("tube"))->panel();
        QVERIFY(qobject_cast<StripTubePanel*>(tube));
        for (const char* name : {"tubeModelA", "tubeModelB", "tubeModelC"}) {
            auto* button = tube->findChild<QPushButton*>(QLatin1String(name));
            QVERIFY2(button && button->isCheckable(), name);
        }
        auto* dryWet = tube->findChild<QSlider*>(QStringLiteral("tubeDryWetSlider"));
        QVERIFY(dryWet);
        QCOMPARE(dryWet->minimum(), 0);
        QCOMPARE(dryWet->maximum(), 100);

        QWidget* exciter = host(QStringLiteral("pudu"))->panel();
        QVERIFY(qobject_cast<StripPuduPanel*>(exciter));
        QCOMPARE(exciter->findChildren<PooDooLogo*>().size(), 1);
        for (const char* text : {"Even", "Odd"}) {
            QPushButton* button = buttonLabelled(exciter, QLatin1String(text));
            QVERIFY2(button && button->isCheckable(), text);
        }
    }

    // Each page owns every key it shows and no other: the rest of its stage is the rail's
    // on/off or is not on the page, and its exact-values window holds all of them, in those
    // three groups.
    void theCompTubeAndExciterOwnEveryKeyTheyShow()
    {
        make();
        const QList<QPair<QString, QStringList>> owned = {
            {QStringLiteral("comp"),
             {QStringLiteral("comp.threshold"), QStringLiteral("comp.ratio"), QStringLiteral("comp.attack"),
              QStringLiteral("comp.release"), QStringLiteral("comp.knee"), QStringLiteral("comp.makeup"),
              QStringLiteral("comp.limiter"), QStringLiteral("comp.ceiling")}},
            {QStringLiteral("tube"),
             {QStringLiteral("tube.model"), QStringLiteral("tube.drive"), QStringLiteral("tube.bias"),
              QStringLiteral("tube.tone"), QStringLiteral("tube.output"), QStringLiteral("tube.mix"),
              QStringLiteral("tube.envelope"), QStringLiteral("tube.release")}},
            {QStringLiteral("pudu"),
             {QStringLiteral("pudu.mode"), QStringLiteral("pudu.pooDrive"), QStringLiteral("pudu.pooTune"),
              QStringLiteral("pudu.pooMix"), QStringLiteral("pudu.dooTune"), QStringLiteral("pudu.dooHarmonics"),
              QStringLiteral("pudu.dooMix")}},
        };
        const QHash<QString, QStringList> notOnPage = {
            {QStringLiteral("comp"), {QStringLiteral("comp.drive"), QStringLiteral("comp.rotator")}},
            {QStringLiteral("tube"), {QStringLiteral("tube.attack")}},
            {QStringLiteral("pudu"), {}},
        };
        for (const auto& [stage, keys] : owned) {
            // In the table's order, which is the order they are pulled in.
            QCOMPARE(binding()->ownedKeys(stage), keys);
            QCOMPARE(esa::DetailsWindow::keysNotOnPage(stage), notOnPage.value(stage));
            const QStringList onRail{stage + QStringLiteral(".enabled")};
            const QStringList groups = onRail + keys + notOnPage.value(stage);
            QStringList all;
            for (const esa::ParamSpec& spec : esa::RxChain::stageParams()) {
                if (esa::StageBinding::stageOf(spec.key) == stage) all << spec.key;
            }
            QStringList sortedGroups = groups;
            sortedGroups.sort();
            all.sort();
            QCOMPARE(sortedGroups, all);  // every key in one group, once

            auto* details = m_window->findChild<esa::DetailsWindow*>(QStringLiteral("details_") + stage);
            QVERIFY(details && details->hasControls() && !details->isVisible());
            QStringList shown;
            for (esa::ParamControl* c : details->controls()) shown << c->spec().key;
            QCOMPARE(shown, groups);
            QStringList headings;
            for (QLabel* label : details->findChildren<QLabel*>(QStringLiteral("SectionLabel"))) headings << label->text();
            QStringList expected{QStringLiteral("ON THE RAIL"), QStringLiteral("ON THE PAGE")};
            if (!notOnPage.value(stage).isEmpty()) expected << QStringLiteral("NOT ON THE PAGE");
            QCOMPARE(headings, expected);
        }
    }

    // Every key these pages own: set in the mirror with upstream's setter, then saved as the
    // panel saves, it reaches the control, the settings and the chain, on the table's decimals.
    void theCompTubeAndExciterReadBackEveryKeyTheyOwn_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<double>("set");       // what the panel would write
        QTest::addColumn<double>("expected");  // on the table's decimals
        const auto row = [](const char* key, double set, double expected) {
            QTest::newRow(key) << QString::fromLatin1(key) << set << expected;
        };
        row("comp.threshold", -33.37, -33.4);
        row("comp.ratio", 2.553, 2.55);
        row("comp.attack", 12.34, 12.3);
        row("comp.release", 123.6, 124.0);
        row("comp.knee", 7.46, 7.5);
        row("comp.makeup", 3.26, 3.3);
        row("comp.limiter", 0.0, 0.0);
        row("comp.ceiling", -2.04, -2.0);
        row("tube.model", 2.0, 2.0);
        row("tube.drive", 7.923, 7.92);
        row("tube.bias", 0.114, 0.11);
        row("tube.tone", 0.587, 0.59);
        row("tube.output", 7.594, 7.59);
        row("tube.mix", 0.736, 0.74);
        row("tube.envelope", -0.514, -0.51);
        row("tube.release", 36.74, 36.7);
        row("pudu.mode", 1.0, 1.0);
        row("pudu.pooDrive", 6.04, 6.0);
        row("pudu.pooTune", 104.4, 104.0);
        row("pudu.pooMix", 0.304, 0.3);
        row("pudu.dooTune", 4903.6, 4904.0);
        row("pudu.dooHarmonics", 7.14, 7.1);
        row("pudu.dooMix", 0.296, 0.3);
    }
    void theCompTubeAndExciterReadBackEveryKeyTheyOwn()
    {
        QFETCH(QString, key);
        QFETCH(double, set);
        QFETCH(double, expected);
        make();
        const QString stage = esa::StageBinding::stageOf(key);
        QVERIFY(binding()->ownedKeys(stage).contains(key));

        esa::StageSet& s = view();
        const float v = float(set);
        if (key == QLatin1String("comp.threshold")) s.comp.setThresholdDb(v);
        else if (key == QLatin1String("comp.ratio")) s.comp.setRatio(v);
        else if (key == QLatin1String("comp.attack")) s.comp.setAttackMs(v);
        else if (key == QLatin1String("comp.release")) s.comp.setReleaseMs(v);
        else if (key == QLatin1String("comp.knee")) s.comp.setKneeDb(v);
        else if (key == QLatin1String("comp.makeup")) s.comp.setMakeupDb(v);
        else if (key == QLatin1String("comp.limiter")) s.comp.setLimiterEnabled(set != 0.0);
        else if (key == QLatin1String("comp.ceiling")) s.comp.setLimiterCeilingDb(v);
        else if (key == QLatin1String("tube.model")) s.tube.setModel(ClientTube::Model::C);
        else if (key == QLatin1String("tube.drive")) s.tube.setDriveDb(v);
        else if (key == QLatin1String("tube.bias")) s.tube.setBiasAmount(v);
        else if (key == QLatin1String("tube.tone")) s.tube.setTone(v);
        else if (key == QLatin1String("tube.output")) s.tube.setOutputGainDb(v);
        else if (key == QLatin1String("tube.mix")) s.tube.setDryWet(v);
        else if (key == QLatin1String("tube.envelope")) s.tube.setEnvelopeAmount(v);
        else if (key == QLatin1String("tube.release")) s.tube.setReleaseMs(v);
        else if (key == QLatin1String("pudu.mode")) s.pudu.setMode(ClientPudu::Mode::Behringer);
        else if (key == QLatin1String("pudu.pooDrive")) s.pudu.setPooDriveDb(v);
        else if (key == QLatin1String("pudu.pooTune")) s.pudu.setPooTuneHz(v);
        else if (key == QLatin1String("pudu.pooMix")) s.pudu.setPooMix(v);
        else if (key == QLatin1String("pudu.dooTune")) s.pudu.setDooTuneHz(v);
        else if (key == QLatin1String("pudu.dooHarmonics")) s.pudu.setDooHarmonicsDb(v);
        else if (key == QLatin1String("pudu.dooMix")) s.pudu.setDooMix(v);
        else QFAIL(qPrintable(key));
        if (stage == QLatin1String("comp")) shim()->saveClientCompRxSettings();
        else if (stage == QLatin1String("tube")) shim()->saveClientTubeRxSettings();
        else shim()->saveClientPuduRxSettings();

        QCOMPARE(control(key)->value(), expected);
        QCOMPARE(held(key), expected);
        QVERIFY2(std::abs(inChain(key) - expected) < 1e-3, qPrintable(QString::number(inChain(key))));
        // The mirror took the rounded value back.
        double mirrored = 0.0;
        QVERIFY(esa::RxChain::readStageParam(view(), key, &mirrored));
        QVERIFY2(std::abs(mirrored - expected) < 1e-3, qPrintable(QString::number(mirrored)));
    }

    // Drive, Phase and the Tube's attack are not on their pages but stay live: what is set is
    // what the audio uses, however the compressor's panel forces Drive and Phase to zero each
    // time it is bound to RX, and the controls in the exact-values windows still change them.
    void driveAndPhaseAndTubeAttackStayLiveAndAreNeverForced()
    {
        esa::AppConfig config;
        config.params.insert(QStringLiteral("comp.drive"), 6.0);
        config.params.insert(QStringLiteral("comp.rotator"), 4.0);
        config.params.insert(QStringLiteral("tube.attack"), 12.5);
        make(config);
        const auto everywhere = [&](const QString& key, double expected) {
            double mirrored = 0.0;
            QVERIFY(esa::RxChain::readStageParam(view(), key, &mirrored));
            QVERIFY2(std::abs(held(key) - expected) < 1e-9 && std::abs(control(key)->value() - expected) < 1e-9
                         && std::abs(mirrored - expected) < 1e-4 && std::abs(inChain(key) - expected) < 1e-4,
                     qPrintable(QStringLiteral("%1: held %2, control %3, mirror %4, chain %5, wanted %6")
                                    .arg(key).arg(held(key)).arg(control(key)->value()).arg(mirrored)
                                    .arg(inChain(key)).arg(expected)));
        };
        // Bound as the window was built: what the panel forced then gave way to what is set.
        everywhere(QStringLiteral("comp.drive"), 6.0);
        everywhere(QStringLiteral("comp.rotator"), 4.0);
        everywhere(QStringLiteral("tube.attack"), 12.5);
        QVERIFY(m_window->saveProfileAs(QStringLiteral("Driven")));
        QVERIFY(!m_window->chainModified());

        // Bound to RX again, as upstream binds it whenever it shows the page: it zeroes both
        // and saves, and nothing follows.
        auto* panel = qobject_cast<StripCompPanel*>(host(QStringLiteral("comp"))->panel());
        QVERIFY(panel);
        QSignalSpy driveSent(control(QStringLiteral("comp.drive")), &esa::ParamControl::valueChanged);
        QSignalSpy phaseSent(control(QStringLiteral("comp.rotator")), &esa::ParamControl::valueChanged);
        panel->showForRx();
        QCOMPARE(driveSent.count(), 0);
        QCOMPARE(phaseSent.count(), 0);
        everywhere(QStringLiteral("comp.drive"), 6.0);
        everywhere(QStringLiteral("comp.rotator"), 4.0);
        for (const QString& id : m_window->pageIds()) {
            m_window->showPage(id);
            QTest::qWait(40);  // a couple of panel ticks
        }
        everywhere(QStringLiteral("comp.drive"), 6.0);
        everywhere(QStringLiteral("comp.rotator"), 4.0);
        everywhere(QStringLiteral("tube.attack"), 12.5);
        QVERIFY(!m_window->chainModified());

        // The exact-values window changes them, and the audio follows.
        control(QStringLiteral("comp.drive"))->setValue(3.0);
        control(QStringLiteral("comp.rotator"))->setValue(0.0);
        control(QStringLiteral("tube.attack"))->setValue(2.0);
        QTest::qWait(40);
        everywhere(QStringLiteral("comp.drive"), 3.0);
        everywhere(QStringLiteral("comp.rotator"), 0.0);
        everywhere(QStringLiteral("tube.attack"), 2.0);
        QVERIFY(m_window->chainModified());
    }

    // The header's exact-values button is on for these pages, and its dot marks a value the
    // page leaves out that is away from its default: Drive and Phase, and the Tube's attack.
    // The Exciter's page shows every value, so it never marks it.
    void theExactValuesDotMarksDriveAndPhaseAndTubeAttack()
    {
        make();
        auto* header = m_window->findChild<esa::HeaderBar*>();
        auto* button = m_window->findChild<QPushButton*>(QStringLiteral("exactValues"));
        QVERIFY(header && button);
        for (const char* id : {"comp", "tube", "exciter"}) {
            m_window->showPage(QLatin1String(id));
            QVERIFY2(button->isEnabled(), id);
            QVERIFY2(!header->exactValuesFlagged(), id);
        }
        m_window->showPage(QStringLiteral("comp"));
        control(QStringLiteral("comp.drive"))->setValue(2.0);
        QVERIFY(header->exactValuesFlagged());
        control(QStringLiteral("comp.drive"))->setValue(0.0);
        QVERIFY(!header->exactValuesFlagged());
        control(QStringLiteral("comp.rotator"))->setValue(4.0);
        QVERIFY(header->exactValuesFlagged());
        // Only on the page that leaves it out.
        m_window->showPage(QStringLiteral("tube"));
        QVERIFY(!header->exactValuesFlagged());
        control(QStringLiteral("tube.attack"))->setValue(9.0);
        QVERIFY(header->exactValuesFlagged());
        m_window->showPage(QStringLiteral("exciter"));
        for (const esa::ParamSpec& spec : esa::RxChain::stageParams()) {
            if (esa::StageBinding::stageOf(spec.key) == QLatin1String("pudu")) control(spec.key)->setValue(nonDefault(spec));
        }
        QVERIFY(!header->exactValuesFlagged());
        m_window->showPage(QStringLiteral("comp"));
        QVERIFY(header->exactValuesFlagged());
    }

    // A profile with every stage key away from its default: these pages show every value,
    // and showing them, in turn and again, changes nothing in the settings or the profile.
    void aProfileLoadReachesTheCompTubeAndExciterPages()
    {
        make();
        esa::ChainSettings chain;
        for (const esa::ParamSpec& spec : esa::RxChain::stageParams()) chain.params.insert(spec.key, nonDefault(spec));
        QVERIFY(m_profiles->save(QStringLiteral("Everything"), chain));
        m_window->loadProfile(QStringLiteral("Everything"));
        QCOMPARE(binding()->firstMismatch(), QString());
        const QHash<QString, double> loaded = m_window->config().params;

        const auto value = [&](const char* key) { return chain.params.value(QLatin1String(key)); };
        const QList<QPair<QString, QString>> pages = {{QStringLiteral("comp"), QStringLiteral("comp")},
                                                      {QStringLiteral("tube"), QStringLiteral("tube")},
                                                      {QStringLiteral("exciter"), QStringLiteral("pudu")}};
        for (int round = 0; round < 2; ++round) {
            for (const auto& [id, stage] : pages) {
                m_window->showPage(id);
                QTest::qWait(40);  // a couple of panel ticks
                esa::StageHost* page = host(stage);
                for (const auto& [label, key] : page->spec().knobs) {
                    if (key.isEmpty()) continue;
                    const double want = chain.params.value(key);
                    const double shown = page->knob(key)->value();
                    QVERIFY2(std::abs(shown - want) <= 1e-4 * std::max(1.0, std::abs(want)),
                             qPrintable(QStringLiteral("%1 shows %2, not %3").arg(label).arg(shown).arg(want)));
                }
            }
        }

        QWidget* comp = host(QStringLiteral("comp"))->panel();
        QVERIFY(std::abs(comp->findChild<ClientCompThresholdFader*>()->thresholdDb() - value("comp.threshold")) < 1e-3);
        QVERIFY(std::abs(makeupMeter(comp)->makeupDb() - value("comp.makeup")) < 1e-3);
        QCOMPARE(comp->findChild<QSlider*>(QStringLiteral("compCeilingSlider"))->value(),
                 int(std::lround(value("comp.ceiling") * 10.0)));
        QCOMPARE(comp->findChild<ClientCompLimiterButton*>()->isChecked(), value("comp.limiter") != 0.0);
        QWidget* tube = host(QStringLiteral("tube"))->panel();
        const int model = int(value("tube.model"));
        QCOMPARE(model, 1);
        QVERIFY(tube->findChild<QPushButton*>(QStringLiteral("tubeModelB"))->isChecked());
        QVERIFY(!tube->findChild<QPushButton*>(QStringLiteral("tubeModelA"))->isChecked());
        QCOMPARE(tube->findChild<QSlider*>(QStringLiteral("tubeDryWetSlider"))->value(),
                 int(std::lround(value("tube.mix") * 100.0)));
        QWidget* exciter = host(QStringLiteral("pudu"))->panel();
        QCOMPARE(value("pudu.mode"), 1.0);
        QVERIFY(buttonLabelled(exciter, QStringLiteral("Odd"))->isChecked());
        QVERIFY(!buttonLabelled(exciter, QStringLiteral("Even"))->isChecked());

        QCOMPARE(m_window->config().params, loaded);
        QVERIFY(!m_window->chainModified());
        const esa::Profile* saved = m_profiles->find(QStringLiteral("Everything"));
        QVERIFY(saved);
        for (auto it = chain.params.cbegin(); it != chain.params.cend(); ++it) {
            QVERIFY2(saved->chain.params.value(it.key()) == it.value(), qPrintable(it.key()));
        }
    }

    // Every control on these pages edits its own key, through to the chain: each knob typed
    // into, the threshold fader's field, LIMIT and the ceiling, the tube's models and its
    // Dry/Wet, the Exciter's Even and Odd.
    void everyControlOnTheCompTubeAndExciterPagesDrivesItsKey()
    {
        make();
        const auto reaches = [&](const QString& key, double expected) {
            QVERIFY2(std::abs(held(key) - expected) < 1e-9,
                     qPrintable(QStringLiteral("%1 holds %2, wanted %3").arg(key).arg(held(key)).arg(expected)));
            QCOMPARE(control(key)->value(), expected);
            QVERIFY2(std::abs(inChain(key) - expected) < 1e-3, qPrintable(key));
        };
        const QList<QPair<QString, QString>> pages = {{QStringLiteral("comp"), QStringLiteral("comp")},
                                                      {QStringLiteral("tube"), QStringLiteral("tube")},
                                                      {QStringLiteral("exciter"), QStringLiteral("pudu")}};
        for (const auto& [id, stage] : pages) {
            m_window->showPage(id);
            esa::StageHost* page = host(stage);
            for (const auto& [label, key] : page->spec().knobs) {
                if (key.isEmpty()) continue;
                const esa::ParamSpec* spec = esa::findParamSpec(key);
                QVERIFY2(spec, qPrintable(key));
                const double value = roundedTo(spec->min + 0.3 * (spec->max - spec->min), spec->decimals);
                QVERIFY2(std::abs(value - held(key)) > 1e-9, qPrintable(key));
                type(page->knob(key), value);
                reaches(key, value);
            }
        }

        m_window->showPage(QStringLiteral("comp"));
        QWidget* comp = host(QStringLiteral("comp"))->panel();
        auto* fader = comp->findChild<ClientCompThresholdFader*>();
        auto* faderField = fader->findChild<QLineEdit*>();
        QVERIFY(faderField);
        faderField->setFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(faderField->hasFocus());
        faderField->selectAll();
        QTest::keyClicks(faderField, QStringLiteral("-27.5"));
        QTest::keyClick(faderField, Qt::Key_Return);
        reaches(QStringLiteral("comp.threshold"), -27.5);
        auto* limiter = comp->findChild<ClientCompLimiterButton*>();
        QVERIFY(limiter->isChecked());
        QTest::mouseClick(limiter, Qt::LeftButton);
        reaches(QStringLiteral("comp.limiter"), 0.0);
        QTest::mouseClick(limiter, Qt::LeftButton);
        reaches(QStringLiteral("comp.limiter"), 1.0);
        comp->findChild<QSlider*>(QStringLiteral("compCeilingSlider"))->setValue(-35);
        reaches(QStringLiteral("comp.ceiling"), -3.5);

        m_window->showPage(QStringLiteral("tube"));
        QWidget* tube = host(QStringLiteral("tube"))->panel();
        QTest::mouseClick(tube->findChild<QPushButton*>(QStringLiteral("tubeModelC")), Qt::LeftButton);
        reaches(QStringLiteral("tube.model"), 2.0);
        QTest::mouseClick(tube->findChild<QPushButton*>(QStringLiteral("tubeModelA")), Qt::LeftButton);
        reaches(QStringLiteral("tube.model"), 0.0);
        tube->findChild<QSlider*>(QStringLiteral("tubeDryWetSlider"))->setValue(42);
        reaches(QStringLiteral("tube.mix"), 0.42);

        m_window->showPage(QStringLiteral("exciter"));
        QWidget* exciter = host(QStringLiteral("pudu"))->panel();
        QTest::mouseClick(buttonLabelled(exciter, QStringLiteral("Odd")), Qt::LeftButton);
        reaches(QStringLiteral("pudu.mode"), 1.0);
        QTest::mouseClick(buttonLabelled(exciter, QStringLiteral("Even")), Qt::LeftButton);
        reaches(QStringLiteral("pudu.mode"), 0.0);
        QVERIFY(m_window->chainModified());
    }

    // The done-when case for each page: a knob turned on it changes the settings, the control
    // in its exact-values window and the chain the audio runs through; and that control
    // turns the knob back, on the page on show or one shown later.
    void aKnobTurnedOnTheCompTubeOrExciterPageReachesEverything_data()
    {
        QTest::addColumn<QString>("page");
        QTest::addColumn<QString>("key");
        QTest::addColumn<double>("before");
        QTest::addColumn<double>("after");  // a tenth of the knob's travel up
        QTest::newRow("Compressor knee") << QStringLiteral("comp") << QStringLiteral("comp.knee") << 6.0 << 8.4;
        QTest::newRow("Tube drive") << QStringLiteral("tube") << QStringLiteral("tube.drive") << 0.0 << 2.4;
        QTest::newRow("Exciter air") << QStringLiteral("exciter") << QStringLiteral("pudu.dooHarmonics") << 6.0 << 8.4;
    }
    void aKnobTurnedOnTheCompTubeOrExciterPageReachesEverything()
    {
        QFETCH(QString, page);
        QFETCH(QString, key);
        QFETCH(double, before);
        QFETCH(double, after);
        make();
        m_window->showPage(page);
        const QString stage = esa::StageBinding::stageOf(key);
        esa::StageHost* host = this->host(stage);
        ClientCompKnob* knob = host->knob(key);
        QVERIFY(knob);
        QCOMPARE(knob->value(), float(before));
        drag(knob, -20);  // a tenth of its 200 px travel

        QCOMPARE(held(key), after);
        esa::ParamControl* exact = control(key);
        QCOMPARE(exact->value(), after);
        auto* details = m_window->findChild<esa::DetailsWindow*>(QStringLiteral("details_") + stage);
        QVERIFY(details && details->controls().contains(exact));
        QVERIFY(std::abs(inChain(key) - after) < 1e-4);
        QVERIFY(m_window->chainModified());

        // From the exact-values window, the page on show follows.
        exact->setValue(before + 1.0);
        QTRY_COMPARE(knob->value(), float(before + 1.0));
        QVERIFY(std::abs(inChain(key) - (before + 1.0)) < 1e-4);
        // And a page not on show is right once it is shown: the Tube's on its first tick.
        m_window->showPage(QStringLiteral("radio"));
        exact->setValue(before + 2.0);
        QCoreApplication::processEvents();
        m_window->showPage(page);
        QTRY_COMPARE(knob->value(), float(before + 2.0));
    }

    // The two controls the panels leave out of their own redraws, LIMIT and the Exciter's
    // Even and Odd, follow their exact-values controls too.
    void theButtonsFollowTheExactValuesWindow()
    {
        make();
        m_window->showPage(QStringLiteral("comp"));
        auto* limiter = host(QStringLiteral("comp"))->panel()->findChild<ClientCompLimiterButton*>();
        QVERIFY(limiter && limiter->isChecked());
        control(QStringLiteral("comp.limiter"))->setValue(0.0);
        QTRY_VERIFY(!limiter->isChecked());
        m_window->showPage(QStringLiteral("radio"));
        control(QStringLiteral("comp.limiter"))->setValue(1.0);
        QCoreApplication::processEvents();
        m_window->showPage(QStringLiteral("comp"));
        QVERIFY(limiter->isChecked());

        m_window->showPage(QStringLiteral("exciter"));
        QWidget* exciter = host(QStringLiteral("pudu"))->panel();
        QVERIFY(buttonLabelled(exciter, QStringLiteral("Even"))->isChecked());
        control(QStringLiteral("pudu.mode"))->setValue(1.0);
        QTRY_VERIFY(buttonLabelled(exciter, QStringLiteral("Odd"))->isChecked());
        QVERIFY(!buttonLabelled(exciter, QStringLiteral("Even"))->isChecked());
        QVERIFY(std::abs(inChain(QStringLiteral("pudu.mode")) - 1.0) < 1e-9);
        QCOMPARE(binding()->firstMismatch(), QString());
    }

    // The makeup guard works the handle's row out as the meter paints it: wherever makeup
    // is, the row it gives is where the meter draws the handle's amber bar.
    void theMakeupGuardFindsTheHandleWhereTheMeterDrawsIt()
    {
        make();
        m_window->showPage(QStringLiteral("comp"));
        ClientCompMeter* meter = makeupMeter(host(QStringLiteral("comp"))->panel());
        QVERIFY(meter);
        auto* guard = meter->findChild<esa::MakeupHitGuard*>();
        QVERIFY(guard);
        // No limiter, so no ceiling line in the same amber.
        control(QStringLiteral("comp.limiter"))->setValue(0.0);
        const QColor amber = ThemeManager::instance().color(meter, QStringLiteral("color.accent.warning"));
        for (double db : {-9.0, -3.0, 0.0, 6.0, 15.0, 21.0}) {
            control(QStringLiteral("comp.makeup"))->setValue(db);
            QTRY_COMPARE(meter->makeupDb(), float(db));
            QTest::qWait(40);  // the meter's tick, which takes the limiter's overlay away
            const QImage shot = meter->grab().toImage().convertToFormat(QImage::Format_RGB32);
            const qreal dpr = shot.devicePixelRatio();
            // The rows, in the meter's own pixels, that are amber across most of the bar.
            QList<int> rows;
            for (int y = 0; y < shot.height(); ++y) {
                const auto* line = reinterpret_cast<const QRgb*>(shot.constScanLine(y));
                int count = 0;
                for (int x = 0; x < shot.width(); ++x) {
                    if (std::abs(qRed(line[x]) - amber.red()) <= 24 && std::abs(qGreen(line[x]) - amber.green()) <= 24
                        && std::abs(qBlue(line[x]) - amber.blue()) <= 24) {
                        ++count;
                    }
                }
                const int row = int(y / dpr);
                if (count >= 14 * dpr && !rows.contains(row)) rows << row;
            }
            const int at = guard->handleY();
            QVERIFY2(rows.contains(at) && rows.contains(at + 1),
                     qPrintable(QStringLiteral("makeup %1 dB: handle at %2, amber rows %3")
                                    .arg(db).arg(at).arg(QStringList([&] {
                                        QStringList out;
                                        for (int r : rows) out << QString::number(r);
                                        return out;
                                    }()).join(u' '))));
            QVERIFY2(!rows.contains(at - 3) && !rows.contains(at + 4), qPrintable(QString::number(db)));
        }
    }

    // One press near the top of the compressor's Out meter jumped makeup to +24 dB. The
    // handle now takes a press only within 10 px of itself; dragging it, the wheel, the keys
    // and a double-click work as before.
    void theMakeupHandleTakesOnlyAPressNearIt()
    {
        make();
        m_window->showPage(QStringLiteral("comp"));
        ClientCompMeter* meter = makeupMeter(host(QStringLiteral("comp"))->panel());
        QVERIFY(meter);
        auto* guard = meter->findChild<esa::MakeupHitGuard*>();
        QVERIFY(guard);
        // Fixed for hearing safety: a press at the edge of the reach moves makeup by well under
        // a dB, where 40 px would let one click jump it by about 3 dB.
        QCOMPARE(esa::MakeupHitGuard::kReachPx, 10);
        const QString key = QStringLiteral("comp.makeup");
        QCOMPARE(held(key), 0.0);
        // Drawn, as it always is before a hand can reach it: the meter reads a press against
        // the bar it last drew.
        meter->grab();
        const int x = meter->width() / 2;
        const int at = guard->handleY();
        QVERIFY2(at - 150 > 20 && at + 100 < meter->height() - 40, qPrintable(QString::number(at)));

        // A hundred pixels above it, then below it: nothing, and no drag starts either.
        QSignalSpy sent(control(key), &esa::ParamControl::valueChanged);
        mouse(meter, QEvent::MouseButtonPress, QPoint(x, at - 100));
        mouse(meter, QEvent::MouseMove, QPoint(x, at - 150));
        mouse(meter, QEvent::MouseMove, QPoint(x, at));
        mouse(meter, QEvent::MouseButtonRelease, QPoint(x, at));
        mouse(meter, QEvent::MouseButtonPress, QPoint(x, at + 100));
        mouse(meter, QEvent::MouseButtonRelease, QPoint(x, at + 100));
        // Just beyond its reach.
        mouse(meter, QEvent::MouseButtonPress, QPoint(x, at - esa::MakeupHitGuard::kReachPx - 1));
        mouse(meter, QEvent::MouseButtonRelease, QPoint(x, at - esa::MakeupHitGuard::kReachPx - 1));
        QCOMPARE(sent.count(), 0);
        QCOMPARE(meter->makeupDb(), 0.0f);
        QCOMPARE(held(key), 0.0);

        // On it: it moves by less than half a dB, the pixel the press fell in.
        mouse(meter, QEvent::MouseButtonPress, QPoint(x, at));
        QVERIFY2(std::abs(held(key)) < 0.5, qPrintable(QString::number(held(key))));
        // Dragged, it follows the pointer, and the chain with it.
        mouse(meter, QEvent::MouseMove, QPoint(x, at - 100));
        mouse(meter, QEvent::MouseButtonRelease, QPoint(x, at - 100));
        QVERIFY2(held(key) > 5.0, qPrintable(QString::number(held(key))));
        QVERIFY(std::abs(guard->handleY() - (at - 100)) <= 1);
        QVERIFY(std::abs(inChain(key) - held(key)) < 1e-4);
        // Within its reach of where it is now, a press takes it again.
        const int now = guard->handleY();
        mouse(meter, QEvent::MouseButtonPress, QPoint(x, now + esa::MakeupHitGuard::kReachPx));
        mouse(meter, QEvent::MouseMove, QPoint(x, at));
        mouse(meter, QEvent::MouseButtonRelease, QPoint(x, at));
        QVERIFY2(std::abs(held(key)) < 0.5, qPrintable(QString::number(held(key))));

        // A double-click anywhere on it is 0 dB, as upstream's: its presses are taken, the
        // double-click is not.
        control(key)->setValue(9.0);
        QTRY_COMPARE(meter->makeupDb(), 9.0f);
        mouse(meter, QEvent::MouseButtonPress, QPoint(x, guard->handleY() + 120));
        mouse(meter, QEvent::MouseButtonRelease, QPoint(x, guard->handleY() + 120));
        mouse(meter, QEvent::MouseButtonDblClick, QPoint(x, guard->handleY() + 120));
        mouse(meter, QEvent::MouseButtonRelease, QPoint(x, guard->handleY() + 120));
        QCOMPARE(held(key), 0.0);
        // The wheel and the keys: half a dB a step.
        wheel(meter, 120);
        QCOMPARE(held(key), 0.5);
        QTest::keyClick(meter, Qt::Key_Up);
        QCOMPARE(held(key), 1.0);
        QTest::keyClick(meter, Qt::Key_Home);
        QCOMPARE(held(key), 0.0);
        // Another button does nothing, as upstream's.
        mouse(meter, QEvent::MouseButtonPress, QPoint(x, 30), Qt::RightButton);
        mouse(meter, QEvent::MouseButtonRelease, QPoint(x, 30), Qt::RightButton);
        QCOMPARE(held(key), 0.0);
        QVERIFY(std::abs(inChain(key)) < 1e-4);
    }

    // The Exciter draws "AetherVoice", not upstream's "AetherVoice™", and its Even and Odd
    // buttons describe what they do without naming whose hardware each follows.
    void theExciterSaysAetherVoiceAndNamesNoCompany()
    {
        const QString mark = QString::fromUtf8(esa::kExciterWordmark);
        QCOMPARE(mark, QStringLiteral("AetherVoice"));
        make();
        m_window->showPage(QStringLiteral("exciter"));
        QWidget* panel = host(QStringLiteral("pudu"))->panel();
        auto* logo = panel->findChild<PooDooLogo*>();
        QVERIFY(logo && logo->isVisible() && logo->width() > 0);
        QCOMPARE(logo->accessibleName(), mark);
        // What it draws is the mark set afresh, and not upstream's.
        const QImage shown = logo->grab().toImage();
        logo->setWordmark(QString::fromUtf8("AetherVoice\xe2\x84\xa2"));
        const QImage upstreams = logo->grab().toImage();
        logo->setWordmark(QStringLiteral("AetherVoice"));
        const QImage ours = logo->grab().toImage();
        QVERIFY(shown == ours);
        QVERIFY(shown != upstreams);

        QPushButton* even = buttonLabelled(panel, QStringLiteral("Even"));
        QPushButton* odd = buttonLabelled(panel, QStringLiteral("Odd"));
        QVERIFY(even && odd);
        QVERIFY(even->toolTip().startsWith(QStringLiteral("Even: ")));
        QVERIFY(odd->toolTip().startsWith(QStringLiteral("Odd: ")));
        for (QWidget* w : panel->findChildren<QWidget*>()) {
            for (const QString& text : {w->toolTip(), w->accessibleName(), w->accessibleDescription()}) {
                for (const char* name : {"Aphex", "Behringer", "Big Bottom", "\xe2\x84\xa2"}) {
                    QVERIFY2(!text.contains(QString::fromUtf8(name), Qt::CaseInsensitive), qPrintable(text));
                }
            }
        }
    }

    // A double-click on an Exciter knob lands on upstream's default, which for Body drive and
    // the mixes is not this app's: the defaults stay ours, so a setting nobody moved keeps
    // its sound. The knob's default is upstream's to choose; this marks the difference.
    void anExciterKnobDoubleClickedLandsOnUpstreamsDefault()
    {
        make();
        m_window->showPage(QStringLiteral("exciter"));
        esa::StageHost* page = host(QStringLiteral("pudu"));
        const QList<QPair<QString, double>> knobs = {{QStringLiteral("pudu.pooDrive"), 6.0},
                                                     {QStringLiteral("pudu.pooMix"), 0.3},
                                                     {QStringLiteral("pudu.dooMix"), 0.3}};
        for (const auto& [key, upstream] : knobs) {
            const esa::ParamSpec* spec = esa::findParamSpec(key);
            QVERIFY(spec && std::abs(spec->def - upstream) > 0.1);
            QCOMPARE(held(key), spec->def);
            ClientCompKnob* knob = page->knob(key);
            const QPoint centre = knob->rect().center();
            mouse(knob, QEvent::MouseButtonDblClick, centre);
            mouse(knob, QEvent::MouseButtonRelease, centre);
            QCOMPARE(held(key), upstream);
            QVERIFY(std::abs(inChain(key) - upstream) < 1e-4);
        }
    }

    // ── The AetherNR page ─────────────────────────────────────────────────────

    // AetherSDR's tabs: a click on an unlit tab runs that method, and a click on the lit one
    // turns NR off. The rail's box, the settings and the engine follow, and the page keeps
    // showing the method that ran.
    void theNrTabsSelectAndClearTheMethod()
    {
        make();
        QObject receiver;  // gone with this test, and its connection with it
        esa::EngineStatus status;
        connect(m_core.get(), &esa::Engine::statusChanged, &receiver,
                [&status](const esa::EngineStatus& s) { status = s; });
        m_window->showPage(QStringLiteral("nr"));
        esa::NrPage* page = nrPage();
        QVERIFY(page && page->isVisible());
        QCOMPARE(page->objectName(), QStringLiteral("page_nr"));
        auto* running = page->findChild<QLabel*>(QStringLiteral("StatusValue"));
        QVERIFY(running);
        QVERIFY(litNrTabs().isEmpty());
        QCOMPARE(running->text(), QStringLiteral("No method running"));

        nrTab(QStringLiteral("rn2"))->click();
        QCOMPARE(m_window->config().nrMethod, QStringLiteral("rn2"));
        QCOMPARE(litNrTabs(), QStringList{QStringLiteral("rn2")});
        QCOMPARE(page->shownMethod(), QStringLiteral("rn2"));
        QVERIFY(page->findChild<QWidget*>(QStringLiteral("nrMethod_rn2"))->isVisible());
        QVERIFY(railBox(QStringLiteral("AetherNR"))->isChecked());
        QCOMPARE(running->text(), esa::findNrMethod(QStringLiteral("rn2"))->label);
        QTRY_COMPARE(status.nrMethod, QStringLiteral("rn2"));
        QVERIFY(m_window->chainModified());

        // Another unlit tab runs that method instead.
        nrTab(QStringLiteral("nr2"))->click();
        QCOMPARE(m_window->config().nrMethod, QStringLiteral("nr2"));
        QCOMPARE(litNrTabs(), QStringList{QStringLiteral("nr2")});
        QCOMPARE(page->shownMethod(), QStringLiteral("nr2"));
        QVERIFY(page->findChild<QWidget*>(QStringLiteral("nrMethod_nr2"))->isVisible());
        QTRY_COMPARE(status.nrMethod, QStringLiteral("nr2"));

        // The lit one turns NR off, and its parameters stay on show.
        nrTab(QStringLiteral("nr2"))->click();
        QCOMPARE(m_window->config().nrMethod, QString());
        QVERIFY(litNrTabs().isEmpty());
        QCOMPARE(page->shownMethod(), QStringLiteral("nr2"));
        QVERIFY(page->findChild<QWidget*>(QStringLiteral("nrMethod_nr2"))->isVisible());
        QVERIFY(!railBox(QStringLiteral("AetherNR"))->isChecked());
        QCOMPARE(running->text(), QStringLiteral("No method running"));
        QCOMPARE(m_settings->lastNrMethod(), QStringLiteral("nr2"));
        QTRY_COMPARE(status.nrMethod, QString());
        QVERIFY2(!m_window->chainModified(), "back to the factory settings, which run no NR");
    }

    // A method this build or this computer cannot run keeps its tab, greyed, saying why, and
    // a click on it does nothing.
    void unavailableNrTabsSayWhy()
    {
        make();
        m_window->showPage(QStringLiteral("nr"));
        QCOMPARE(esa::NrPage::tabIds().mid(0, 7),
                 (QStringList{QStringLiteral("nr2"), QStringLiteral("nr4"), QStringLiteral("mnr"), QStringLiteral("dfnr"),
                              QStringLiteral("rn2"), QStringLiteral("bnr"), QStringLiteral("nnr")}));
        const QStringList reasons{QStringLiteral("Not built into this version"), QStringLiteral("Needs macOS"),
                                  QStringLiteral("Unsafe on a virtual Mac"), QStringLiteral("Not available in this build")};
        int unavailable = 0;
        for (const QString& id : esa::NrPage::tabIds()) {
            QPushButton* tab = nrTab(id);
            QVERIFY2(tab, qPrintable(id));
            QVERIFY(tab->isVisible());
            QCOMPARE(tab->text(), id.toUpper());
            const QString reason = esa::nrMethodUnavailableReason(id);
            QCOMPARE(reason.isEmpty(), esa::findNrMethod(id) != nullptr);
            QCOMPARE(tab->isEnabled(), reason.isEmpty());
            if (reason.isEmpty()) continue;
            ++unavailable;
            QVERIFY2(reasons.contains(reason), qPrintable(reason));
            QCOMPARE(tab->toolTip(), reason);
            QCOMPARE(tab->accessibleDescription(), reason);
            QTest::mouseClick(tab, Qt::LeftButton);
            QVERIFY(!tab->isChecked());
            QCOMPARE(m_window->config().nrMethod, QString());
        }
        QVERIFY(unavailable >= 1);
        QCOMPARE(nrTab(QStringLiteral("bnr"))->toolTip(), QStringLiteral("Not available in this build"));
#ifndef __APPLE__
        QCOMPARE(nrTab(QStringLiteral("mnr"))->toolTip(), QStringLiteral("Needs macOS"));
#endif
#ifndef HAVE_DFNR
        QCOMPARE(nrTab(QStringLiteral("dfnr"))->toolTip(), QStringLiteral("Not built into this version"));
#endif
        QVERIFY(litNrTabs().isEmpty());
    }

    // While NR is off the page shows the method that ran last (nr/lastMethod), which the
    // rail's AetherNR box brings back, here and in a later session. A profile that runs a
    // method counts as running it.
    void theNrPageShowsTheLastMethodWhileOff()
    {
        esa::Settings(m_dir.filePath(QStringLiteral("aether.ini"))).setLastNrMethod(QStringLiteral("rn2"));
        make();
        m_window->showPage(QStringLiteral("nr"));
        QVERIFY(litNrTabs().isEmpty());
        QCOMPARE(nrPage()->shownMethod(), QStringLiteral("rn2"));
        QVERIFY(nrPage()->findChild<QWidget*>(QStringLiteral("nrMethod_rn2"))->isVisible());

        QCheckBox* box = railBox(QStringLiteral("AetherNR"));
        box->click();
        QCOMPARE(m_window->config().nrMethod, QStringLiteral("rn2"));
        QCOMPARE(litNrTabs(), QStringList{QStringLiteral("rn2")});
        nrTab(QStringLiteral("nr2"))->click();
        box->click();
        QCOMPARE(m_window->config().nrMethod, QString());
        QVERIFY(litNrTabs().isEmpty());
        QCOMPARE(nrPage()->shownMethod(), QStringLiteral("nr2"));
        QCOMPARE(m_settings->lastNrMethod(), QStringLiteral("nr2"));

        // A profile that runs RN2, then the factory settings, which run none: RN2 ran last.
        esa::ChainSettings chain;
        chain.nrMethod = QStringLiteral("rn2");
        QVERIFY(m_profiles->save(QStringLiteral("Voice"), chain));
        m_window->loadProfile(QStringLiteral("Voice"));
        QCOMPARE(litNrTabs(), QStringList{QStringLiteral("rn2")});
        QCOMPARE(nrPage()->shownMethod(), QStringLiteral("rn2"));
        m_window->loadProfile(QString());
        QVERIFY(litNrTabs().isEmpty());
        QCOMPARE(nrPage()->shownMethod(), QStringLiteral("rn2"));
        QCOMPARE(m_settings->lastNrMethod(), QStringLiteral("rn2"));

        // The next session.
        make();
        m_window->showPage(QStringLiteral("nr"));
        QVERIFY(litNrTabs().isEmpty());
        QCOMPARE(nrPage()->shownMethod(), QStringLiteral("rn2"));
        railBox(QStringLiteral("AetherNR"))->click();
        QCOMPARE(m_window->config().nrMethod, QStringLiteral("rn2"));
        QCOMPARE(litNrTabs(), QStringList{QStringLiteral("rn2")});
    }

    // The table's display hints: the noise fill's three sliders work only with the fill on,
    // NR2's controls sit under AetherSDR's headings in its order, and a percent value shows
    // as 0-100 %.
    void theNrParametersFollowTheirHints()
    {
        make();
        m_window->showPage(QStringLiteral("nr"));
        const QStringList fill{QStringLiteral("nr2.post2Level"), QStringLiteral("nr2.post2Factor"),
                               QStringLiteral("nr2.post2Taper")};
        const auto fillUsable = [&](bool usable) {
            for (const QString& key : fill) {
                esa::ParamControl* c = control(key);
                QVERIFY2(c, qPrintable(key));
                QCOMPARE(c->spec().enabledByKey, QStringLiteral("nr2.post2"));
                QCOMPARE(c->isEnabled(), usable);
                QCOMPARE(c->widgets().size(), 3);  // label, slider, reading
                for (QWidget* w : c->widgets()) QCOMPARE(w->isEnabled(), usable);
            }
        };
        fillUsable(false);  // the fill is off by default
        control(QStringLiteral("nr2.post2"))->setValue(1);
        fillUsable(true);
        control(QStringLiteral("nr2.post2"))->setValue(0);
        fillUsable(false);
        // A profile load sets the controls without a click, and they follow all the same.
        esa::ChainSettings chain;
        chain.params.insert(QStringLiteral("nr2.post2"), 1.0);
        QVERIFY(m_profiles->save(QStringLiteral("Fill"), chain));
        m_window->loadProfile(QStringLiteral("Fill"));
        fillUsable(true);
        m_window->loadProfile(QString());
        fillUsable(false);
        for (const esa::ParamSpec& spec : esa::findNrMethod(QStringLiteral("nr2"))->params) {
            if (!fill.contains(spec.key)) QVERIFY2(control(spec.key)->isEnabled(), qPrintable(spec.key));
        }

        // NR2's frames, under AetherSDR's headings and in its order: the pickers, the
        // switches, then the sliders.
        QWidget* nr2 = nrPage()->findChild<QWidget*>(QStringLiteral("nrMethod_nr2"));
        QVERIFY(nr2);
        QStringList headings;
        for (QLabel* label : nr2->findChildren<QLabel*>(QStringLiteral("SectionLabel"))) headings << label->text();
        QCOMPARE(headings, (QStringList{QStringLiteral("GAIN METHOD"), QStringLiteral("NOISE ESTIMATION"),
                                        QStringLiteral("POST PROCESSING"), QStringLiteral("MASK")}));

        // A percent hint: 0..1 held and sent, 0..100 % shown and typed.
        esa::ParamSpec mixSpec{QStringLiteral("test.mix"), QStringLiteral("Mix"), 0.0, 1.0, 0.5, 2};
        mixSpec.percent = true;
        QWidget host;
        auto* mix = new esa::ParamControl(mixSpec, 0.25, new QGridLayout(&host), 0, &host);
        QDoubleSpinBox* field = nullptr;
        for (QWidget* w : mix->widgets()) {
            if (auto* spin = qobject_cast<QDoubleSpinBox*>(w)) field = spin;
        }
        QVERIFY(field);
        QCOMPARE(field->minimum(), 0.0);
        QCOMPARE(field->maximum(), 100.0);
        QCOMPARE(field->decimals(), 0);
        QCOMPARE(field->value(), 25.0);
        QCOMPARE(field->suffix(), QStringLiteral(" %"));
        QSignalSpy sent(mix, &esa::ParamControl::valueChanged);
        field->setValue(42);
        QCOMPARE(mix->value(), 0.42);
        QCOMPARE(sent.count(), 1);
        QCOMPARE(sent.at(0).at(1).toDouble(), 0.42);
        mix->setValue(0.371);
        QCOMPARE(mix->value(), 0.37);
        QCOMPARE(field->value(), 37.0);
        // MNR's strength is one, where there is MNR (macOS).
        if (esa::findNrMethod(QStringLiteral("mnr"))) {
            esa::ParamControl* strength = control(QStringLiteral("mnr.strength"));
            QVERIFY(strength && strength->spec().percent);
            QDoubleSpinBox* shown = nullptr;
            for (QWidget* w : strength->widgets()) {
                if (auto* spin = qobject_cast<QDoubleSpinBox*>(w)) shown = spin;
            }
            QVERIFY(shown);
            QCOMPARE(shown->value(), 100.0);
            QCOMPARE(shown->maximum(), 100.0);
            QCOMPARE(shown->suffix(), QStringLiteral(" %"));
            shown->setValue(60);
            QCOMPARE(held(QStringLiteral("mnr.strength")), 0.6);
        }
    }

    // Reset puts the method on show back to its defaults, each through its control, and
    // leaves every other method alone.
    void nrResetPutsTheShownMethodBack()
    {
        make();
        m_window->showPage(QStringLiteral("nr"));
        QCOMPARE(nrPage()->shownMethod(), QStringLiteral("nr2"));
        const esa::NrMethod* nr2 = esa::findNrMethod(QStringLiteral("nr2"));
        for (const esa::ParamSpec& spec : nr2->params) control(spec.key)->setValue(nonDefault(spec));
        control(QStringLiteral("rn2.dry"))->setValue(0.4);
        QVERIFY(m_window->chainModified());

        auto* reset = nrPage()->findChild<QWidget*>(QStringLiteral("nrMethod_nr2"))->findChild<QPushButton*>(QStringLiteral("nrReset"));
        QVERIFY(reset && reset->isVisible());
        reset->click();
        for (const esa::ParamSpec& spec : nr2->params) {
            QCOMPARE(control(spec.key)->value(), spec.def);
            QCOMPARE(held(spec.key), spec.def);
        }
        QCOMPARE(held(QStringLiteral("rn2.dry")), 0.4);
        control(QStringLiteral("rn2.dry"))->setValue(0.0);
        QVERIFY(!m_window->chainModified());
    }

    // The status strip's NR GAIN trace draws the gain of the method running from the engine's
    // taps, on the panels' 16 ms cadence while the page is on screen, and that timer stops
    // while it is not. Turning NR off sends the radio nothing.
    void theNrGainTraceRunsWhileThePageIsShown()
    {
        MockExpert radio;
        QVERIFY(radio.listen());
        esa::AppConfig config;
        config.nrMethod = QStringLiteral("nr2");
        make(config);
        auto* feed = m_window->findChild<QTimer*>(QStringLiteral("nrGainFeed"));
        QVERIFY(feed);
        QCOMPARE(feed->interval(), 16);
        QVERIFY(m_window->currentPage() != QLatin1String("nr"));
        QVERIFY(!feed->isActive());

        esa::DiscoveredRadio picked;
        picked.host = QStringLiteral("127.0.0.1");
        picked.port = radio.port();
        m_window->connectToRadio(picked);
        QTRY_VERIFY_WITH_TIMEOUT(radio.audioStartCount() == 1, 6000);
        const std::shared_ptr<esa::LiveTaps> taps = m_core->taps();
        QTRY_VERIFY_WITH_TIMEOUT(taps->nrActive.load(), 10000);  // NR2 built and running

        m_window->showPage(QStringLiteral("nr"));
        QVERIFY(feed->isActive());
        auto* strip = nrPage()->findChild<AetherSDR::NrGainStrip*>(QStringLiteral("nrGainStrip"));
        QVERIFY(strip && strip->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(tracedPixels(strip) > 20, 3000);
        QTest::qWait(300);  // a few dozen steps of the sweep
        const QString shots = qEnvironmentVariable("ESA_GUI_SHOT_DIR");
        if (!shots.isEmpty()) QVERIFY(m_window->grab().save(QDir(shots).filePath(QStringLiteral("window-nr-running.png"))));

        m_window->showPage(QStringLiteral("gate"));
        QVERIFY(!feed->isActive());
        m_window->showPage(QStringLiteral("nr"));
        QVERIFY(feed->isActive());

        // Off: the engine runs no method, and the trace falls to its idle line.
        QTest::qWait(300);  // the link settles
        const qsizetype commandsBefore = radio.commands.size();
        nrTab(QStringLiteral("nr2"))->click();
        QTRY_VERIFY(!taps->nrActive.load());
        QTRY_COMPARE(tracedPixels(strip), 0);
        QTest::qWait(300);
        QVERIFY2(radio.commands.size() == commandsBefore, qPrintable(radio.commands.mid(commandsBefore).join(u' ')));

        // Value by value: with the engine stopped, only this test writes the taps. The newest
        // samples are at the right-hand end, just before the sweep cursor, which the last
        // few columns hold.
        m_core->shutdown();
        const int newest = strip->width() - 4;
        taps->nrGain.store(0.9f);
        taps->nrActive.store(true);
        QTRY_VERIFY(tracePixels(strip, 0, newest, true, true) > 10);  // speech: bright, high
        taps->nrGain.store(0.1f);
        QTRY_VERIFY(tracePixels(strip, newest - 12, newest, true, true) == 0
                    && tracePixels(strip, newest - 12, newest) > 0);  // the floor: dim, low
        // Off the screen, the page stops reading them.
        m_window->showPage(QStringLiteral("gate"));
        QVERIFY(!feed->isActive());
        QVERIFY(!strip->isVisible());
    }

    // The foot of the page: the auto-bypass box reaches the settings and the engine; the line
    // under it says what NR is doing and is hidden with nothing to say; the strip's dot is grey
    // with nothing running, green while a method runs, amber while it waits or is passed by,
    // and red when it failed; and another method starts the trace afresh.
    void theNrPageFootSaysWhatNrIsDoing()
    {
        MockExpert radio;
        QVERIFY(radio.listen());
        radio.modulation = QStringLiteral("CW");
        esa::AppConfig config;
        config.nrMethod = QStringLiteral("nr2");
        make(config);
        QObject receiver;  // gone with this test, and its connection with it
        esa::EngineStatus status;
        connect(m_core.get(), &esa::Engine::statusChanged, &receiver,
                [&status](const esa::EngineStatus& s) { status = s; });
        m_window->showPage(QStringLiteral("nr"));
        esa::NrPage* page = nrPage();
        QVERIFY(page);
        auto* autoBypass = page->findChild<QCheckBox*>(QStringLiteral("nrAutoBypass"));
        auto* line = page->findChild<QLabel*>(QStringLiteral("nrStatus"));
        auto* dot = page->findChild<QLabel*>(QStringLiteral("StatusDot"));
        auto* strip = page->findChild<AetherSDR::NrGainStrip*>(QStringLiteral("nrGainStrip"));
        QVERIFY(autoBypass && line && dot && strip);
        QVERIFY(autoBypass->isChecked());
        QVERIFY(m_window->config().nrAutoBypass);
        QVERIFY(m_window->saveProfileAs(QStringLiteral("NR2")));
        // The dot's colour, as drawn, against a theme colour.
        const auto dotIs = [&](const char* token) {
            const QImage shot = dot->grab().toImage().convertToFormat(QImage::Format_RGB32);
            const QColor drawn = shot.pixelColor(shot.width() / 2, shot.height() / 2);
            const QColor want = ThemeManager::instance().color(dot, QLatin1String(token));
            return std::abs(drawn.red() - want.red()) <= 8 && std::abs(drawn.green() - want.green()) <= 8
                   && std::abs(drawn.blue() - want.blue()) <= 8;
        };

        // On CW, with the box ticked, NR2 is passed by: amber, and the line says why.
        esa::DiscoveredRadio picked;
        picked.host = QStringLiteral("127.0.0.1");
        picked.port = radio.port();
        m_window->connectToRadio(picked);
        QTRY_VERIFY_WITH_TIMEOUT(status.audioFlowing && status.nrBypassedByMode, 10000);
        QTRY_VERIFY(line->isVisible() && line->text().contains(QLatin1String("Bypassed")));
        QVERIFY(dotIs("color.accent.warning"));

        // Unticked: the setting, a change to the profile, and NR2 runs on CW too, in green.
        autoBypass->click();
        QVERIFY(!m_window->config().nrAutoBypass);
        QVERIFY(m_window->chainModified());
        QTRY_VERIFY_WITH_TIMEOUT(!status.nrBypassedByMode && !status.nrPreparing && m_core->taps()->nrActive.load(),
                                 10000);
        QTRY_VERIFY(line->isVisible() && line->text().contains(QLatin1String("Active")));
        QTRY_VERIFY(dotIs("color.accent.success"));
        // Ticked again: passed by again, and nothing left to save.
        autoBypass->click();
        QVERIFY(m_window->config().nrAutoBypass);
        QVERIFY(!m_window->chainModified());
        QTRY_VERIFY_WITH_TIMEOUT(status.nrBypassedByMode, 10000);
        QTRY_VERIFY(dotIs("color.accent.warning"));

        // With the engine stopped, only this test says what NR is doing.
        m_core->shutdown();
        QTest::qWait(150);
        esa::EngineStatus told;
        told.nrMethod = QStringLiteral("nr2");
        told.nrFailed = true;
        page->showStatus(told);
        QVERIFY(line->isVisible());
        QCOMPARE(line->text(), esa::statusText::nr(told));
        QVERIFY(dotIs("color.accent.danger"));
        told.nrFailed = false;
        told.nrPreparing = true;
        page->showStatus(told);
        QVERIFY(line->isVisible() && line->text().contains(QLatin1String("Preparing")));
        QVERIFY(dotIs("color.accent.warning"));
        told.nrPreparing = false;
        page->showStatus(told);
        QVERIFY(line->isHidden());  // nothing to say: no audio, nothing failing or waiting
        QVERIFY(dotIs("color.accent.success"));

        // The trace of one method says nothing about the next: it starts afresh. Speech, drawn
        // bright and high, then the new method's first silence, low: none of the speech is left.
        const std::shared_ptr<esa::LiveTaps> taps = m_core->taps();
        const auto speech = [&] { return tracePixels(strip, 0, strip->width(), true, true); };
        taps->nrGain.store(0.9f);
        taps->nrActive.store(true);
        QTRY_VERIFY(speech() > 10);
        taps->nrGain.store(0.0f);
        page->showMethod(QStringLiteral("rn2"));
        QCOMPARE(speech(), 0);
        QTest::qWait(100);
        QCOMPARE(speech(), 0);
        page->showMethod(QString());
        QVERIFY(dotIs("color.text.label"));  // nothing running: grey
    }

    // At the window's narrowest the status strip still fits the window, with the trace at
    // upstream's width, and the name of what runs ends in "…" there rather than being sliced off.
    void theNrStatusStripFitsTheNarrowestWindow()
    {
        make();
        m_window->showPage(QStringLiteral("nr"));
        m_window->resize(1, 400);  // as narrow as it goes
        QTRY_VERIFY(m_window->width() < 700);
        QTest::qWait(50);
        auto* strip = nrPage()->findChild<AetherSDR::NrGainStrip*>(QStringLiteral("nrGainStrip"));
        auto* running = nrPage()->findChild<QLabel*>(QStringLiteral("StatusValue"));
        QVERIFY(strip && running);
        // The trace, the last thing on the strip's row, within the strip and the window: a row
        // with less room than its parts need lays them out past its own edge.
        QWidget* frame = strip->parentWidget();
        const auto onWindow = [&](QWidget* w) { return QRect(w->mapTo(m_window.get(), QPoint()), w->size()); };
        QVERIFY2(m_window->rect().contains(onWindow(frame)) && onWindow(frame).contains(onWindow(strip)),
                 qPrintable(QStringLiteral("window %1 px wide, strip from %2 to %3, trace from %4 to %5")
                                .arg(m_window->width()).arg(onWindow(frame).left()).arg(onWindow(frame).right())
                                .arg(onWindow(strip).left()).arg(onWindow(strip).right())));
        QVERIFY(strip->width() >= 120);  // upstream's least
        // Still the whole name for a screen reader, whatever fits on screen.
        QCOMPARE(running->text(), QStringLiteral("No method running"));
        // On screen, where the name does not fit, it is what a label of the same look shows of
        // the name cut short with "…", not the name with its end sliced off.
        const QString elided = running->fontMetrics().elidedText(running->text(), Qt::ElideRight,
                                                                 running->contentsRect().width());
        QLabel twin(elided, frame);
        twin.setObjectName(running->objectName());
        twin.setGeometry(running->geometry());
        twin.show();
        QCoreApplication::processEvents();
        QVERIFY2(running->grab().toImage() == twin.grab().toImage(),
                 qPrintable(QStringLiteral("%1 px for \"%2\", shown as \"%3\"")
                                .arg(running->contentsRect().width()).arg(running->text(), elided)));
    }

    // The method tabs share their row evenly. At the window's narrowest every label still fits
    // the room its tab gives it, where the style cuts a label off; DFNR once lost both ends.
    // At the reference width each tab, lit or not, is drawn on exactly the pixels it would be
    // with AetherSDR's own padding.
    void theNrTabsShowWholeLabelsAtTheNarrowestWindow()
    {
        make();
        m_window->showPage(QStringLiteral("nr"));
        nrPage()->showMethod(QStringLiteral("nr2"));  // one lit, which has a border of its own
        // What the style keeps for a tab's label, and cuts it off at: the tab less its border
        // and padding.
        const auto room = [](QPushButton* tab) {
            QStyleOptionButton option;
            option.initFrom(tab);
            option.features = QStyleOptionButton::Flat;
            option.text = tab->text();
            if (tab->isChecked()) option.state |= QStyle::State_On;
            return tab->style()->subElementRect(QStyle::SE_PushButtonContents, &option, tab).width();
        };
        const auto label = [](QPushButton* tab) { return tab->fontMetrics().horizontalAdvance(tab->text()); };

        m_window->resize(1, 750);  // as narrow as it goes
        QTRY_COMPARE(m_window->width(), m_window->minimumWidth());
        QTest::qWait(50);
        for (const QString& id : esa::NrPage::tabIds()) {
            QPushButton* tab = nrTab(id);
            QVERIFY2(tab && tab->isVisible(), qPrintable(id));
            QVERIFY2(label(tab) <= room(tab),
                     qPrintable(QStringLiteral("%1 in a window %2 px wide: a %3 px label in %4 px of room")
                                    .arg(tab->text()).arg(m_window->width()).arg(label(tab)).arg(room(tab))));
        }

        m_window->resize(720, 750);
        QTRY_COMPARE(m_window->width(), 720);
        QTest::qWait(50);
        // AetherSDR's method tab padding (ModemChrome, dialog scale), on the tab alone.
        const QString upstream = QStringLiteral("QPushButton[chrome=\"tab\"] { padding-left: 12px; padding-right: 12px; }");
        for (const QString& id : esa::NrPage::tabIds()) {
            QPushButton* tab = nrTab(id);
            const QImage ours = tab->grab().toImage();
            tab->setStyleSheet(upstream);
            QVERIFY2(label(tab) <= room(tab), qPrintable(id));  // nothing cut off there either
            const QImage theirs = tab->grab().toImage();
            tab->setStyleSheet(QString());
            QVERIFY2(!ours.isNull() && ours == theirs, qPrintable(QStringLiteral("%1, %2 px wide, %3")
                                                                     .arg(tab->text()).arg(tab->width())
                                                                     .arg(tab->isChecked() ? QStringLiteral("lit")
                                                                                           : QStringLiteral("unlit"))));
        }
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

        // Every stage is on its AetherRX page and has its window, which keeps its controls.
        for (const QString& stage : esa::RxChain::stageIds()) {
            auto* window = m_window->findChild<esa::DetailsWindow*>(QStringLiteral("details_") + stage);
            QVERIFY2(window, qPrintable(stage));
            QCOMPARE(window->parent(), m_window.get());
            QVERIFY2(host(stage), qPrintable(stage));
            QVERIFY2(window->hasControls(), qPrintable(stage));
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
