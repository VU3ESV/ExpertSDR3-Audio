#include "MockExpert.h"

#include "app/AppInfo.h"
#include "app/Engine.h"
#include "app/Profiles.h"
#include "app/Settings.h"
#include "app/WavFile.h"
#include "dsp/NrProcessor.h"
#include "dsp/RxChain.h"
#include "gui/AboutDialog.h"
#include "gui/DetailsWindow.h"
#include "gui/MainWindow.h"
#include "gui/ParamWidgets.h"
#include "gui/aether/Appearance.h"
#include "gui/pages/Pages.h"
#include "gui/shell/HeaderBar.h"
#include "gui/shell/StatusChip.h"
#include "gui/shell/WindowChrome.h"

#include "ClientCompKnob.h"
#include "FramelessResizer.h"
#include "RxStageReorder.h"
#include "StageTabBar.h"
#include "core/AppSettings.h"

#include <QApplication>
#include <QCheckBox>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

using namespace esa;

namespace {

// Counts the close events a window receives, without changing what it does with them.
class CloseCounter : public QObject {
public:
    int closes{0};

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Close) ++closes;
        return QObject::eventFilter(watched, event);
    }
};

// Every action in a menu and its submenus, separators left out.
QList<QAction*> actionsIn(const QMenu* menu)
{
    QList<QAction*> found;
    const auto actions = menu->actions();
    for (QAction* action : actions) {
        if (action->isSeparator()) continue;
        if (action->menu()) {
            found += actionsIn(action->menu());
        } else {
            found.append(action);
        }
    }
    return found;
}

QString railTabName(const QString& label)
{
    return QStringLiteral("aetherRxTab") + QString(label).remove(QLatin1Char(' '));
}

// Delivers a drag-enter then a drop carrying `payload` under `mime` to `bar` at height `y`.
bool deliverDrop(AetherSDR::StageTabBar& bar, const QString& mime, int payload, int y)
{
    QMimeData data;
    data.setData(mime, QByteArray::number(payload));
    const QPoint pos(bar.width() / 2, y);
    QDragEnterEvent enter(pos, Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &enter);
    QDropEvent drop(QPointF(pos), Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &drop);
    return drop.isAccepted();
}

// A bar on its own, wired to a host that records each reorder it commits. From upstream's
// stage_tab_bar_drag_test.
struct LoneBar {
    LoneBar(const QString& prefix, std::initializer_list<int> ids)
        : bar(prefix)
    {
        order = QVector<int>(ids);
        AetherSDR::StageTabBar::Host host;
        host.isChainStage = [](int) { return true; };
        host.stageEnabled = [](int) { return true; };
        host.setStageEnabled = [](int, bool) {};
        host.chainOrder = [this] { return order; };
        host.commitChainOrder = [this](const QVector<int>& o) {
            order = o;
            ++commits;
        };
        bar.setHost(host);
        int n = 0;
        for (int id : ids) bar.addStage(id, QStringLiteral("S%1").arg(++n));
        bar.resize(160, 300);
    }

    AetherSDR::StageTabBar bar;
    QVector<int> order;
    int commits{0};
};

// Keeps what would have reached the speakers, so a test can hear what PLAY played.
class RecordingSink final : public AudioSink {
public:
    QString deviceName() const override { return QStringLiteral("recording"); }
    void write(const float* stereo, int frames) override
    {
        samples.insert(samples.end(), stereo, stereo + qsizetype(frames) * 2);
    }
    bool alive() const override { return true; }
    SinkStats stats() override { return {}; }
    void setGainDb(double) override {}
    void setBoost(bool) override {}
    void setMuted(bool) override {}
    void setTargetLatencyMs(int) override {}

    std::vector<float> samples;
};

// A recording's frames, as a player reads them.
std::optional<std::vector<float>> framesOf(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    const std::optional<WavFile::Info> info = WavFile::parse(file);
    if (!info || info->sampleRate != 48000) return std::nullopt;
    std::vector<float> frames(size_t(info->frames) * 2);
    const qint64 bytes = qint64(frames.size() * sizeof(float));
    if (file.read(reinterpret_cast<char*>(frames.data()), bytes) != bytes) return std::nullopt;
    return frames;
}

// What would change what the radio does or how it sounds. VOLUME also clears MUTE on
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

// While it lives, and when `on`, Custom is the frameless frame of Windows and Linux, so that
// path runs on every machine. A test makes and uses its window within its lifetime.
struct FramelessCustom {
    explicit FramelessCustom(bool on)
    {
        if (on) WindowChrome::overrideCustomMode(WindowChrome::Mode::Frameless);
    }
    ~FramelessCustom() { WindowChrome::overrideCustomMode(std::nullopt); }
    FramelessCustom(const FramelessCustom&) = delete;
    FramelessCustom& operator=(const FramelessCustom&) = delete;
};

// A main window's flags as Qt gives them, frame and all.
Qt::WindowFlags plainFlags()
{
    return QMainWindow().windowFlags();
}

// Whether a combo box has room for its current text whole, in the field its style draws it in.
bool showsItsTextWhole(const QComboBox* combo)
{
    QStyleOptionComboBox option;
    option.initFrom(combo);
    option.currentText = combo->currentText();
    const QRect field = combo->style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, combo);
    return combo->fontMetrics().horizontalAdvance(combo->currentText()) <= field.width();
}

} // namespace

// The window's shell: the rail, the header, the menus, where it opens and every way it
// closes. Built against a real engine that is never connected unless a test hands it a
// MockExpert, and then only to that.
class TestShell : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    std::unique_ptr<Settings> m_settings;
    std::unique_ptr<ProfileLibrary> m_profiles;
    std::unique_ptr<Engine> m_engine;
    std::unique_ptr<MainWindow> m_window;
    int m_quits{0};

    // Where REC saves in every test: never the real Documents folder. Made fresh for each
    // test and only by a recording, so a test can tell whether one made it.
    std::unique_ptr<QTemporaryDir> m_recordings;
    QString m_recordingFolder;  // set before make() to record somewhere else
    // The window's ways out to the desktop, replaced: what it asked to reveal, and the folder
    // the next Folder... returns.
    QStringList m_revealed;
    QString m_folderChoice;
    int m_folderAsks{0};
    // With a radio: what reached the speakers, and the engine's last status.
    bool m_withSink{false};
    RecordingSink* m_sink{nullptr};
    EngineStatus m_status;

    QString iniPath() const { return m_dir.filePath(QStringLiteral("shell.ini")); }

    QString recordingsFolder()
    {
        if (!m_recordings) m_recordings = std::make_unique<QTemporaryDir>();
        return m_recordings->filePath(QStringLiteral("Recordings"));
    }

    // `window` is what the last session left in ui/*.
    void make(const Settings::WindowState& window = {}, const AppConfig& config = {})
    {
        m_window.reset();
        if (m_withSink) {
            m_engine = std::make_unique<Engine>(iniPath(), [this](const AppConfig&, QString*) {
                auto sink = std::make_unique<RecordingSink>();
                m_sink = sink.get();
                return std::unique_ptr<AudioSink>(std::move(sink));
            });
        } else {
            m_engine = std::make_unique<Engine>(iniPath());
        }
        // The engine runs on this thread, so this sees each status as it is sent.
        connect(m_engine.get(), &Engine::statusChanged, this, [this](const EngineStatus& s) { m_status = s; });
        m_settings = std::make_unique<Settings>(iniPath());
        m_settings->saveWindowState(window);
        m_settings->setRecordingFolder(m_recordingFolder.isEmpty() ? recordingsFolder() : m_recordingFolder);
        m_profiles = std::make_unique<ProfileLibrary>(ProfileLibrary::defaultPath(iniPath()));
        m_window = std::make_unique<MainWindow>(m_engine.get(), m_engine->taps(), m_settings.get(), m_profiles.get(),
                                                config);
        m_window->setQuitHandler([this] { ++m_quits; });
        m_window->setRevealHandler([this](const QString& path) { m_revealed << path; });
        m_window->setFolderChooser([this](const QString&) {
            ++m_folderAsks;
            return m_folderChoice;
        });
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
    }

    // The window on an engine connected to `radio` alone, playing into a RecordingSink.
    void makeOnRadio(MockExpert& radio)
    {
        AppConfig config;
        config.host = QStringLiteral("127.0.0.1");
        config.port = radio.port();
        // Never the defaults: one of them is where a real ExpertSDR3 may be listening.
        config.fallbackPorts.clear();
        m_withSink = true;
        make({}, config);
        if (QTest::currentTestFailed()) return;
        DiscoveredRadio picked;
        picked.host = QStringLiteral("127.0.0.1");
        picked.port = radio.port();
        m_window->connectToRadio(picked);
        QTRY_VERIFY_WITH_TIMEOUT(m_status.audioFlowing && m_status.frequencyHz > 0, 6000);
    }

    // Nothing more from the engine, so a test can hand the window statuses of its own.
    void quieten()
    {
        m_engine->shutdown();
        QCoreApplication::processEvents();
    }

    // A status, as the engine sends it.
    void feed(const EngineStatus& s)
    {
        emit m_engine->statusChanged(s);
        QCoreApplication::processEvents();
    }

    QPushButton* recordButton() { return rail()->findChild<QPushButton*>(QStringLiteral("aetherRxRecord")); }
    QPushButton* playButton() { return rail()->findChild<QPushButton*>(QStringLiteral("aetherRxPlay")); }

    // The warnings on screen.
    QList<QMessageBox*> notices()
    {
        QList<QMessageBox*> shown;
        const auto boxes = m_window->findChildren<QMessageBox*>();
        for (QMessageBox* box : boxes) {
            if (box->isVisible()) shown << box;
        }
        return shown;
    }

    // Opens gear › Recordings, which fills it in as it opens.
    QMenu* openRecordingsMenu()
    {
        auto* menu = m_window->findChild<QMenu*>(QStringLiteral("recordingsMenu"));
        if (!menu) return nullptr;
        menu->popup(m_window->mapToGlobal(QPoint(40, 40)));
        if (!QTest::qWaitFor([menu] { return menu->isVisible(); })) return nullptr;
        return menu;
    }

    QString menuEntry(const char* name)
    {
        auto* action = m_window->findChild<QAction*>(QLatin1String(name));
        return action ? action->text() : QString();
    }

    // Shows it at the size of the references, not maximised however small the screen.
    void atReferenceSize()
    {
        m_window->showNormal();
        m_window->resize(722, 746);
        QCoreApplication::processEvents();
        QTRY_COMPARE(m_window->size(), QSize(722, 746));
    }

    void activate()
    {
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
    }

    // What a fresh Settings reads back: what the window wrote.
    Settings::WindowState savedWindow() const { return Settings(iniPath()).loadWindowState(); }

    QPushButton* railTab(const QString& label) { return m_window->findChild<QPushButton*>(railTabName(label)); }

    AetherSDR::StageTabBar* rail() { return m_window->findChild<AetherSDR::StageTabBar*>(); }

    // Every row's label, top to bottom as the rail shows it.
    QStringList railRows()
    {
        QCoreApplication::processEvents();  // the rail re-lays its rows out on the next pass
        QList<QPair<int, QString>> rows;
        const QStringList labels = {QStringLiteral("Radio"), QStringLiteral("AetherNR"), QStringLiteral("Gate"),
                                    QStringLiteral("EQ"), QStringLiteral("Compressor"), QStringLiteral("Tube"),
                                    QStringLiteral("Exciter"), QStringLiteral("Final Output")};
        for (const QString& label : labels) {
            QPushButton* tab = railTab(label);
            if (!tab) return {};
            rows.append({tab->mapTo(m_window.get(), QPoint()).y(), label});
        }
        std::sort(rows.begin(), rows.end());
        QStringList order;
        for (const auto& row : rows) order << row.second;
        return order;
    }

    ParamControl* control(const QString& key)
    {
        const auto controls = m_window->findChildren<ParamControl*>();
        for (ParamControl* c : controls) {
            if (c->spec().key == key) return c;
        }
        return nullptr;
    }

    // Set ESA_GUI_SHOT_DIR to keep a PNG of a menu or popup, for eyeballing.
    static void shot(QWidget* widget, const QString& name)
    {
        const QString dir = qEnvironmentVariable("ESA_GUI_SHOT_DIR");
        if (!dir.isEmpty() && widget) widget->grab().save(QDir(dir).filePath(name + QStringLiteral(".png")));
    }

    // The menu that has just popped up, once it has.
    QMenu* openPopup()
    {
        for (int i = 0; i < 100; ++i) {
            if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) return menu;
            QTest::qWait(20);
        }
        return nullptr;
    }

    QAction* actionNamed(const QMenu* menu, const QString& text)
    {
        const auto actions = actionsIn(menu);
        for (QAction* action : actions) {
            if (action->text() == text) return action;
        }
        return nullptr;
    }

    // The brightest pixel of an action's row as the menu draws it: its text, in practice.
    static int rowPeak(QMenu* menu, QAction* action)
    {
        menu->setActiveAction(nullptr);  // no row lit up by wherever the pointer is
        const QImage image = menu->grab().toImage();
        const qreal dpr = image.devicePixelRatio();
        const QRect row = menu->actionGeometry(action);
        const QRect area = QRect(row.topLeft() * dpr, row.size() * dpr) & image.rect();
        int peak = 0;
        for (int y = area.top(); y <= area.bottom(); ++y) {
            for (int x = area.left(); x <= area.right(); ++x) {
                const QRgb pixel = image.pixel(x, y);
                peak = std::max(peak, qRed(pixel) + qGreen(pixel) + qBlue(pixel));
            }
        }
        return peak;
    }

    // Each of `off` is drawn in the dim colour and each of `on` brighter. A dim row's text is
    // never brighter than its colour; a normal row's (#eaf2fb, a sum of 727 against the dim
    // 288) has pixels well above it however lightly a platform antialiases.
    void compareRows(QMenu* menu, const QStringList& off, const QStringList& on)
    {
        const QColor dim = Appearance::color(QStringLiteral("color.button.foreground.disabled"));
        const int limit = dim.red() + dim.green() + dim.blue() + 60;
        for (const QString& text : off) {
            QAction* action = actionNamed(menu, text);
            QVERIFY2(action && !action->isEnabled(), qPrintable(text));
            const int peak = rowPeak(menu, action);
            QVERIFY2(peak <= limit, qPrintable(QStringLiteral("%1 is drawn at %2, over %3").arg(text).arg(peak).arg(limit)));
        }
        for (const QString& text : on) {
            QAction* action = actionNamed(menu, text);
            QVERIFY2(action && action->isEnabled(), qPrintable(text));
            const int peak = rowPeak(menu, action);
            QVERIFY2(peak > limit, qPrintable(QStringLiteral("%1 is drawn at %2, not over %3").arg(text).arg(peak).arg(limit)));
        }
    }

private slots:
    void initTestCase()
    {
        // Caches the pages write (AetherSDR's painted check boxes) go to the test location.
        QStandardPaths::setTestModeEnabled(true);
        Appearance::install();
    }

    void cleanup()
    {
        if (QWidget* popup = QApplication::activePopupWidget()) popup->close();
        m_window.reset();
        if (m_engine) m_engine->shutdown();
        m_engine.reset();
        m_settings.reset();
        m_profiles.reset();
        m_quits = 0;
        // After the engine, which has finished every file in it by now.
        m_recordings.reset();
        m_recordingFolder.clear();
        m_revealed.clear();
        m_folderChoice.clear();
        m_folderAsks = 0;
        m_withSink = false;
        m_sink = nullptr;
        m_status = EngineStatus();
        QDir dir(m_dir.path());
        const QStringList files = dir.entryList(QDir::Files);
        for (const QString& file : files) dir.remove(file);
    }

    // ── Closing ───────────────────────────────────────────────────────────────

    // Escape belongs to whatever has focus: it reverts a field, closes a menu, and never
    // closes the window, because closing it is what gives ExpertSDR3 its audio back.
    void escapeNeverClosesTheWindow()
    {
        make();
        CloseCounter counter;
        m_window->installEventFilter(&counter);
        activate();

        QTest::keyClick(m_window.get(), Qt::Key_Escape);

        auto* host = m_window->findChild<QLineEdit*>(QStringLiteral("host"));
        QVERIFY(host);
        m_window->showPage(QStringLiteral("radio"));
        host->setFocus();
        QTest::keyClick(host, Qt::Key_Escape);

        // An AetherRX knob's value field, where Escape reverts what was typed.
        m_window->showPage(QStringLiteral("gate"));
        auto* page = m_window->findChild<QWidget*>(QStringLiteral("page_gate"));
        QVERIFY(page);
        auto* knob = page->findChild<AetherSDR::ClientCompKnob*>();
        QVERIFY(knob);
        auto* knobField = knob->findChild<QLineEdit*>();
        QVERIFY(knobField);
        knobField->setFocus();
        QTest::keyClick(knobField, Qt::Key_Escape);

        // A parameter's value in a spin box on a page of the main window: AetherNR's readings.
        m_window->showPage(QStringLiteral("nr"));
        auto* nr = m_window->findChild<QWidget*>(QStringLiteral("page_nr"));
        QVERIFY(nr);
        QDoubleSpinBox* reading = nullptr;
        for (QDoubleSpinBox* spin : nr->findChildren<QDoubleSpinBox*>()) {
            if (spin->isVisible()) {
                reading = spin;
                break;
            }
        }
        QVERIFY(reading);
        reading->setFocus();
        QTest::keyClick(reading, Qt::Key_Escape);
        QVERIFY(m_window->isVisible());

        // A parameter's exact value, in the stage's exact-values window: Escape closes that
        // window, as it should a secondary one, and never the main one.
        m_window->showPage(QStringLiteral("comp"));
        auto* details = m_window->findChild<DetailsWindow*>(QStringLiteral("details_comp"));
        QVERIFY(details);
        details->present();
        QVERIFY(QTest::qWaitForWindowExposed(details));
        auto* field = details->findChild<QDoubleSpinBox*>();
        QVERIFY(field);
        field->setFocus();
        QTest::keyClick(field, Qt::Key_Escape);
        QTRY_VERIFY(!details->isVisible());
        activate();

        // In the gear's menu, Escape closes the menu.
        rail()->findChild<QPushButton*>(QStringLiteral("aetherRxSettingsButton"))->click();
        QMenu* gear = openPopup();
        QVERIFY(gear);
        shot(gear, QStringLiteral("gear-menu"));
        QTest::keyClick(gear, Qt::Key_Escape);
        QTRY_VERIFY(!gear->isVisible());

        QCoreApplication::processEvents();
        QVERIFY(m_window->isVisible());
        QCOMPARE(counter.closes, 0);
        QCOMPARE(m_quits, 0);
    }

    void everyWayOutReachesCloseEvent_data()
    {
        QTest::addColumn<QString>("way");
        QTest::newRow("close()") << QStringLiteral("close");
        // What the frame's close button, Alt+F4 and a Mac's Cmd+Q all do: the platform asks
        // the window to close.
        QTest::newRow("the platform's close") << QStringLiteral("platform");
        QTest::newRow("Quit in the gear menu") << QStringLiteral("quit");
    }
    void everyWayOutReachesCloseEvent()
    {
        QFETCH(QString, way);
        make();
        CloseCounter counter;
        m_window->installEventFilter(&counter);
        m_window->showPage(QStringLiteral("tube"));

        if (way == QLatin1String("close")) {
            m_window->close();
        } else if (way == QLatin1String("platform")) {
            QVERIFY(m_window->windowHandle());
            m_window->windowHandle()->close();
        } else {
            auto* gear = m_window->findChild<QMenu*>(QStringLiteral("gearMenu"));
            QVERIFY(gear);
            QAction* quit = actionNamed(gear, QStringLiteral("Quit"));
#ifdef Q_OS_MACOS
            QVERIFY2(!quit, "a Mac's application menu has Quit");
            QSKIP("a Mac quits from its application menu, which closes the window as the platform row does");
#else
            QVERIFY(quit);
            quit->trigger();
#endif
        }

        QTRY_COMPARE(counter.closes, 1);
        QCOMPARE(m_quits, 1);
        QVERIFY(!m_window->isVisible());
        // closeEvent saved on the way out.
        QCOMPARE(savedWindow().page, QStringLiteral("tube"));
    }

    // ── Opening ───────────────────────────────────────────────────────────────

    void theKeyboardStartsOnTheRail()
    {
        make();
        activate();
        QTRY_VERIFY(QApplication::focusWidget() != nullptr);
        QCOMPARE(QApplication::focusWidget()->objectName(), QStringLiteral("aetherRxTabRadio"));
        // A chip is not a stop on the way through the window.
        QCOMPARE(m_window->findChild<StatusChip*>()->focusPolicy(), Qt::NoFocus);
    }

    void theFirstRunOpensOnRadioAndLaterRunsWhereTheyLeftOff()
    {
        make();
        QCOMPARE(m_window->currentPage(), QStringLiteral("radio"));
        QVERIFY(railTab(QStringLiteral("Radio"))->isChecked());
        m_window->showPage(QStringLiteral("exciter"));
        m_window->close();
        QCOMPARE(savedWindow().page, QStringLiteral("exciter"));

        Settings::WindowState window;
        window.page = QStringLiteral("exciter");
        make(window);
        QCOMPARE(m_window->currentPage(), QStringLiteral("exciter"));
        QVERIFY(railTab(QStringLiteral("Exciter"))->isChecked());

        window.page = QStringLiteral("no such page");
        make(window);
        QCOMPARE(m_window->currentPage(), QStringLiteral("radio"));
    }

    // Header and rail exactly as tall and wide as AetherRX's, so the pages sit on the same
    // pixels as in the reference screenshots.
    void thePageColumnStartsWhereAetherRxsDoes()
    {
        make();
        atReferenceSize();
        auto* stack = m_window->findChild<QStackedWidget*>();
        QVERIFY(stack);
        QCOMPARE(stack->mapTo(m_window.get(), QPoint()), QPoint(203, 53));
        QCOMPARE(rail()->mapTo(m_window.get(), QPoint()), QPoint(15, 53));
        QCOMPARE(rail()->width(), 180);
        QCOMPARE(stack->size(), QSize(722 - 203 - 15, 746 - 53 - 15));
    }

    // At that size every page fits the column's width: a page may scroll down, never sideways,
    // where a cut-off field would hide behind a scroll bar.
    void everyPageFitsTheColumnsWidth()
    {
        make();
        atReferenceSize();
        auto* stack = m_window->findChild<QStackedWidget*>();
        QVERIFY(stack);
        for (const QString& id : m_window->pageIds()) {
            m_window->showPage(id);
            QCoreApplication::processEvents();
            QWidget* page = stack->currentWidget();
            QVERIFY(page);
            for (QScrollArea* scroll : page->findChildren<QScrollArea*>()) {
                if (!scroll->isVisible()) continue;
                QVERIFY2(scroll->horizontalScrollBar()->maximum() == 0,
                         qPrintable(QStringLiteral("%1 is %2 px too wide")
                                        .arg(id)
                                        .arg(scroll->horizontalScrollBar()->maximum())));
            }
        }
    }

    void theGeometryRoundTrips()
    {
        Settings::WindowState window;
        // Whole on the offscreen platform's 800x800 screen: one hanging off it is moved back on.
        window.geometry = QStringLiteral("80,90,700,650");
        make(window);
        // Maximised or not (a small screen opens it maximised), this is where it returns to.
        QCOMPARE(m_window->normalGeometry(), QRect(80, 90, 700, 650));
        m_window->close();
        QCOMPARE(savedWindow().geometry, QStringLiteral("80,90,700,650"));
    }

    void aWindowFromAScreenThatHasGoneIsCentred()
    {
        Settings::WindowState window;
        window.geometry = QStringLiteral("-30000,-30000,700,500");  // no screen is there
        make(window);
        const QRect normal = m_window->normalGeometry();
        const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
        QCOMPARE(normal.size(), QSize(700, 500).boundedTo(screen.size()));
        QVERIFY2(qAbs(normal.center().x() - screen.center().x()) <= 1 && qAbs(normal.center().y() - screen.center().y()) <= 1,
                 qPrintable(QStringLiteral("%1,%2 against %3,%4").arg(normal.center().x()).arg(normal.center().y())
                                .arg(screen.center().x()).arg(screen.center().y())));
    }

    void aSmallScreenOpensMaximised()
    {
        QVERIFY(MainWindow::isSmallScreen(QSize(800, 480)));  // a Raspberry Pi touchscreen
        QVERIFY(MainWindow::isSmallScreen(QSize(1280, 720)));
        QVERIFY(!MainWindow::isSmallScreen(QSize(1920, 1080)));
        QVERIFY(!MainWindow::isSmallScreen(QSize(1440, 900)));
        make();
        const QSize available = QGuiApplication::primaryScreen()->availableGeometry().size();
        QCOMPARE(m_window->isMaximized(), MainWindow::isSmallScreen(available));
        if (!m_window->isMaximized()) QCOMPARE(m_window->size(), MainWindow::defaultSize());
    }

    // Every case of where the window opens, on screens the offscreen platform cannot have:
    // its one screen is 800x800, where the usual size fits.
    void whereTheWindowOpens_data()
    {
        QTest::addColumn<QString>("saved");
        QTest::addColumn<QList<QRect>>("screens");  // the main screen first
        QTest::addColumn<QRect>("geometry");
        QTest::addColumn<bool>("positioned");
        QTest::addColumn<bool>("maximised");
        QTest::addColumn<int>("title");  // the system's title bar above the content; 0 for none
        QTest::addColumn<bool>("placeTitle");  // the system leaves the title bar where it is put
        const QRect desk(0, 25, 1920, 1055);    // a desktop under a menu bar
        const QRect side(1920, 0, 1280, 720);   // a small second screen to its right
        const QRect pi(0, 0, 800, 444);         // a Pi touchscreen above its panel
        const QRect wide(800, 0, 1920, 1080);   // a big screen beside the Pi's
        QTest::newRow("first run, a big screen: the default size")
            << QString() << QList<QRect>{desk} << QRect(0, 0, 724, 752) << false << false << 0 << false;
        QTest::newRow("first run, a small screen: maximised")
            << QString() << QList<QRect>{pi} << QRect(0, 0, 724, 444) << false << true << 0 << false;
        QTest::newRow("first run, no screen at all")
            << QString() << QList<QRect>{} << QRect(0, 0, 724, 752) << false << false << 0 << false;
        QTest::newRow("a saved geometry that does not parse")
            << QStringLiteral("700x650") << QList<QRect>{desk} << QRect(0, 0, 724, 752) << false << false << 0 << false;
        QTest::newRow("saved on the main screen")
            << QStringLiteral("100,100,900,800") << QList<QRect>{desk, side} << QRect(100, 100, 900, 800) << true << false << 0 << false;
        QTest::newRow("saved on a small second screen: maximised there")
            << QStringLiteral("2000,50,700,600") << QList<QRect>{desk, side} << QRect(2000, 50, 700, 600) << true << true << 0 << false;
        QTest::newRow("saved on a big second screen, the main one small")
            << QStringLiteral("900,100,724,752") << QList<QRect>{pi, wide} << QRect(900, 100, 724, 752) << true << false << 0 << false;
        QTest::newRow("its screen has gone: the same size, centred on the main one")
            << QStringLiteral("2000,50,700,600") << QList<QRect>{desk} << QRect(610, 253, 700, 600) << true << false << 0 << false;
        QTest::newRow("its screen has gone, and it is bigger than the main one")
            << QStringLiteral("-5000,-5000,2500,1400") << QList<QRect>{desk} << desk << true << false << 0 << false;
        QTest::newRow("saved on a screen that has since shrunk: kept whole on it")
            << QStringLiteral("100,100,1600,1000") << QList<QRect>{QRect(0, 25, 1440, 875)} << QRect(0, 25, 1440, 875)
            << true << false << 0 << false;
        QTest::newRow("saved hanging off its screen's corner: moved back onto it")
            << QStringLiteral("1500,900,724,752") << QList<QRect>{desk} << QRect(1196, 328, 724, 752) << true << false << 0 << false;
        // With the system's frame, room for its title bar: content as tall as the screen would
        // leave the title above the top, and the window system, moving it down, would push the
        // bottom off. Where it fits, it stays where it was.
        QTest::newRow("saved on a screen that has since shrunk, with a title bar: room left for it")
            << QStringLiteral("100,100,1600,1000") << QList<QRect>{QRect(0, 25, 1440, 875)} << QRect(0, 63, 1440, 837)
            << true << false << 38 << false;
        QTest::newRow("saved just under the menu bar, with a title bar: left where it was")
            << QStringLiteral("200,53,724,752") << QList<QRect>{desk} << QRect(200, 53, 724, 752) << true << false << 38 << false;
        QTest::newRow("its screen has gone, and it is bigger than the main one, with a title bar")
            << QStringLiteral("-5000,-5000,2500,1400") << QList<QRect>{desk} << QRect(0, 44, 1920, 1017) << true << false
            << 38 << false;
        QTest::newRow("its screen has gone, with a title bar: still centred")
            << QStringLiteral("2000,50,700,600") << QList<QRect>{desk} << QRect(610, 253, 700, 600) << true << false << 38 << false;
        QTest::newRow("first run, a short screen, with a title bar: the default size less the title")
            << QString() << QList<QRect>{QRect(0, 0, 1024, 768)} << QRect(0, 0, 724, 730) << false << true << 38 << false;
        // Windows leaves a window where it is put, its title bar above the screen or not, so
        // the content starts a title bar's height down, and a centred window is centred whole.
        const QRect work(0, 0, 1920, 1040);  // a Windows desktop over its taskbar
        QTest::newRow("saved near the top, a system that leaves the title bar where it is put: moved under it")
            << QStringLiteral("300,10,724,752") << QList<QRect>{work} << QRect(300, 38, 724, 752) << true << false << 38
            << true;
        QTest::newRow("saved lower down, a system that leaves the title bar where it is put: left where it was")
            << QStringLiteral("300,38,724,752") << QList<QRect>{work} << QRect(300, 38, 724, 752) << true << false << 38
            << true;
        QTest::newRow("its screen has gone, a system that leaves the title bar where it is put: centred whole")
            << QStringLiteral("2000,50,700,600") << QList<QRect>{desk} << QRect(610, 272, 700, 600) << true << false << 38
            << true;
        QTest::newRow("its screen has gone, bigger than the main one, a system that leaves the title bar")
            << QStringLiteral("-5000,-5000,2500,1400") << QList<QRect>{desk} << QRect(0, 63, 1920, 1017) << true << false
            << 38 << true;
        QTest::newRow("saved on a shrunk screen, a system that leaves the title bar: kept whole under it")
            << QStringLiteral("100,0,1600,1000") << QList<QRect>{QRect(0, 0, 1440, 860)} << QRect(0, 38, 1440, 822)
            << true << false << 38 << true;
        QTest::newRow("no title bar, a system that leaves the title bar where it is put: nothing to place")
            << QStringLiteral("300,0,724,752") << QList<QRect>{work} << QRect(300, 0, 724, 752) << true << false << 0
            << true;
    }

    void whereTheWindowOpens()
    {
        QFETCH(QString, saved);
        QFETCH(QList<QRect>, screens);
        QFETCH(QRect, geometry);
        QFETCH(bool, positioned);
        QFETCH(bool, maximised);
        QFETCH(int, title);
        QFETCH(bool, placeTitle);
        const MainWindow::Placement place = MainWindow::placement(saved, screens, title, placeTitle);
        QCOMPARE(place.positioned, positioned);
        QCOMPARE(positioned ? place.geometry : QRect(QPoint(), place.geometry.size()), geometry);
        QCOMPARE(place.maximised, maximised);
    }

    // AetherRX's 600x400 minimum: its height, and the layout's own width, which is wider.
    void theWindowKeepsAetherRxsMinimumSize()
    {
        make();
        m_window->showNormal();
        QCOMPARE(m_window->minimumHeight(), 400);
        QVERIFY2(m_window->minimumWidth() >= 600, qPrintable(QString::number(m_window->minimumWidth())));
        QCOMPARE(m_window->minimumWidth(), m_window->minimumSizeHint().width());
        m_window->resize(300, 200);
        QTRY_COMPARE(m_window->size(), QSize(m_window->minimumWidth(), 400));
    }

    // A screenshot run shows a page and a size of its own choosing. The next ordinary start
    // still opens where the operator left the window.
    void aScreenshotRunLeavesTheWindowStateAlone()
    {
        Settings::WindowState window;
        window.page = QStringLiteral("tube");
        window.geometry = QStringLiteral("100,100,700,650");
        make(window);
        m_window->setSavesWindowState(false);  // as main() does for --screenshot
        m_window->showPage(QStringLiteral("eq"));
        atReferenceSize();
        control(QStringLiteral("tube.drive"))->setValue(3);
        m_window->close();
        QCOMPARE(m_quits, 1);
        const Settings::WindowState saved = savedWindow();
        QCOMPARE(saved.page, window.page);
        QCOMPARE(saved.geometry, window.geometry);
        // The settings themselves are saved as before.
        QCOMPARE(Settings(iniPath()).load().params.value(QStringLiteral("tube.drive")), 3.0);
    }

    // The AetherSDR pages' lasting display choices, such as the EQ's reference curve, are this
    // computer's settings, under ui/aether/. What else they store stays in memory: our window
    // keeps its own geometry.
    void thePagesDisplayChoicesAreKeptWithTheSettings()
    {
        const QString curve = QStringLiteral("ClientEqReferenceCurve");
        const QString geometry = QStringLiteral("StripGatePanelGeometry");
        auto& pages = AetherSDR::AppSettings::instance();
        make();
        pages.setValue(curve, QStringLiteral("Pink"));
        pages.setValue(geometry, QStringLiteral("x"));
        QCOMPARE(Settings(iniPath()).aetherValue(curve).toString(), QStringLiteral("Pink"));
        QVERIFY(!Settings(iniPath()).aetherValue(geometry).isValid());
        QVERIFY(QSettings(iniPath(), QSettings::IniFormat).contains(QStringLiteral("ui/aether/ClientEqReferenceCurve")));

        // Without a window nothing reaches settings that may be gone; the next window reads
        // the choice back.
        m_window.reset();
        QVERIFY(!pages.contains(curve));
        make();
        QCOMPARE(pages.value(curve).toString(), QStringLiteral("Pink"));
        pages.remove(curve);
        QVERIFY(!Settings(iniPath()).aetherValue(curve).isValid());
        pages.remove(geometry);
    }

    // ── The frame ─────────────────────────────────────────────────────────────

    // The system's frame on every platform until its custom one is signed off on a real
    // screen. A fresh settings file says nothing, and nothing is written for it.
    void aFreshSettingsFileGivesTheSystemFrame()
    {
        QCOMPARE(WindowChrome::defaultFrame(), WindowChrome::Frame::System);
        QCOMPARE(WindowChrome::parse(QString()), WindowChrome::Frame::System);
        QCOMPARE(WindowChrome::parse(QStringLiteral("Fancy")), WindowChrome::Frame::System);
        make();
        QCOMPARE(m_window->windowFlags(), plainFlags());
        QVERIFY(!m_window->testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY(m_window->testAttribute(Qt::WA_ContentsMarginsRespectsSafeArea));
        QVERIFY(!m_window->findChild<QToolButton*>(QStringLiteral("headerClose")));
        QVERIFY(!m_window->findChild<AetherSDR::FramelessResizer*>());
        QMenu* gear = m_window->findChild<QMenu*>(QStringLiteral("gearMenu"));
        QVERIFY(actionNamed(gear, QStringLiteral("System"))->isChecked());
        QVERIFY(!actionNamed(gear, QStringLiteral("Custom"))->isChecked());
        m_window->close();
        QVERIFY(savedWindow().frame.isEmpty());  // still the default, should the default change
    }

    // What each ui/frame value gives on this platform: Custom draws the title bar over the
    // ground on a Mac, and has no system frame at all on Windows and Linux. Never see-through.
    void theFrameFlagsForEachValue_data()
    {
        QTest::addColumn<QString>("frame");
        QTest::addColumn<bool>("custom");
        QTest::newRow("never chosen") << QString() << false;
        QTest::newRow("System") << QStringLiteral("System") << false;
        QTest::newRow("Custom") << QStringLiteral("Custom") << true;
        QTest::newRow("not a frame") << QStringLiteral("Fancy") << false;
    }
    void theFrameFlagsForEachValue()
    {
        QFETCH(QString, frame);
        QFETCH(bool, custom);
        Settings::WindowState window;
        window.frame = frame;
        make(window);
        const Qt::WindowFlags flags = m_window->windowFlags();
        const Qt::WindowFlags plain = plainFlags();
        QVERIFY(!m_window->testAttribute(Qt::WA_TranslucentBackground));
        bool chips = false;
#if defined(Q_OS_MACOS) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        QCOMPARE(flags, custom ? plain | Qt::ExpandedClientAreaHint | Qt::NoTitleBarBackgroundHint : plain);
        // The content spreads under the title bar, and keeps clear of it by its own margins.
        QCOMPARE(m_window->testAttribute(Qt::WA_ContentsMarginsRespectsSafeArea), !custom);
        QCOMPARE(m_window->centralWidget()->testAttribute(Qt::WA_ContentsMarginsRespectsSafeArea), !custom);
#elif defined(Q_OS_MACOS)
        QCOMPARE(flags, plain);  // Qt before 6.9 cannot draw a title bar over the content
#else
        QCOMPARE(flags, custom ? plain | Qt::FramelessWindowHint : plain);
        chips = custom;
#endif
        // The chips come only with a window that has no buttons of its own.
        QCOMPARE(m_window->findChild<QToolButton*>(QStringLiteral("headerMinimise")) != nullptr, chips);
        QCOMPARE(m_window->findChild<QToolButton*>(QStringLiteral("headerClose")) != nullptr, chips);
        QCOMPARE(m_window->findChild<AetherSDR::FramelessResizer*>() != nullptr, chips);
        QMenu* gear = m_window->findChild<QMenu*>(QStringLiteral("gearMenu"));
        const bool customShown = custom && WindowChrome::customMode() != WindowChrome::Mode::System;
        QCOMPARE(actionNamed(gear, QStringLiteral("Custom"))->isChecked(), customShown);
        QCOMPARE(actionNamed(gear, QStringLiteral("System"))->isChecked(), !customShown);
        m_window->close();
        // Kept as it was found; a value that is not a frame reads as never chosen.
        QCOMPARE(savedWindow().frame, custom || frame == QLatin1String("System") ? frame : QString());
    }

    // The flags each way of making a frame gives, whatever this machine is, and what Custom
    // is here.
    void theFrameFlagsOnEveryPlatform()
    {
        using Mode = WindowChrome::Mode;
        const Qt::WindowFlags plain = plainFlags();
        QCOMPARE(WindowChrome::flagsFor(plain, Mode::System), plain);
        // Nothing but the frame changes: Windows' minimise and maximise come from the rest.
        QCOMPARE(WindowChrome::flagsFor(plain, Mode::Frameless), plain | Qt::FramelessWindowHint);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        QCOMPARE(WindowChrome::flagsFor(plain, Mode::ExpandedClientArea),
                 plain | Qt::ExpandedClientAreaHint | Qt::NoTitleBarBackgroundHint);
#else
        QCOMPARE(WindowChrome::flagsFor(plain, Mode::ExpandedClientArea), plain);
#endif
        for (Mode from : {Mode::ExpandedClientArea, Mode::Frameless}) {
            QCOMPARE(WindowChrome::flagsFor(WindowChrome::flagsFor(plain, from), Mode::System), plain);
        }
#if defined(Q_OS_MACOS) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        QCOMPARE(WindowChrome::customMode(), Mode::ExpandedClientArea);
#elif defined(Q_OS_MACOS)
        QCOMPARE(WindowChrome::customMode(), Mode::System);
#else
        QCOMPARE(WindowChrome::customMode(), Mode::Frameless);
#endif
        QCOMPARE(WindowChrome::modeFor(WindowChrome::Frame::System), Mode::System);
    }

    // Gear > Window frame applies at once, with no restart: the window comes back where it
    // was, at its size, on its page, with the keyboard where it was, and the choice is saved.
    // Then the same with the window maximised.
    void switchingTheFrameAppliesAtOnce_data()
    {
        QTest::addColumn<bool>("frameless");
        QTest::newRow("this platform's Custom") << false;
        QTest::newRow("Windows' and Linux's Custom") << true;
    }
    void switchingTheFrameAppliesAtOnce()
    {
        QFETCH(bool, frameless);
        const FramelessCustom asOnWindows(frameless);
        make();
        QMenu* gear = m_window->findChild<QMenu*>(QStringLiteral("gearMenu"));
        QAction* custom = actionNamed(gear, QStringLiteral("Custom"));
        QAction* system = actionNamed(gear, QStringLiteral("System"));
        QVERIFY(custom && system && custom->isCheckable() && system->isCheckable());
        if (WindowChrome::customMode() == WindowChrome::Mode::System) {
            QVERIFY(!custom->isEnabled());
            QSKIP("this build's Qt has no custom frame on this platform");
        }
        atReferenceSize();
        m_window->move(30, 20);
        QTRY_COMPARE(m_window->pos(), QPoint(30, 20));
        m_window->showPage(QStringLiteral("gate"));
        activate();
        railTab(QStringLiteral("Gate"))->setFocus();
        auto* stack = m_window->findChild<QStackedWidget*>();
        const QRect before = m_window->geometry();
        const QSize minimum = m_window->minimumSizeHint();
        const Qt::WindowFlags plain = m_window->windowFlags();

        custom->trigger();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        QCOMPARE(m_window->windowFlags(), WindowChrome::flagsFor(plain, WindowChrome::customMode()));
        QTRY_COMPARE(m_window->geometry(), before);
        QCOMPARE(m_window->currentPage(), QStringLiteral("gate"));
        QVERIFY(railTab(QStringLiteral("Gate"))->isChecked());
        // The keyboard's place in the window, which it gets back on being activated again.
        // (Offscreen never activates a window shown again: its focus window never changed.)
        QCOMPARE(m_window->focusWidget(), railTab(QStringLiteral("Gate")));
        QVERIFY(custom->isChecked() && !system->isChecked());
        QCOMPARE(savedWindow().frame, QStringLiteral("Custom"));  // saved at once, not on the way out
        // The header keeps its height with the chips in it, so the pages stay on AetherRX's
        // pixels. (Offscreen there is no title bar, so a Mac's inset is 0 here.)
        QCOMPARE(stack->mapTo(m_window.get(), QPoint()), QPoint(203, 53));
        QCOMPARE(m_window->minimumSizeHint(), minimum);  // nor do the chips widen it
        shot(m_window.get(), frameless ? QStringLiteral("frame-frameless-gate") : QStringLiteral("frame-custom-gate"));

        system->trigger();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        QCOMPARE(m_window->windowFlags(), plain);
        QTRY_COMPARE(m_window->geometry(), before);
        QCOMPARE(m_window->currentPage(), QStringLiteral("gate"));
        QCOMPARE(savedWindow().frame, QStringLiteral("System"));  // chosen, so no longer the default
        QCOMPARE(stack->mapTo(m_window.get(), QPoint()), QPoint(203, 53));
        if (QToolButton* close = m_window->findChild<QToolButton*>(QStringLiteral("headerClose"))) {
            QVERIFY(close->isHidden());
        }

        // Maximised, it stays maximised, and still returns to the same normal size.
        m_window->showMaximized();
        QTRY_VERIFY(m_window->isMaximized());
        custom->trigger();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        QTRY_VERIFY(m_window->isMaximized());
        QCOMPARE(m_window->normalGeometry(), before);
        system->trigger();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        QTRY_VERIFY(m_window->isMaximized());
        m_window->showNormal();
        QTRY_COMPARE(m_window->geometry(), before);
        QCOMPARE(m_quits, 0);
        // One resizer however often the frame changes.
        QCOMPARE(m_window->findChildren<AetherSDR::FramelessResizer*>().size(), frameless ? 1 : 0);
    }

    // The frameless Custom of Windows and Linux, on any machine: minimise and close chips at
    // the end of the header, as tall as its row and off the keyboard's path, the close chip
    // going out through closeEvent like every other way; and the header's empty part moving
    // the window, double-clicked maximising it.
    void aFramelessWindowHasItsOwnChipsAndHandle()
    {
        const FramelessCustom asOnWindows(true);
        Settings::WindowState window;
        window.frame = QStringLiteral("Custom");
        make(window);
        atReferenceSize();
        QVERIFY(m_window->windowFlags() & Qt::FramelessWindowHint);
        auto* minimise = m_window->findChild<QToolButton*>(QStringLiteral("headerMinimise"));
        auto* close = m_window->findChild<QToolButton*>(QStringLiteral("headerClose"));
        auto* title = m_window->findChild<QLabel*>(QStringLiteral("canonHeaderTitle"));
        auto* status = m_window->findChild<StatusChip*>();
        QVERIFY(minimise && close && title && status);
        for (QToolButton* chip : {minimise, close}) {
            QVERIFY(chip->isVisible());
            QCOMPARE(chip->focusPolicy(), Qt::NoFocus);
            QVERIFY2(chip->height() <= title->height(), qPrintable(QString::number(chip->height())));
        }
        QCOMPARE(close->text(), QStringLiteral("✕"));
        QCOMPARE(close->accessibleName(), QStringLiteral("Close"));
        QCOMPARE(minimise->accessibleName(), QStringLiteral("Minimise"));
        // Last in the row, in that order, at the window's right margin.
        const int statusRight = status->mapTo(m_window.get(), QPoint(status->width(), 0)).x();
        const int minimiseLeft = minimise->mapTo(m_window.get(), QPoint()).x();
        const int closeLeft = close->mapTo(m_window.get(), QPoint()).x();
        QVERIFY2(statusRight < minimiseLeft && minimiseLeft < closeLeft,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(statusRight).arg(minimiseLeft).arg(closeLeft)));
        QCOMPARE(closeLeft + close->width(), m_window->width() - 15);
        QCOMPARE(m_window->findChildren<AetherSDR::FramelessResizer*>().size(), 1);
        shot(m_window.get(), QStringLiteral("frame-frameless-radio"));

        // Dragging the title moves the window.
        m_window->move(40, 30);
        QTRY_COMPARE(m_window->pos(), QPoint(40, 30));
        QTest::mousePress(title, Qt::LeftButton, Qt::NoModifier, QPoint(4, 4));
        QTest::mouseMove(title, QPoint(29, 19));
        QTest::mouseRelease(title, Qt::LeftButton, Qt::NoModifier, QPoint(29, 19));
        QTRY_COMPARE(m_window->pos(), QPoint(65, 45));
        QCOMPARE(QWidget::mouseGrabber(), nullptr);
        // A control keeps its own clicks: the status chip opens its page, nothing moves.
        QTest::mouseClick(status, Qt::LeftButton);
        QCOMPARE(m_window->pos(), QPoint(65, 45));

        // Double-clicking the header's empty part maximises, and again restores.
        QTest::mouseDClick(title, Qt::LeftButton, Qt::NoModifier, QPoint(4, 4));
        QTRY_VERIFY(m_window->isMaximized());
        QTest::mouseDClick(title, Qt::LeftButton, Qt::NoModifier, QPoint(4, 4));
        QTRY_VERIFY(!m_window->isMaximized());

        minimise->click();
        QTRY_VERIFY(m_window->isMinimized());
        m_window->showNormal();
        QTRY_VERIFY(!m_window->isMinimized());

        CloseCounter counter;
        m_window->installEventFilter(&counter);
        m_window->showPage(QStringLiteral("eq"));
        close->click();
        QTRY_COMPARE(counter.closes, 1);
        QCOMPARE(m_quits, 1);
        QVERIFY(!m_window->isVisible());
        QCOMPARE(savedWindow().page, QStringLiteral("eq"));  // closeEvent saved on the way out
        QCOMPARE(savedWindow().frame, QStringLiteral("Custom"));
    }

    // Everything the header can hold at once, in one row on the title's line, left to right:
    // the title, the profile and its Modified chip, REC's and PLAY's clocks, the status chip,
    // the exact-values button and, without a system frame, minimise and close. The rail and
    // the pages start where they do with the system's frame.
    void theHeaderHoldsEverythingInOneRow()
    {
        const FramelessCustom asOnWindows(true);
        Settings::WindowState window;
        window.frame = QStringLiteral("Custom");
        make(window);
        atReferenceSize();
        m_window->showPage(QStringLiteral("gate"));
        control(QStringLiteral("gate.threshold"))->setValue(-30.0);
        quieten();
        EngineStatus s;
        s.recording.active = true;
        s.recording.seconds = 83.4;
        s.recording.path = QStringLiteral("rec.wav");
        s.playback.active = true;
        s.playback.seconds = 12.2;
        s.playback.totalSeconds = 83.9;
        s.lastRecordingAvailable = true;
        s.lastRecordingPath = QStringLiteral("last.wav");
        feed(s);

        auto* header = m_window->findChild<HeaderBar*>();
        QVERIFY(header);
        QCOMPARE(header->profileState(), QStringLiteral("Modified"));
        QCOMPARE(header->recordingClock(), QStringLiteral("● REC 01:23"));
        QCOMPARE(header->playbackClock(), QStringLiteral("▶ 00:12 / 01:23"));
        // A clock cut short still says which it is, once the row has been laid out again.
        auto* recordClock = m_window->findChild<QLabel*>(QStringLiteral("recordClock"));
        auto* playClock = m_window->findChild<QLabel*>(QStringLiteral("playClock"));
        QVERIFY(recordClock && playClock);
        QTRY_VERIFY2(recordClock->text().startsWith(QStringLiteral("● ")) && playClock->text().startsWith(QStringLiteral("▶ "))
                         && !recordClock->text().contains(QStringLiteral("…"))
                         && !playClock->text().contains(QStringLiteral("…")),
                     qPrintable(recordClock->text() + QStringLiteral(" | ") + playClock->text()));
        const QList<QWidget*> row = {
            m_window->findChild<QLabel*>(QStringLiteral("canonHeaderTitle")),
            m_window->findChild<QComboBox*>(QStringLiteral("headerProfile")),
            m_window->findChild<QLabel*>(QStringLiteral("profileState")),
            m_window->findChild<QLabel*>(QStringLiteral("recordClock")),
            m_window->findChild<QLabel*>(QStringLiteral("playClock")),
            m_window->findChild<StatusChip*>(),
            m_window->findChild<QPushButton*>(QStringLiteral("exactValues")),
            m_window->findChild<QToolButton*>(QStringLiteral("headerMinimise")),
            m_window->findChild<QToolButton*>(QStringLiteral("headerClose")),
        };
        const QWidget* title = row.first();
        QVERIFY(title);
        const QRect line(title->mapTo(m_window.get(), QPoint()), title->size());
        int right = 0;
        for (int i = 0; i < row.size(); ++i) {
            QWidget* w = row.at(i);
            QVERIFY2(w && w->isVisible(), qPrintable(QString::number(i)));
            const QRect at(w->mapTo(m_window.get(), QPoint()), w->size());
            qInfo().noquote() << w->objectName() << at << (qobject_cast<QLabel*>(w) ? qobject_cast<QLabel*>(w)->text() : QString());
            QVERIFY2(at.width() > 0, qPrintable(w->objectName()));
            QVERIFY2(at.left() >= right, qPrintable(w->objectName() + QStringLiteral(" overlaps what is before it")));
            QVERIFY2(at.top() >= line.top() && at.bottom() <= line.bottom(),
                     qPrintable(w->objectName() + QStringLiteral(" leaves the title's line")));
            right = at.right() + 1;
        }
        QCOMPARE(right, m_window->width() - 15);  // the close chip ends at the window's margin
        shot(m_window.get(), QStringLiteral("frame-frameless-everything"));

        auto* stack = m_window->findChild<QStackedWidget*>();
        QVERIFY(stack);
        QCOMPARE(stack->mapTo(m_window.get(), QPoint()), QPoint(203, 53));
        QCOMPARE(rail()->mapTo(m_window.get(), QPoint()), QPoint(15, 53));
    }

    // The header's chips on their own: the row keeps the title's height with them in it.
    void theHeadersChipsKeepItsHeight()
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* header = new HeaderBar(QStringLiteral("ExpertSDR3 Audio"));
        layout->addWidget(header);
        Appearance::apply(&host);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        const int height = header->sizeHint().height();
        header->setWindowButtons(false);  // never wanted: never made
        QVERIFY(!header->findChild<QToolButton*>());
        header->setWindowButtons(true);
        QCOMPARE(header->findChildren<QToolButton*>().size(), 2);
        QCoreApplication::processEvents();
        QCOMPARE(header->sizeHint().height(), height);
        header->setWindowButtons(false);
        for (QToolButton* chip : header->findChildren<QToolButton*>()) QVERIFY(chip->isHidden());
        header->setWindowButtons(true);
        QCOMPARE(header->findChildren<QToolButton*>().size(), 2);  // the same two, shown again
        // Round when the pointer is over them, as upstream's are. Qt draws a sheet's corners
        // square once two radii add up to more than the side, so the radius follows the side.
        for (QToolButton* chip : header->findChildren<QToolButton*>()) {
            chip->setAttribute(Qt::WA_UnderMouse, true);
            const QImage image = chip->grab().toImage();
            chip->setAttribute(Qt::WA_UnderMouse, false);
            const qreal dpr = image.devicePixelRatio();
            const QColor corner = image.pixelColor(QPoint(1, 1) * dpr);
            const QColor middle = image.pixelColor(QPoint(chip->width() / 2, chip->height() / 4) * dpr);
            QVERIFY2(middle.alpha() == 255, qPrintable(chip->objectName() + QStringLiteral(" has no hover fill")));
            QVERIFY2(corner.alpha() == 0, qPrintable(chip->objectName() + QStringLiteral(" is square: ")
                                                     + corner.name(QColor::HexArgb)));
        }
        CloseCounter counter;
        host.installEventFilter(&counter);
        header->findChild<QToolButton*>(QStringLiteral("headerClose"))->click();
        QCOMPARE(counter.closes, 1);  // the window it is in, through close()
        QVERIFY(!host.isVisible());
    }

    // ── Menus ─────────────────────────────────────────────────────────────────

    void theGearMenuHasEverythingTheMenuBarHas()
    {
        make();
        auto* gear = m_window->findChild<QMenu*>(QStringLiteral("gearMenu"));
        QVERIFY(gear);
        const QList<QAction*> inGear = actionsIn(gear);
        const auto menus = m_window->menuBar()->actions();
        int count = 0;
        for (QAction* menu : menus) {
            QVERIFY(menu->menu());
            const auto actions = actionsIn(menu->menu());
            for (QAction* action : actions) {
                ++count;
                QVERIFY2(inGear.contains(action), qPrintable(action->text()));
            }
        }
        QCOMPARE(count, 9);  // Find Radios, the six profile actions, the website and About
        QVERIFY(actionNamed(gear, QStringLiteral("Reset stage order")));

        if (!m_window->menuBar()->isNativeMenuBar()) {
            // A visible bar would paint a strip across the ground.
            QVERIFY(m_window->menuBar()->isHidden());
            for (QAction* menu : menus) {
                const auto actions = actionsIn(menu->menu());
                for (QAction* action : actions) QVERIFY2(m_window->actions().contains(action), qPrintable(action->text()));
            }
        }
    }

    // The gear's own rows, top to bottom: the profiles and the radio first, then the page's,
    // the recordings and the window's, then About and the way out.
    void theGearMenuIsInItsOrder()
    {
        make();
        auto* gear = m_window->findChild<QMenu*>(QStringLiteral("gearMenu"));
        QVERIFY(gear);
        QStringList rows;
        const auto actions = gear->actions();
        for (QAction* action : actions) {
            if (!action->isSeparator()) rows << action->text();
        }
        QStringList want = {QStringLiteral("Profiles"),
                            QStringLiteral("Find Radios..."),
                            QStringLiteral("Exact values..."),
                            QStringLiteral("Reset stage order"),
                            QStringLiteral("Recordings"),
                            QStringLiteral("Window frame"),
                            QStringLiteral("About %1").arg(QString::fromUtf8(kAppDisplayName)),
                            QStringLiteral("Project Website")};
#ifndef Q_OS_MACOS
        want << QStringLiteral("Quit");  // a Mac has it in the application menu
#endif
        QCOMPARE(rows, want);
        auto* settings = m_window->findChild<QPushButton*>(QStringLiteral("aetherRxSettingsButton"));
        QVERIFY(settings);
        for (const char* word : {"Profiles", "Find Radios", "exact values", "stage order", "recordings", "window frame",
                                 "About"}) {
            QVERIFY2(settings->toolTip().contains(QLatin1String(word)), word);
        }
    }

    void theMenuShortcutsFireWithTheBarHidden()
    {
        make();
        if (m_window->menuBar()->isNativeMenuBar()) QSKIP("a native menu bar handles its own shortcuts");
        activate();
        auto* gear = m_window->findChild<QMenu*>(QStringLiteral("gearMenu"));

        // Save keeps a named profile's changes, with no dialog.
        QVERIFY(m_window->saveProfileAs(QStringLiteral("Mine")));
        control(QStringLiteral("comp.threshold"))->setValue(-25);
        QVERIFY(m_window->chainModified());
        QAction* save = actionNamed(gear, QStringLiteral("Save"));
        QVERIFY(save && !save->shortcut().isEmpty());
        QSignalSpy saved(save, &QAction::triggered);
        QTest::keySequence(m_window.get(), save->shortcut());
        QCOMPARE(saved.count(), 1);
        QVERIFY(!m_window->chainModified());

        // Save As asks for a name: the dialog it opens is sent away unanswered.
        QAction* saveAs = actionNamed(gear, QStringLiteral("Save As..."));
        QVERIFY(saveAs);
        if (!saveAs->shortcut().isEmpty()) {
            QSignalSpy asked(saveAs, &QAction::triggered);
            QTimer::singleShot(50, [] {
                if (QWidget* dialog = QApplication::activeModalWidget()) dialog->close();
            });
            QTest::keySequence(m_window.get(), saveAs->shortcut());
            QCOMPARE(asked.count(), 1);
        }

#ifndef Q_OS_MACOS
        QAction* quit = actionNamed(gear, QStringLiteral("Quit"));
        QVERIFY(quit);
        if (!quit->shortcut().isEmpty()) {
            QTest::keySequence(m_window.get(), quit->shortcut());
            QTRY_COMPARE(m_quits, 1);
        }
#endif
    }

    // Off a Mac the gear's menu is the only home of the profile commands, so one that cannot
    // run must look it. The application sheet's QWidget rule gives every state one colour.
    void aCommandThatCannotRunLooksDisabled()
    {
        make();  // the factory settings, unchanged, no saved profiles, the default order
        activate();

        rail()->findChild<QPushButton*>(QStringLiteral("aetherRxSettingsButton"))->click();
        QMenu* gear = openPopup();
        QVERIFY(gear);
        compareRows(gear, {QStringLiteral("Reset stage order")}, {QStringLiteral("Find Radios...")});
        if (QTest::currentTestFailed()) return;

        QMenu* profiles = nullptr;
        const auto entries = gear->actions();
        for (QAction* entry : entries) {
            if (entry->menu() && entry->text() == QLatin1String("Profiles")) profiles = entry->menu();
        }
        QVERIFY(profiles);
        profiles->popup(gear->mapToGlobal(gear->rect().topRight()));
        QTRY_VERIFY(profiles->isVisible());
        shot(profiles, QStringLiteral("profiles-menu"));
        compareRows(profiles,
                    {QStringLiteral("Save"), QStringLiteral("Export..."), QStringLiteral("Export All..."), QStringLiteral("Delete")},
                    {QStringLiteral("Save As..."), QStringLiteral("Import...")});
        profiles->close();
        gear->close();
        if (QTest::currentTestFailed()) return;

        // A stage tab's menu, on the stage at the top of the chain.
        activate();
        emit railTab(QStringLiteral("Gate"))->customContextMenuRequested(QPoint(10, 10));
        QMenu* stage = openPopup();
        QVERIFY(stage);
        compareRows(stage, {QStringLiteral("Move up"), QStringLiteral("Reset stage order")}, {QStringLiteral("Move down")});
        stage->close();
    }

    // ── The rail ──────────────────────────────────────────────────────────────

    void theRailHasItsRowsAndTheFooterBelowThem()
    {
        make();
        QCOMPARE(railRows(), (QStringList{QStringLiteral("Radio"), QStringLiteral("AetherNR"), QStringLiteral("Gate"),
                                          QStringLiteral("EQ"), QStringLiteral("Compressor"), QStringLiteral("Tube"),
                                          QStringLiteral("Exciter"), QStringLiteral("Final Output")}));
        // Grips and boxes for the chain; a box for AetherNR; neither for Radio or Final Output.
        for (const QString& slug : {QStringLiteral("Gate"), QStringLiteral("EQ"), QStringLiteral("Compressor"),
                                    QStringLiteral("Tube"), QStringLiteral("Exciter")}) {
            QVERIFY2(rail()->findChild<QWidget*>(QStringLiteral("aetherRxGrip") + slug), qPrintable(slug));
            QVERIFY2(rail()->findChild<QCheckBox*>(QStringLiteral("aetherRxEnable") + slug), qPrintable(slug));
        }
        QVERIFY(rail()->findChild<QCheckBox*>(QStringLiteral("aetherRxEnableAetherNR")));
        for (const QString& slug : {QStringLiteral("Radio"), QStringLiteral("AetherNR"), QStringLiteral("FinalOutput")}) {
            QVERIFY2(!rail()->findChild<QWidget*>(QStringLiteral("aetherRxGrip") + slug), qPrintable(slug));
        }
        QVERIFY(!rail()->findChild<QCheckBox*>(QStringLiteral("aetherRxEnableRadio")));
        QVERIFY(!rail()->findChild<QCheckBox*>(QStringLiteral("aetherRxEnableFinalOutput")));

        // The footer, as in AetherRX: REC and PLAY on a row of their own, then the gear and
        // BYPASS sharing the last row.
        auto* gear = rail()->findChild<QPushButton*>(QStringLiteral("aetherRxSettingsButton"));
        auto* bypass = rail()->findChild<QPushButton*>(QStringLiteral("aetherRxBypass"));
        QVERIFY(gear && bypass);
        QCOMPARE(bypass->text(), QStringLiteral("BYPASS"));
        QVERIFY(bypass->isCheckable() && !bypass->isChecked());
        QCOMPARE(gear->parentWidget(), bypass->parentWidget());
        auto* record = rail()->findChild<QPushButton*>(QStringLiteral("aetherRxRecord"));
        auto* play = rail()->findChild<QPushButton*>(QStringLiteral("aetherRxPlay"));
        QVERIFY(record && play);
        QCOMPARE(record->text(), QStringLiteral("REC"));
        QCOMPARE(play->text(), QStringLiteral("PLAY"));
        QCOMPARE(record->parentWidget(), play->parentWidget());
        QVERIFY(record->parentWidget() != bypass->parentWidget());
        const auto at = [this](QWidget* w) { return w->mapTo(m_window.get(), QPoint()); };
        QCOMPARE(at(record).y(), at(play).y());
        QVERIFY(at(record).x() < at(play).x());
        QVERIFY(at(gear).x() < at(bypass).x());
        QCOMPARE(at(gear).y(), at(bypass).y());
        QVERIFY2(at(record).y() + record->height() <= at(bypass).y(),
                 qPrintable(QStringLiteral("REC at %1, BYPASS at %2").arg(at(record).y()).arg(at(bypass).y())));
        // Below the last stage row.
        QVERIFY(at(railTab(QStringLiteral("Final Output"))).y() < at(record).y());
    }

    void aTabOpensItsPage()
    {
        make();
        railTab(QStringLiteral("Compressor"))->click();
        QCOMPARE(m_window->currentPage(), QStringLiteral("comp"));
        railTab(QStringLiteral("Final Output"))->click();
        QCOMPARE(m_window->currentPage(), QStringLiteral("output"));
    }

    void theRailBoxesAreTheStagesEnables()
    {
        make();
        auto* box = rail()->findChild<QCheckBox*>(QStringLiteral("aetherRxEnableGate"));
        QVERIFY(!box->isChecked());
        box->click();
        QCOMPARE(control(QStringLiteral("gate.enabled"))->value(), 1.0);
        QVERIFY(m_window->chainModified());
        control(QStringLiteral("gate.enabled"))->setValue(0);
        QVERIFY(!box->isChecked());
        QVERIFY(!m_window->chainModified());
    }

    void theAetherNrBoxBringsBackTheLastMethod()
    {
        // The AetherNR page's method tabs that are lit, by method id: the method NR runs.
        const auto lit = [this] {
            QStringList ids;
            const auto buttons = m_window->findChildren<QPushButton*>();
            for (QPushButton* b : buttons) {
                if (b->objectName().startsWith(QLatin1String("nrTab_")) && b->isChecked()) ids << b->objectName().mid(6);
            }
            return ids;
        };
        const auto tab = [this](const QString& id) {
            return m_window->findChild<QPushButton*>(QStringLiteral("nrTab_") + id);
        };
        make();
        auto* box = rail()->findChild<QCheckBox*>(QStringLiteral("aetherRxEnableAetherNR"));
        QVERIFY(box && tab(QStringLiteral("rn2")));
        QVERIFY(!box->isChecked());
        QCOMPARE(lit(), QStringList());

        box->click();  // never chosen before: NR2
        QCOMPARE(lit(), QStringList{QStringLiteral("nr2")});
        tab(QStringLiteral("rn2"))->click();
        QVERIFY(box->isChecked());
        box->click();
        QCOMPARE(lit(), QStringList());
        QVERIFY(!box->isChecked());
        QCOMPARE(Settings(iniPath()).lastNrMethod(), QStringLiteral("rn2"));
        box->click();
        QCOMPARE(lit(), QStringList{QStringLiteral("rn2")});

        // A later session remembers it too.
        tab(QStringLiteral("rn2"))->click();  // the lit tab: off
        QCOMPARE(lit(), QStringList());
        make();
        box = rail()->findChild<QCheckBox*>(QStringLiteral("aetherRxEnableAetherNR"));
        box->click();
        QCOMPARE(lit(), QStringList{QStringLiteral("rn2")});
    }

    void theStageMenuAndAltArrowsMoveAStage()
    {
        make();
        activate();
        const QStringList defaults = RxChain::stageIds();
        QCOMPARE(m_window->stageOrder(), defaults);

        // The tab's menu: Move up.
        QPushButton* eq = railTab(QStringLiteral("EQ"));
        emit eq->customContextMenuRequested(QPoint(10, 10));
        QMenu* menu = openPopup();
        QVERIFY(menu);
        shot(menu, QStringLiteral("stage-menu"));
        QAction* up = actionNamed(menu, QStringLiteral("Move up"));
        QVERIFY(up && up->isEnabled());
        up->trigger();
        menu->close();
        const QStringList eqFirst = {QStringLiteral("eq"), QStringLiteral("gate"), QStringLiteral("comp"),
                                     QStringLiteral("tube"), QStringLiteral("pudu")};
        QCOMPARE(m_window->stageOrder(), eqFirst);
        QCOMPARE(railRows().mid(2, 2), (QStringList{QStringLiteral("EQ"), QStringLiteral("Gate")}));
        QVERIFY(m_window->chainModified());

        // At the top already: Move up is off, and Alt+Up does nothing.
        emit eq->customContextMenuRequested(QPoint(10, 10));
        menu = openPopup();
        QVERIFY(!actionNamed(menu, QStringLiteral("Move up"))->isEnabled());
        menu->close();
        activate();  // the menu had the keyboard
        eq->setFocus();
        QTest::keyClick(eq, Qt::Key_Up, Qt::AltModifier);
        QCOMPARE(m_window->stageOrder(), eqFirst);

        // Alt+Down puts it back, and the tab keeps the keyboard.
        QTest::keyClick(eq, Qt::Key_Down, Qt::AltModifier);
        QCOMPARE(m_window->stageOrder(), defaults);
        QCOMPARE(QApplication::focusWidget(), eq);
        QVERIFY(!m_window->chainModified());

        // Down to the end, past which it stops.
        for (int i = 0; i < 6; ++i) QTest::keyClick(eq, Qt::Key_Down, Qt::AltModifier);
        QCOMPARE(m_window->stageOrder().last(), QStringLiteral("eq"));

        // Reset stage order, from the gear.
        actionNamed(m_window->findChild<QMenu*>(QStringLiteral("gearMenu")), QStringLiteral("Reset stage order"))->trigger();
        QCOMPARE(m_window->stageOrder(), defaults);

        // And the order is what is saved.
        QTest::keyClick(railTab(QStringLiteral("Tube")), Qt::Key_Up, Qt::AltModifier);
        m_window->close();
        QSettings ini(iniPath(), QSettings::IniFormat);
        QCOMPARE(ini.value(QStringLiteral("chain/order")).toStringList(),
                 (QStringList{QStringLiteral("gate"), QStringLiteral("eq"), QStringLiteral("tube"), QStringLiteral("comp"),
                              QStringLiteral("pudu")}));
    }

    void thePinnedRowsNeverMove()
    {
        make();
        activate();
        const QStringList before = railRows();
        for (const QString& label : {QStringLiteral("Radio"), QStringLiteral("AetherNR"), QStringLiteral("Final Output")}) {
            QPushButton* tab = railTab(label);
            QCOMPARE(tab->contextMenuPolicy() == Qt::CustomContextMenu, false);  // no menu to move it with
            tab->setFocus();
            QTest::keyClick(tab, Qt::Key_Up, Qt::AltModifier);
            QTest::keyClick(tab, Qt::Key_Down, Qt::AltModifier);
        }
        QCOMPARE(railRows(), before);
        QCOMPARE(m_window->stageOrder(), RxChain::stageIds());

        // Moving every chain stage to each end leaves them where they were declared.
        for (const QString& label : {QStringLiteral("Exciter"), QStringLiteral("Gate")}) {
            for (int i = 0; i < 5; ++i) QTest::keyClick(railTab(label), Qt::Key_Up, Qt::AltModifier);
            const QStringList rows = railRows();
            QCOMPARE(rows.first(), QStringLiteral("Radio"));
            QCOMPARE(rows.at(1), QStringLiteral("AetherNR"));
            QCOMPARE(rows.last(), QStringLiteral("Final Output"));
        }
    }

    // A grip's drag cannot run offscreen (it is a nested drag loop), so the drop arrives as
    // upstream's own test delivers it.
    void aDropOnTheRailReordersOnlyTheChain()
    {
        make();
        atReferenceSize();
        AetherSDR::StageTabBar* bar = rail();
        const int foot = railTab(QStringLiteral("Final Output"))->mapTo(bar, QPoint()).y() - 2;

        QVERIFY(deliverDrop(*bar, bar->dragMimeType(), kRailGate, foot));
        QCOMPARE(m_window->stageOrder().last(), QStringLiteral("gate"));
        QCOMPARE(railRows().at(6), QStringLiteral("Gate"));
        QVERIFY(m_window->chainModified());

        // The rows outside the chain carry no grip, and a drop naming one changes nothing.
        const QStringList order = m_window->stageOrder();
        for (int pinned : {int(kRailRadio), int(kRailNr), int(kRailOutput)}) {
            deliverDrop(*bar, bar->dragMimeType(), pinned, 4);
            QCOMPARE(m_window->stageOrder(), order);
        }
    }

    // ── Upstream's rail tests, ported ─────────────────────────────────────────
    // From AetherSDR tests/stage_tab_bar_drag_test.cpp at 141231e4: a bar takes drops only
    // under its own window's MIME type, so a drag from any other bar cannot reorder it.

    void aBarAcceptsItsOwnWindowsDrag()
    {
        LoneBar bar(QStringLiteral("elsewhere"), {1, 2, 3});
        QVERIFY(deliverDrop(bar.bar, bar.bar.dragMimeType(), 1, bar.bar.height() - 10));
        QCOMPARE(bar.commits, 1);
        QCOMPARE(bar.order.last(), 1);  // dropped at the foot, so it is last
    }

    void anotherBarsDragCannotReorderTheRail()
    {
        LoneBar ours(QStringLiteral("aetherRx"), {1, 2, 3});
        const QVector<int> before = ours.order;
        LoneBar other(QStringLiteral("elsewhere"), {1, 2, 3});
        QVERIFY(!deliverDrop(ours.bar, other.bar.dragMimeType(), 1, ours.bar.height() - 10));
        QCOMPARE(ours.commits, 0);
        QCOMPARE(ours.order, before);
        // Nor the other way.
        QVERIFY(!deliverDrop(other.bar, ours.bar.dragMimeType(), 1, other.bar.height() - 10));
        QCOMPARE(other.commits, 0);
    }

    void twoBarsDoNotShareAMimeType()
    {
        LoneBar ours(QStringLiteral("aetherRx"), {1});
        LoneBar other(QStringLiteral("elsewhere"), {1});
        QVERIFY2(ours.bar.dragMimeType() != other.bar.dragMimeType(), qPrintable(ours.bar.dragMimeType()));
    }

    // From AetherSDR tests/rx_stage_reorder_test.cpp at 141231e4: the arithmetic behind a
    // drop, five rows 40 px apart with the first centred at 20.
    void theDropRule_data()
    {
        QTest::addColumn<int>("moved");
        QTest::addColumn<int>("dropY");
        QTest::addColumn<QVector<int>>("expected");
        enum { Eq = 1, Gate = 2, Comp = 3, Tube = 4, Pudu = 5 };
        QTest::newRow("above every row goes to the front") << int(Pudu) << 0 << QVector<int>{Pudu, Eq, Gate, Comp, Tube};
        QTest::newRow("below every row goes to the end") << int(Eq) << 500 << QVector<int>{Gate, Comp, Tube, Pudu, Eq};
        QTest::newRow("its own row, above its middle") << int(Comp) << 95 << QVector<int>{Eq, Gate, Comp, Tube, Pudu};
        QTest::newRow("its own row, below its middle") << int(Comp) << 105 << QVector<int>{Eq, Gate, Comp, Tube, Pudu};
        QTest::newRow("moving down allows for its removal") << int(Eq) << 145 << QVector<int>{Gate, Comp, Tube, Eq, Pudu};
        QTest::newRow("moving up lands before the row") << int(Pudu) << 55 << QVector<int>{Eq, Pudu, Gate, Comp, Tube};
        QTest::newRow("a stage not in the order") << 99 << 100 << QVector<int>{Eq, Gate, Comp, Tube, Pudu};
    }
    void theDropRule()
    {
        QFETCH(int, moved);
        QFETCH(int, dropY);
        QFETCH(QVector<int>, expected);
        const QVector<int> mids{20, 60, 100, 140, 180};
        const QVector<int> order{1, 2, 3, 4, 5};
        QCOMPARE(AetherSDR::RxStageReorder::dropped(order, moved, mids, dropY), expected);
    }

    // ── The header ────────────────────────────────────────────────────────────

    void theStatusChipShowsTheMostSeriousState()
    {
        using Chip = StatusChip;
        EngineStatus s;
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("Not connected"));
        QCOMPARE(Chip::summarize(s).page, QStringLiteral("radio"));
        s.connection = TciClient::State::Connecting;
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("Connecting…"));
        QCOMPARE(Chip::summarize(s).tone, QStringLiteral("warning"));
        s.connection = TciClient::State::Ready;
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("No audio from radio"));
        s.audioFlowing = true;
        s.frequencyHz = 14200000;
        s.modulation = QStringLiteral("USB");
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("● 14.200 MHz USB · RX1"));
        QCOMPARE(Chip::summarize(s).tone, QStringLiteral("success"));
        s.nrBypassedByMode = true;
        s.modulation = QStringLiteral("CW");
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("NR bypassed: CW"));
        QCOMPARE(Chip::summarize(s).page, QStringLiteral("nr"));
        s.nrPreparing = true;
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("NR preparing…"));
        s.audioFlowing = false;  // more serious than NR's state
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("No audio from radio"));
        s.nrFailed = true;
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("NR failed: running dry"));
        QCOMPARE(Chip::summarize(s).tone, QStringLiteral("danger"));
        s.dspLoad = 0.9;
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("Can't keep up: try NR2"));
        s.mutePhase = MuteController::Phase::Failed;
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("ExpertSDR3 mute failed"));
        QCOMPARE(Chip::summarize(s).page, QStringLiteral("radio"));
        s.outputError = QStringLiteral("Device gone");
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("Output: Device gone"));
        QCOMPARE(Chip::summarize(s).page, QStringLiteral("output"));

        // In the window: the tooltip has today's messages in full, and a click opens the page.
        make();
        auto* chip = m_window->findChild<StatusChip*>();
        QVERIFY(chip);
        chip->showStatus(s);
        QVERIFY(chip->toolTip().contains(QStringLiteral("ExpertSDR3 did not respond to mute")));
        QVERIFY(chip->toolTip().contains(QStringLiteral("Device gone")));
        chip->click();
        QCOMPARE(m_window->currentPage(), QStringLiteral("output"));
        EngineStatus quiet;
        chip->showStatus(quiet);
        chip->click();
        QCOMPARE(m_window->currentPage(), QStringLiteral("radio"));
    }

    // A recording that stopped early is red, above everything from the connection down, until
    // its warning is answered; one losing audio to a slow disk is amber while it lasts. Neither
    // has a page to open. A REC that was turned down is neither.
    void theStatusChipShowsRecordingTrouble()
    {
        using Chip = StatusChip;
        EngineStatus s;
        s.connection = TciClient::State::Ready;
        s.audioFlowing = true;
        s.recording.active = true;
        s.recording.path = QStringLiteral("/rec/a.wav");
        s.recording.droppedFrames = 480;
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("Recording dropped audio"));
        QCOMPARE(Chip::summarize(s).tone, QStringLiteral("warning"));
        QVERIFY(Chip::summarize(s).page.isEmpty());
        s.nrPreparing = true;  // less serious than losing audio
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("Recording dropped audio"));
        s.audioFlowing = false;  // more serious
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("No audio from radio"));
        s.audioFlowing = true;
        s.recording.active = false;  // the count stays, but nothing is being lost now
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("NR preparing…"));

        s.recording.error = QStringLiteral("Could not write a.wav: No space left on device");
        QCOMPARE(Chip::summarize(s).text,
                 QStringLiteral("Recording stopped: Could not write a.wav: No space left on device"));
        QCOMPARE(Chip::summarize(s).tone, QStringLiteral("danger"));
        QVERIFY(Chip::summarize(s).page.isEmpty());
        s.connection = TciClient::State::Disconnected;  // still the more serious
        QCOMPARE(Chip::summarize(s).text.left(18), QStringLiteral("Recording stopped:"));
        // Once its warning is answered, it is the connection's again.
        QCOMPARE(Chip::summarize(s, true).text, QStringLiteral("Not connected"));
        QCOMPARE(Chip::summarize(s, true).page, QStringLiteral("radio"));
        s.nrFailed = true;
        QCOMPARE(Chip::summarize(s).text, QStringLiteral("NR failed: running dry"));
        s.nrFailed = false;

        // The tooltip has the reason in full and where the file is.
        make();
        auto* chip = m_window->findChild<StatusChip*>();
        QVERIFY(chip);
        chip->showStatus(s);
        QVERIFY(chip->toolTip().contains(QStringLiteral("No space left on device")));
        QVERIFY(chip->toolTip().contains(QStringLiteral("/rec/a.wav")));
        const QString before = m_window->currentPage();
        chip->click();
        QCOMPARE(m_window->currentPage(), before);

        // REC pressed with no radio: nothing was recorded, so nothing stopped. The chip keeps
        // to the connection, and the tooltip says why REC did not start.
        EngineStatus refused;
        refused.recording.error = QStringLiteral("Connect to a radio first");
        refused.recording.refused = true;
        QCOMPARE(Chip::summarize(refused).text, QStringLiteral("Not connected"));
        QCOMPARE(Chip::summarize(refused).page, QStringLiteral("radio"));
        chip->showStatus(refused);
        QVERIFY2(chip->toolTip().contains(QStringLiteral("Not recording: Connect to a radio first")),
                 qPrintable(chip->toolTip()));
        QVERIFY(!chip->toolTip().contains(QStringLiteral("Recording stopped")));
    }

    // The From radio meter is AetherSDR's level strip, read from the engine's taps by a timer
    // that runs only while the Radio page is on screen.
    void theRadioPageMetersTheTapsWhileItIsShown()
    {
        make();
        const std::shared_ptr<LiveTaps> taps = m_engine->taps();
        auto* meter = m_window->findChild<QWidget*>(QStringLiteral("inputLevel"));
        QVERIFY(meter);
        QCOMPARE(meter->metaObject()->className(), "AetherSDR::ClientEqOutputFader");
        // Made for the EQ's output, it says OUT and explains the output; corrected from
        // outside to say what it measures here.
        QStringList caps;
        for (QLabel* label : meter->findChildren<QLabel*>()) caps << label->text();
        QVERIFY2(caps.contains(QStringLiteral("IN")), qPrintable(caps.join(u',')));
        QVERIFY2(!caps.contains(QStringLiteral("OUT")), qPrintable(caps.join(u',')));
        QVERIFY2(meter->toolTip().contains(QStringLiteral("arriving from ExpertSDR3")), qPrintable(meter->toolTip()));

        m_window->showPage(QStringLiteral("gate"));
        QCoreApplication::processEvents();
        taps->inputPeak.store(0.5f);
        QTest::qWait(150);  // several of its ticks, were it running
        QCOMPARE(taps->inputPeak.load(), 0.5f);

        m_window->showPage(QStringLiteral("radio"));
        taps->inputPeak.store(0.5f);
        QTRY_COMPARE(taps->inputPeak.load(), 0.0f);
        // The reading is the strip's own cap: -6 dB, rising towards it from silence.
        QLabel* reading = nullptr;
        for (QLabel* label : meter->findChildren<QLabel*>()) {
            if (label->text().endsWith(QLatin1String(" dB")) || label->text() == QLatin1String("-inf")) reading = label;
        }
        QVERIFY(reading);
        QVERIFY2(reading->text() != QLatin1String("-inf"), qPrintable(reading->text()));
    }

    void theHeaderShowsTheNameAndTheProfile()
    {
        make();
        QCOMPARE(m_window->windowTitle(), QStringLiteral("ExpertSDR3 Audio"));
        auto* title = m_window->findChild<QLabel*>(QStringLiteral("canonHeaderTitle"));
        QVERIFY(title);
        QCOMPARE(title->text(), QStringLiteral("ExpertSDR3 Audio"));
        auto* profiles = m_window->findChild<QComboBox*>(QStringLiteral("headerProfile"));
        QVERIFY(profiles);
        QCOMPARE(profiles->itemText(0), QStringLiteral("Factory settings"));
        profiles->showPopup();
        shot(profiles->view()->window(), QStringLiteral("profile-list"));
        profiles->hidePopup();
        auto* state = m_window->findChild<QLabel*>(QStringLiteral("profileState"));
        QVERIFY(state);
        QVERIFY(state->isHidden());
        control(QStringLiteral("tube.drive"))->setValue(3);
        QVERIFY(!state->isHidden());
        QCOMPARE(state->text(), QStringLiteral("Modified"));
    }

    // ── REC and PLAY ──────────────────────────────────────────────────────────

    void theHeaderClocksReadAsClocks()
    {
        QCOMPARE(HeaderBar::clockText(0.0), QStringLiteral("00:00"));
        QCOMPARE(HeaderBar::clockText(12.9), QStringLiteral("00:12"));  // whole seconds, as a clock counts them
        QCOMPARE(HeaderBar::clockText(83.4), QStringLiteral("01:23"));
        QCOMPARE(HeaderBar::clockText(599.0), QStringLiteral("09:59"));
        QCOMPARE(HeaderBar::clockText(60.0 - 1e-9), QStringLiteral("01:00"));  // summed from frames, a hair short
        QCOMPARE(HeaderBar::clockText(3599.0), QStringLiteral("59:59"));
        QCOMPARE(HeaderBar::clockText(3600.0), QStringLiteral("1:00:00"));
        QCOMPARE(HeaderBar::clockText(2.7 * 3600.0 + 5.0), QStringLiteral("2:42:05"));
        QCOMPARE(HeaderBar::clockText(-1.0), QStringLiteral("00:00"));
        QCOMPARE(HeaderBar::clockText(std::nan("")), QStringLiteral("00:00"));

        EngineStatus s;
        QVERIFY(HeaderBar::recordingText(s).isEmpty());
        QVERIFY(HeaderBar::playbackText(s).isEmpty());
        s.recording.active = true;
        s.recording.seconds = 83.4;
        QCOMPARE(HeaderBar::recordingText(s), QStringLiteral("● REC 01:23"));
        s.playback.active = true;
        s.playback.seconds = 12.7;
        QCOMPARE(HeaderBar::playbackText(s), QStringLiteral("▶ 00:12"));  // its length, once the file is read
        s.playback.totalSeconds = 83.9;
        QCOMPARE(HeaderBar::playbackText(s), QStringLiteral("▶ 00:12 / 01:23"));

        // In the header: in its one row and as tall as it, between the profile and the status
        // chip, and never widening the window however full the row is.
        make();
        atReferenceSize();
        control(QStringLiteral("tube.drive"))->setValue(3);  // "Modified" shows too
        QCoreApplication::processEvents();
        const int minimumWidth = m_window->minimumSizeHint().width();
        quieten();
        s.connection = TciClient::State::Ready;
        s.audioFlowing = true;
        s.frequencyHz = 14200000;
        s.modulation = QStringLiteral("USB");
        s.recording.path = m_dir.filePath(QStringLiteral("a.wav"));
        s.lastRecordingAvailable = true;
        s.playback.path = m_dir.filePath(QStringLiteral("b.wav"));
        // Finished while b plays: the next PLAY's.
        s.lastRecordingPath = m_dir.filePath(QStringLiteral("c.wav"));
        auto* header = m_window->findChild<HeaderBar*>();
        QVERIFY(header);
        auto* recordClock = header->findChild<QLabel*>(QStringLiteral("recordClock"));
        auto* playClock = header->findChild<QLabel*>(QStringLiteral("playClock"));
        auto* title = header->findChild<QLabel*>(QStringLiteral("canonHeaderTitle"));
        auto* state = header->findChild<QLabel*>(QStringLiteral("profileState"));
        auto* chip = header->statusChip();
        QComboBox* profiles = header->profileList();
        QVERIFY(recordClock && playClock && title && state && chip && profiles);

        // Recording, at the references' size, with a long status and Modified: the profile
        // combo gives up the room its name does not need before the chip is shortened, so the
        // name, the clock and the chip are all whole.
        EngineStatus recording = s;
        recording.playback.active = false;
        feed(recording);
        QTRY_COMPARE(recordClock->text(), QStringLiteral("● REC 01:23"));
        QVERIFY(playClock->isHidden());
        QTRY_COMPARE(chip->text(), chip->summary().text);
        QVERIFY2(profiles->width() < profiles->sizeHint().width(), qPrintable(QString::number(profiles->width())));
        QVERIFY(showsItsTextWhole(profiles));
        shot(header, QStringLiteral("header-recording"));
        // A name that needs all of the combo leaves nothing there to give: the chip makes way,
        // and the clock is still whole.
        profiles->addItem(QStringLiteral("Contest weekend, 40 metres"));
        profiles->setCurrentIndex(profiles->count() - 1);
        QTRY_VERIFY2(chip->text() != chip->summary().text, qPrintable(chip->text()));  // shortened
        QVERIFY(chip->text().endsWith(QStringLiteral("…")));
        QCOMPARE(recordClock->text(), QStringLiteral("● REC 01:23"));
        shot(header, QStringLiteral("header-recording-long-name"));
        profiles->setCurrentIndex(0);
        profiles->removeItem(profiles->count() - 1);
        QTRY_COMPARE(chip->text(), chip->summary().text);

        // Recording and playing: each clock shown whole or as its time alone, never cut short.
        feed(s);
        QCOMPARE(header->recordingClock(), QStringLiteral("● REC 01:23"));
        QCOMPARE(header->playbackClock(), QStringLiteral("▶ 00:12 / 01:23"));
        QVERIFY(recordClock->isVisible() && playClock->isVisible() && state->isVisible());
        QTRY_VERIFY2(!recordClock->text().contains(QStringLiteral("…")) && !playClock->text().contains(QStringLiteral("…")),
                     qPrintable(recordClock->text() + QStringLiteral(" | ") + playClock->text()));
        QVERIFY2(recordClock->text() == QStringLiteral("● REC 01:23") || recordClock->text() == QStringLiteral("● 01:23"),
                 qPrintable(recordClock->text()));
        QVERIFY2(playClock->text() == QStringLiteral("▶ 00:12 / 01:23") || playClock->text() == QStringLiteral("▶ 00:12"),
                 qPrintable(playClock->text()));
        QVERIFY(recordClock->toolTip().contains(QDir::toNativeSeparators(s.recording.path)));
        QVERIFY(playClock->toolTip().contains(QDir::toNativeSeparators(s.playback.path)));
        QVERIFY(!playClock->toolTip().contains(QStringLiteral("c.wav")));
        shot(header, QStringLiteral("header-recording-playing"));

        QCOMPARE(m_window->size(), QSize(722, 746));
        QCOMPARE(m_window->minimumSizeHint().width(), minimumWidth);
        auto* stack = m_window->findChild<QStackedWidget*>();
        QCOMPARE(stack->mapTo(m_window.get(), QPoint()), QPoint(203, 53));
        const auto at = [header](QWidget* w) { return w->mapTo(header, QPoint()); };
        QWidget* exact = header->exactValuesButton();
        for (QWidget* w : {static_cast<QWidget*>(recordClock), static_cast<QWidget*>(playClock),
                           static_cast<QWidget*>(chip), exact}) {
            QCOMPARE(w->height(), title->height());
            QCOMPARE(at(w).y(), at(title).y());
        }
        QVERIFY(at(state).x() + state->width() <= at(recordClock).x());
        QVERIFY(at(recordClock).x() + recordClock->width() <= at(playClock).x());
        QVERIFY(at(playClock).x() + playClock->width() <= at(chip).x());
        QVERIFY(at(chip).x() + chip->width() <= at(exact).x());

        // At the window's narrowest, the clocks are shortened rather than pushing it wider.
        m_window->resize(m_window->minimumWidth(), 746);
        QTRY_COMPARE(m_window->width(), minimumWidth);
        QCoreApplication::processEvents();
        QCOMPARE(m_window->minimumSizeHint().width(), minimumWidth);
        QCOMPARE(m_window->width(), minimumWidth);
        QVERIFY(at(exact).x() + exact->width() <= header->width());
        QCOMPARE(chip->width(), chip->minimumSizeHint().width());  // the chip gave all it could first
        shot(header, QStringLiteral("header-recording-playing-narrow"));
        // Back to the references' size: the chip takes back what the clocks do not need.
        m_window->resize(722, 746);
        QTRY_COMPARE(m_window->width(), 722);
        feed(recording);
        QTRY_COMPARE(recordClock->text(), QStringLiteral("● REC 01:23"));

        // A recording losing audio to a slow disk: amber, as on the chip.
        QCOMPARE(recordClock->property("tone").toString(), QString());
        s.recording.droppedFrames = 480;
        feed(s);
        QCOMPARE(recordClock->property("tone").toString(), QStringLiteral("warning"));
        QVERIFY(recordClock->toolTip().contains(QStringLiteral("could not keep up")));

        s.recording.active = false;
        s.playback.active = false;
        feed(s);
        QVERIFY(recordClock->isHidden() && playClock->isHidden());
        QVERIFY(header->recordingClock().isEmpty() && header->playbackClock().isEmpty());
        // Nothing to make way for: the chip has its whole width again.
        QTRY_COMPARE(chip->text(), chip->summary().text);
    }

    // Dragged narrower a step at a time with both clocks up, as a drag does it, the window
    // goes exactly as far as it does without them, in either frame. On the way the row gives
    // way in its order: PLAY's clock to its time alone before REC's, and both of them before
    // the combo goes below half.
    void aDragNarrowsTheWindowAsFarAsWithoutTheClocks_data()
    {
        QTest::addColumn<bool>("frameless");
        QTest::newRow("system frame") << false;
        QTest::newRow("frameless custom frame") << true;
    }

    void aDragNarrowsTheWindowAsFarAsWithoutTheClocks()
    {
        QFETCH(bool, frameless);
        const FramelessCustom asOnWindows(frameless);
        Settings::WindowState window;
        window.frame = frameless ? QStringLiteral("Custom") : QStringLiteral("System");
        make(window);
        atReferenceSize();
        QCOMPARE(bool(m_window->windowFlags() & Qt::FramelessWindowHint), frameless);
        control(QStringLiteral("tube.drive"))->setValue(3);  // "Modified" shows too
        quieten();
        EngineStatus s;
        s.connection = TciClient::State::Ready;
        s.audioFlowing = true;
        s.frequencyHz = 14200000;
        s.modulation = QStringLiteral("USB");
        feed(s);
        const int least = m_window->minimumWidth();

        s.recording.active = true;
        s.recording.seconds = 83.4;
        s.recording.path = m_dir.filePath(QStringLiteral("a.wav"));
        s.playback.active = true;
        s.playback.seconds = 12.7;
        s.playback.totalSeconds = 83.9;
        s.lastRecordingAvailable = true;
        s.lastRecordingPath = s.recording.path;
        feed(s);
        auto* header = m_window->findChild<HeaderBar*>();
        QVERIFY(header);
        auto* recordClock = header->findChild<QLabel*>(QStringLiteral("recordClock"));
        auto* playClock = header->findChild<QLabel*>(QStringLiteral("playClock"));
        QComboBox* profiles = header->profileList();
        QVERIFY(recordClock && playClock && profiles);
        QCOMPARE(m_window->minimumWidth(), least);  // showing the clocks widened nothing
        const QString recordWhole = QStringLiteral("● REC 01:23");
        const QString recordShort = QStringLiteral("● 01:23");
        const QString playWhole = QStringLiteral("▶ 00:12 / 01:23");
        const QString playShort = QStringLiteral("▶ 00:12");
        const int comboHalf = profiles->sizeHint().width() - profiles->sizeHint().width() / 2;

        bool recordGaveWay = false;
        for (int width = m_window->width(); width >= least; width -= 2) {
            m_window->resize(width, 746);
            QCoreApplication::processEvents();
            const QString at = QStringLiteral("at %1: %2 | %3 | combo %4 | minimum %5")
                                   .arg(width)
                                   .arg(recordClock->text(), playClock->text())
                                   .arg(profiles->width())
                                   .arg(m_window->minimumWidth());
            QVERIFY2(m_window->width() == width, qPrintable(at));  // the drag never stops short
            QVERIFY2(m_window->minimumWidth() == least, qPrintable(at));
            QVERIFY2(recordClock->text() == recordWhole || recordClock->text() == recordShort, qPrintable(at));
            QVERIFY2(playClock->text() == playWhole || playClock->text() == playShort, qPrintable(at));
            if (recordClock->text() == recordShort) {
                recordGaveWay = true;
                QVERIFY2(playClock->text() == playShort, qPrintable(at));
            }
            if (profiles->width() < comboHalf) QVERIFY2(recordClock->text() == recordShort, qPrintable(at));
        }
        m_window->resize(least, 746);
        QCoreApplication::processEvents();
        QCOMPARE(m_window->width(), least);
        QVERIFY(recordGaveWay);
        QCOMPARE(recordClock->text(), recordShort);
        QCOMPARE(playClock->text(), playShort);
        shot(header, frameless ? QStringLiteral("header-narrowest-frameless") : QStringLiteral("header-narrowest"));

        // Recording and playing over: the window is no narrower than it may be without them.
        s.recording.active = false;
        s.playback.active = false;
        feed(s);
        QCOMPARE(m_window->minimumWidth(), least);
        QCOMPARE(m_window->width(), least);
    }

    // The buttons show what the engine reports, and showing it asks the engine for nothing.
    void theButtonsFollowTheEngineNotTheClick()
    {
        make();
        QPushButton* record = recordButton();
        QPushButton* play = playButton();
        QVERIFY(record && play);
        QVERIFY(!record->isChecked() && record->isEnabled());
        QVERIFY(!play->isChecked() && !play->isEnabled());
        QCOMPARE(play->accessibleDescription(),
                 QStringLiteral("Unavailable until something has been recorded. Use REC first."));
        quieten();
        QSignalSpy recordToggles(record, &QPushButton::toggled);
        QSignalSpy playToggles(play, &QPushButton::toggled);
        // With no radio, any REC asked of the engine now would be turned down, and say so.
        QSignalSpy asked(m_engine.get(), &Engine::recordingFailed);

        EngineStatus s;
        s.recording.active = true;
        feed(s);
        QVERIFY(record->isChecked());
        QVERIFY(!play->isEnabled());  // nothing finished yet to play
        s.recording.active = false;
        s.lastRecordingAvailable = true;
        s.lastRecordingPath = m_dir.filePath(QStringLiteral("a.wav"));
        feed(s);
        QVERIFY(!record->isChecked());
        QVERIFY(play->isEnabled() && !play->isChecked());
        QCOMPARE(play->accessibleDescription(), QStringLiteral("Play back the last recording. Click again to stop."));
        s.playback.active = true;
        feed(s);
        QVERIFY(play->isChecked());
        s.playback.active = false;
        feed(s);
        QVERIFY(!play->isChecked());

        QCOMPARE(recordToggles.count(), 0);
        QCOMPARE(playToggles.count(), 0);
        QCoreApplication::processEvents();
        QCOMPARE(asked.count(), 0);
    }

    // With no radio, REC is turned down: the button flips straight back, and nothing failed,
    // so no warning and no red chip. The chip's tooltip and the button's say why.
    void aRefusedRecFlipsBackAndLeavesTheChipAlone()
    {
        make();
        QPushButton* record = recordButton();
        auto* chip = m_window->findChild<StatusChip*>();
        QVERIFY(record && chip);
        QTRY_COMPARE(chip->summary().text, QStringLiteral("Not connected"));
        const StatusChip::Summary before = chip->summary();
        QSignalSpy failed(m_engine.get(), &Engine::recordingFailed);

        QElapsedTimer timer;
        timer.start();
        record->click();
        QVERIFY(record->isChecked());  // the click's own, until the engine answers
        while (record->isChecked() && timer.elapsed() < 2000) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        qInfo() << "REC flipped back after" << timer.elapsed() << "ms";
        QVERIFY(!record->isChecked());
        QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("Connect to a radio first"));
        QVERIFY(m_status.recording.refused);

        QTest::qWait(300);  // several statuses: a warning would be up by now
        QVERIFY(notices().isEmpty());
        QVERIFY(!record->isChecked());
        QCOMPARE(chip->summary().text, before.text);
        QCOMPARE(chip->summary().tone, before.tone);
        QVERIFY2(chip->toolTip().contains(QStringLiteral("Not recording: Connect to a radio first")),
                 qPrintable(chip->toolTip()));
        QVERIFY2(record->toolTip().contains(QStringLiteral("Not recording: Connect to a radio first")),
                 qPrintable(record->toolTip()));
        QVERIFY(!QFileInfo::exists(recordingsFolder()));  // nothing was made for it
    }

    // A refusal is told from a failure by the status sent just before its signal. Connecting
    // straight after the press answers the refusal, so the status after the signal no longer
    // says there was one; read from that, the refusal would warn as a recording that stopped.
    void aRefusalAnsweredAtOnceWarnsNothing()
    {
        MockExpert radio;
        QVERIFY(radio.listen());
        AppConfig config;
        config.host = QStringLiteral("127.0.0.1");
        config.port = radio.port();
        // Never the defaults: one of them is where a real ExpertSDR3 may be listening.
        config.fallbackPorts.clear();
        m_withSink = true;  // and never this computer's speakers
        make({}, config);
        QPushButton* record = recordButton();
        QVERIFY(record);
        QSignalSpy failed(m_engine.get(), &Engine::recordingFailed);

        // Both reach the engine before the window hears back from either.
        record->click();
        DiscoveredRadio picked;
        picked.host = QStringLiteral("127.0.0.1");
        picked.port = radio.port();
        m_window->connectToRadio(picked);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("Connect to a radio first"));
        QVERIFY(!m_status.recording.refused);  // answered by the connect
        QTRY_VERIFY_WITH_TIMEOUT(m_status.audioFlowing, 6000);

        QTest::qWait(300);  // several statuses: a warning would be up by now
        QVERIFY(notices().isEmpty());
        QVERIFY(!record->isChecked());
        QVERIFY(!m_status.recording.active);
    }

    // A refusal is told from a failure by its status, which comes first, and by its reason. A
    // failure of the recording before it (its last write lost to a full disk, say) can arrive
    // while that refusal is still the last status seen, after its own signal and before its
    // own status: it still warns.
    void aFailureAfterARefusalStillWarns()
    {
        make();
        quieten();
        EngineStatus refused;
        refused.recording.error = QStringLiteral("Connect to a radio first");
        refused.recording.refused = true;
        feed(refused);
        emit m_engine->recordingFailed(refused.recording.error);  // the refusal's own signal
        QCoreApplication::processEvents();
        QVERIFY(notices().isEmpty());

        const QString full = QStringLiteral("The disk is full");
        emit m_engine->recordingFailed(full);
        EngineStatus failed;
        failed.recording.error = full;
        failed.recording.path = m_dir.filePath(QStringLiteral("kept.wav"));
        feed(failed);
        QTRY_COMPARE(notices().size(), 1);
        QCOMPARE(notices().constFirst()->text(), QStringLiteral("Recording stopped"));
        QVERIFY(notices().constFirst()->informativeText().startsWith(full));
    }

    // A recording that stops early warns once and turns the chip red. The same failure again
    // brings that warning forward rather than adding a second; once it has been answered, the
    // next one is news again.
    void aRecordingFailureWarnsOnceAndTurnsTheChipRed()
    {
        // A folder that cannot be made, because a file has its parent's name: the same failure
        // at every REC, on any system.
        recordingsFolder();
        const QString blocker = m_recordings->filePath(QStringLiteral("not-a-folder"));
        {
            QFile file(blocker);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        m_recordingFolder = blocker + QStringLiteral("/Recordings");
        MockExpert radio;
        QVERIFY(radio.listen());
        makeOnRadio(radio);
        if (QTest::currentTestFailed()) return;
        QPushButton* record = recordButton();
        auto* chip = m_window->findChild<StatusChip*>();
        QTRY_COMPARE(chip->summary().tone, QStringLiteral("success"));
        QSignalSpy failed(m_engine.get(), &Engine::recordingFailed);

        record->click();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
        const QString reason = failed.at(0).at(0).toString();
        QVERIFY2(reason.contains(QStringLiteral("Could not create the folder")), qPrintable(reason));
        QTRY_COMPARE(notices().size(), 1);
        QPointer<QMessageBox> notice = notices().constFirst();
        QCOMPARE(notice->text(), QStringLiteral("Recording stopped"));
        QVERIFY2(notice->informativeText().contains(reason), qPrintable(notice->informativeText()));
        QCOMPARE(notice->windowModality(), Qt::NonModal);  // the window goes on working beneath it
        // It says how to record elsewhere, and offers to choose the folder at once.
        QVERIFY2(notice->informativeText().contains(QStringLiteral("choose another folder")),
                 qPrintable(notice->informativeText()));
        QVERIFY(notice->findChild<QPushButton*>(QStringLiteral("noticeChooseFolder")));
        QVERIFY(!m_status.recording.refused);
        QVERIFY(!record->isChecked());
        QTRY_COMPARE(chip->summary().tone, QStringLiteral("danger"));
        QCOMPARE(chip->summary().text, QStringLiteral("Recording stopped: ") + reason);
        shot(m_window.get(), QStringLiteral("window-recording-failed"));

        record->click();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 2, 5000);
        QCOMPARE(failed.at(1).at(0).toString(), reason);
        QTest::qWait(300);  // the status after it, and any warning it brings
        QCOMPARE(notices().size(), 1);
        QCOMPARE(notices().constFirst(), notice.data());
        QCOMPARE(chip->summary().tone, QStringLiteral("danger"));

        notice->button(QMessageBox::Ok)->click();
        QTRY_VERIFY(notices().isEmpty());
        QTRY_VERIFY(notice.isNull());  // gone, not just hidden
        record->click();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 3, 5000);
        QTRY_COMPARE(notices().size(), 1);
        QVERIFY(!QFileInfo::exists(m_recordingFolder));

        // Choose Folder... answers the warning and asks for a folder, through the window's
        // replaceable chooser; the next REC records there.
        notice = notices().constFirst();
        auto* choose = notice->findChild<QPushButton*>(QStringLiteral("noticeChooseFolder"));
        QVERIFY(choose);
        const QString chosen = QDir::cleanPath(m_recordings->filePath(QStringLiteral("Chosen")));
        m_folderChoice = chosen;
        const int asksBefore = m_folderAsks;
        choose->click();
        QCOMPARE(m_folderAsks, asksBefore + 1);
        QCOMPARE(Settings(iniPath()).recordingFolder(), chosen);
        QTRY_VERIFY(notices().isEmpty());
        record->click();
        QTRY_VERIFY_WITH_TIMEOUT(m_status.recording.active && m_status.recording.seconds >= 0.3, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(m_status.recording.path.startsWith(chosen), 5000);
        record->click();
        QTRY_VERIFY_WITH_TIMEOUT(!m_status.recording.active, 5000);
        QCOMPARE(failed.count(), 3);
        QVERIFY(notices().isEmpty());
    }

    // A recording that stopped early is news, not a state: red on the chip until its warning
    // is answered, and red again only for the next failure. After that the chip shows the
    // connection again and opens Radio, and a new connection starts with no failure at all.
    void aRecordingFailureIsNewsNotAState()
    {
        recordingsFolder();
        const QString blocker = m_recordings->filePath(QStringLiteral("not-a-folder"));
        {
            QFile file(blocker);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        m_recordingFolder = blocker + QStringLiteral("/Recordings");
        MockExpert radio;
        QVERIFY(radio.listen());
        makeOnRadio(radio);
        if (QTest::currentTestFailed()) return;
        QPushButton* record = recordButton();
        auto* chip = m_window->findChild<StatusChip*>();
        auto* connectButton = m_window->findChild<QPushButton*>(QStringLiteral("connect"));
        QVERIFY(chip && connectButton && connectButton->isChecked());
        QTRY_COMPARE(chip->summary().tone, QStringLiteral("success"));
        QSignalSpy failed(m_engine.get(), &Engine::recordingFailed);

        record->click();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
        QTRY_COMPARE(notices().size(), 1);
        QTRY_COMPARE(chip->summary().tone, QStringLiteral("danger"));
        notices().constFirst()->button(QMessageBox::Ok)->click();
        QTRY_VERIFY(notices().isEmpty());
        // Answered: the chip is the radio's again, and its tooltip still says what happened.
        QTRY_COMPARE(chip->summary().tone, QStringLiteral("success"));
        QCOMPARE(chip->summary().page, QStringLiteral("radio"));
        QVERIFY2(chip->toolTip().contains(QStringLiteral("Recording stopped")), qPrintable(chip->toolTip()));
        QTest::qWait(300);  // and stays so, status after status
        QCOMPARE(chip->summary().tone, QStringLiteral("success"));

        // The next failure is news again, until its own warning is answered.
        record->click();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 2, 5000);
        QTRY_COMPARE(notices().size(), 1);
        QTRY_COMPARE(chip->summary().tone, QStringLiteral("danger"));
        notices().constFirst()->button(QMessageBox::Ok)->click();
        QTRY_COMPARE(chip->summary().tone, QStringLiteral("success"));

        // Disconnected: the chip says so, and a click on it opens Radio.
        connectButton->click();
        QTRY_COMPARE_WITH_TIMEOUT(m_status.connection, TciClient::State::Disconnected, 5000);
        QTRY_COMPARE(chip->summary().text, QStringLiteral("Not connected"));
        m_window->showPage(QStringLiteral("nr"));
        chip->click();
        QCOMPARE(m_window->currentPage(), QStringLiteral("radio"));

        // Connected again: a new session, with no recording failure in it.
        connectButton->click();
        QTRY_VERIFY_WITH_TIMEOUT(m_status.connection == TciClient::State::Ready && m_status.audioFlowing, 6000);
        QVERIFY2(m_status.recording.error.isEmpty(), qPrintable(m_status.recording.error));
        QTRY_COMPARE(chip->summary().tone, QStringLiteral("success"));
        QVERIFY2(!chip->toolTip().contains(QStringLiteral("Recording stopped")), qPrintable(chip->toolTip()));
        QCOMPARE(failed.count(), 2);
    }

    // A failure already on its way as the warning for the same one is answered: the box is
    // hidden at once but deleted only later, and the failure must still be said rather than
    // handed to the box on its way out.
    void aFailureArrivingAsItsWarningIsAnsweredIsStillShown()
    {
        make();
        quieten();
        EngineStatus playing;
        playing.lastRecordingAvailable = true;
        playing.playback.active = true;
        feed(playing);
        const QString reason = QStringLiteral("Output device stopped");
        emit m_engine->playbackFailed(reason);
        QCoreApplication::processEvents();
        QTRY_COMPARE(notices().size(), 1);
        QPointer<QMessageBox> first = notices().constFirst();

        // The window takes the engine's reports queued, so this one waits until after the OK.
        emit m_engine->playbackFailed(reason);
        first->button(QMessageBox::Ok)->click();
        QCoreApplication::processEvents();
        QTRY_VERIFY(first.isNull());
        QCOMPARE(notices().size(), 1);
        QCOMPARE(notices().constFirst()->informativeText(), reason);
    }

    // PLAY pressed as a recording stops waits for its file: it is on, but nothing plays yet.
    // The clock says it is waiting, not that it plays the recording before, and a refusal
    // then says it could not play rather than that playback stopped.
    void aPlayWaitingForItsRecordingIsNotPlaying()
    {
        make();
        quieten();
        auto* playClock = m_window->findChild<QLabel*>(QStringLiteral("playClock"));
        QVERIFY(playClock);
        EngineStatus s;
        s.lastRecordingAvailable = true;
        s.lastRecordingPath = m_dir.filePath(QStringLiteral("older.wav"));
        s.playback.active = true;
        s.playback.waiting = true;
        feed(s);
        QVERIFY(playButton()->isChecked());
        QVERIFY(!playClock->isHidden());
        QCOMPARE(playClock->toolTip(), QStringLiteral("Waiting for the recording to finish, then playing it"));

        emit m_engine->playbackFailed(QStringLiteral("No output device"));
        QCoreApplication::processEvents();
        QTRY_COMPARE(notices().size(), 1);
        QCOMPARE(notices().constFirst()->text(), QStringLiteral("Could not play the recording"));
        notices().constFirst()->button(QMessageBox::Ok)->click();
        QTRY_VERIFY(notices().isEmpty());

        // Once it plays, the clock names what it plays, and a failure is one that cut it short.
        s.playback.waiting = false;
        s.lastRecordingPath = m_dir.filePath(QStringLiteral("newer.wav"));
        s.playback.path = s.lastRecordingPath;
        feed(s);
        QCOMPARE(playClock->toolTip(), QStringLiteral("Playing %1").arg(QDir::toNativeSeparators(s.playback.path)));
        emit m_engine->playbackFailed(QStringLiteral("Output device stopped"));
        QCoreApplication::processEvents();
        QTRY_COMPARE(notices().size(), 1);
        QCOMPARE(notices().constFirst()->text(), QStringLiteral("Playback stopped"));
    }

    // PLAY turned down says why, in a warning, and the button flips back. The same cause again
    // brings that warning forward; a different one takes its place.
    void playWarnsWhenItCannotPlay()
    {
        MockExpert radio;
        QVERIFY(radio.listen());
        makeOnRadio(radio);
        if (QTest::currentTestFailed()) return;
        QPushButton* record = recordButton();
        QPushButton* play = playButton();
        record->click();
        QTRY_VERIFY_WITH_TIMEOUT(m_status.recording.active && m_status.recording.seconds >= 0.3, 5000);
        record->click();
        QTRY_VERIFY_WITH_TIMEOUT(play->isEnabled(), 5000);

        // The radio changes rate, and the output with it: a 48 kHz recording cannot play now.
        radio.operatorSetStreamRate(24000);
        QTRY_COMPARE_WITH_TIMEOUT(m_status.chainRate, 24000, 5000);
        QSignalSpy failed(m_engine.get(), &Engine::playbackFailed);
        play->click();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
        QTRY_VERIFY(!play->isChecked());
        QTRY_COMPARE(notices().size(), 1);
        QPointer<QMessageBox> notice = notices().constFirst();
        QCOMPARE(notice->text(), QStringLiteral("Could not play the recording"));
        QVERIFY2(notice->informativeText().contains(QStringLiteral("48000"))
                     && notice->informativeText().contains(QStringLiteral("24000")),
                 qPrintable(notice->informativeText()));
        play->click();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 2, 5000);
        QTest::qWait(200);
        QCOMPARE(notices().size(), 1);
        QCOMPARE(notices().constFirst(), notice.data());
        QVERIFY(!play->isChecked());

        // Cut short while playing: said as such, in place of the warning before.
        quieten();
        EngineStatus playing;
        playing.lastRecordingAvailable = true;
        playing.playback.active = true;
        feed(playing);
        emit m_engine->playbackFailed(QStringLiteral("Output device stopped"));
        QCoreApplication::processEvents();
        QTRY_VERIFY(notice.isNull());
        QCOMPARE(notices().size(), 1);
        QCOMPARE(notices().constFirst()->text(), QStringLiteral("Playback stopped"));
        QCOMPARE(notices().constFirst()->informativeText(), QStringLiteral("Output device stopped"));
    }

    // gear › Recordings: where REC saves, the last recording, and a way there, which only
    // ever goes through the window's replaceable reveal. Looking makes no folder.
    void theRecordingsMenuShowsTheFolderAndRevealsIt()
    {
#if defined(Q_OS_MACOS)
        QCOMPARE(MainWindow::revealCommandText(), QStringLiteral("Show in Finder"));
#elif defined(Q_OS_WIN)
        QCOMPARE(MainWindow::revealCommandText(), QStringLiteral("Show in Explorer"));
#else
        QCOMPARE(MainWindow::revealCommandText(), QStringLiteral("Show in Files"));
#endif
        make();
        const QString folder = recordingsFolder();
        auto* gear = m_window->findChild<QMenu*>(QStringLiteral("gearMenu"));
        QVERIFY(actionNamed(gear, QStringLiteral("Folder...")));
        QVERIFY(actionNamed(gear, MainWindow::revealCommandText()));
        auto* reveal = m_window->findChild<QAction*>(QStringLiteral("recordingsReveal"));
        auto* folderShown = m_window->findChild<QAction*>(QStringLiteral("recordingsFolderShown"));
        auto* lastShown = m_window->findChild<QAction*>(QStringLiteral("recordingsLastShown"));
        QVERIFY(reveal && folderShown && lastShown);

        QMenu* menu = openRecordingsMenu();
        QVERIFY(menu);
        shot(menu, QStringLiteral("recordings-menu"));
        QCOMPARE(folderShown->toolTip(), QDir::toNativeSeparators(folder));
        QVERIFY(!folderShown->isEnabled() && !lastShown->isEnabled());  // they say, they do nothing
        QCOMPARE(lastShown->text(), QStringLiteral("No recording yet this session"));
        QVERIFY(!reveal->isEnabled());  // nothing there yet
        reveal->trigger();
        QVERIFY(m_revealed.isEmpty());
        menu->close();
        QVERIFY(!QFileInfo::exists(folder));

        QVERIFY(QDir().mkpath(folder));
        menu = openRecordingsMenu();
        QVERIFY(menu);
        QVERIFY(reveal->isEnabled());
        reveal->trigger();
        QCOMPARE(m_revealed, QStringList{folder});
        menu->close();

        // The last recording, once there is one: shown by name, revealed itself.
        quieten();
        const QString last = QDir(folder).filePath(QStringLiteral("2026-11-07_141503Z_14.200MHz_USB.wav"));
        {
            QFile file(last);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        EngineStatus s;
        s.recording.path = last;
        s.lastRecordingAvailable = true;
        s.lastRecordingPath = last;
        feed(s);
        menu = openRecordingsMenu();
        QVERIFY(menu);
        QCOMPARE(lastShown->text(), QStringLiteral("Last: 2026-11-07_141503Z_14.200MHz_USB.wav"));
        QCOMPARE(lastShown->toolTip(), QDir::toNativeSeparators(last));
        shot(menu, QStringLiteral("recordings-menu-last"));
        reveal->trigger();
        QCOMPARE(m_revealed.last(), last);
        menu->close();

        // Moved away since: said so, and Show in… shows the folder instead.
        QVERIFY(QFile::rename(last, QDir(folder).filePath(QStringLiteral("moved.wav"))));
        menu = openRecordingsMenu();
        QVERIFY(menu);
        QVERIFY2(lastShown->text().startsWith(QStringLiteral("Last: 2026-11-07"))
                     && lastShown->text().endsWith(QStringLiteral(" (not found)")),
                 qPrintable(lastShown->text()));
        QVERIFY2(lastShown->toolTip().contains(QStringLiteral("No longer there")), qPrintable(lastShown->toolTip()));
        reveal->trigger();
        QCOMPARE(m_revealed.last(), folder);
        menu->close();

        // Folder... asks, and keeps the answer as recording/folder, without making it.
        const QString elsewhere = QDir::cleanPath(m_recordings->filePath(QStringLiteral("Elsewhere")));
        m_folderChoice = elsewhere;
        m_window->findChild<QAction*>(QStringLiteral("recordingsFolder"))->trigger();
        QCOMPARE(m_folderAsks, 1);
        QCOMPARE(Settings(iniPath()).recordingFolder(), elsewhere);
        QVERIFY(!QFileInfo::exists(elsewhere));
        menu = openRecordingsMenu();
        QVERIFY(menu);
        QCOMPARE(folderShown->toolTip(), QDir::toNativeSeparators(elsewhere));
        menu->close();
        // Asked and cancelled: as it was.
        m_folderChoice.clear();
        m_window->findChild<QAction*>(QStringLiteral("recordingsFolder"))->trigger();
        QCOMPARE(m_folderAsks, 2);
        QCOMPARE(Settings(iniPath()).recordingFolder(), elsewhere);
    }

    // From the window to the disk and back, through MockExpert: REC makes a file in the folder
    // set for it, PLAY sends that file to the speakers, and neither says anything to the radio.
    void recAndPlayFromTheWindowEndToEnd()
    {
        MockExpert radio;
        QVERIFY(radio.listen());
        makeOnRadio(radio);
        if (QTest::currentTestFailed()) return;
        const QString folder = recordingsFolder();
        // Starting, connecting and listening make no folder: only a recording does.
        QVERIFY(!QFileInfo::exists(folder));
        QTRY_COMPARE_WITH_TIMEOUT(m_status.mutePhase, MuteController::Phase::Holding, 5000);
        QTest::qWait(300);  // nothing more is due from the mute policy
        const qsizetype commandsBefore = radio.commands.size();

        QPushButton* record = recordButton();
        QPushButton* play = playButton();
        auto* header = m_window->findChild<HeaderBar*>();
        QVERIFY(!play->isEnabled());  // nothing recorded yet

        record->click();
        QTRY_VERIFY(m_status.recording.active);
        QTRY_VERIFY(record->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(m_status.recording.active && m_status.recording.seconds >= 1.0, 5000);
        QTRY_VERIFY(header->recordingClock() == QStringLiteral("● REC 00:01"));
        QVERIFY(!play->isEnabled());  // what is still being written cannot be played
        record->click();
        QTRY_VERIFY(!record->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(play->isEnabled(), 5000);
        QVERIFY(header->recordingClock().isEmpty());

        // One file, named from the time and the radio, in the folder set for it.
        const QStringList files = QDir(folder).entryList({QStringLiteral("*.wav")}, QDir::Files);
        QCOMPARE(files.size(), 1);
        static const QRegularExpression named(QStringLiteral(R"(^\d{4}-\d\d-\d\d_\d{6}Z_\d+\.\d{3}MHz_[A-Z0-9]+\.wav$)"));
        QVERIFY2(named.match(files.first()).hasMatch(), qPrintable(files.first()));
        const QString path = QDir(folder).filePath(files.first());
        QCOMPARE(QFileInfo(m_status.lastRecordingPath).canonicalFilePath(), QFileInfo(path).canonicalFilePath());
        const std::optional<std::vector<float>> recorded = framesOf(path);
        QVERIFY(recorded && recorded->size() >= size_t(48000) * 2);

        // PLAY: exactly the file's frames reach the speakers, in place of the radio's.
        std::optional<size_t> from;
        std::optional<size_t> to;
        bool wasPlaying = false;
        const auto watch = connect(m_engine.get(), &Engine::statusChanged, this, [&](const EngineStatus& s) {
            if (s.playback.active && !wasPlaying) from = m_sink->samples.size();
            if (!s.playback.active && wasPlaying) to = m_sink->samples.size();
            wasPlaying = s.playback.active;
        });
        play->click();
        QTRY_VERIFY(play->isChecked());
        const QString length = HeaderBar::clockText(double(recorded->size() / 2) / 48000.0);
        QTRY_VERIFY2(header->playbackClock().startsWith(QStringLiteral("▶ 00:0"))
                         && header->playbackClock().endsWith(QStringLiteral(" / ") + length),
                     qPrintable(header->playbackClock()));
        QTRY_VERIFY_WITH_TIMEOUT(to.has_value(), 10000);
        disconnect(watch);
        QTRY_VERIFY(!play->isChecked());
        QVERIFY(header->playbackClock().isEmpty());
        QVERIFY(from.has_value());
        QCOMPARE(*to - *from, recorded->size());
        QVERIFY2(std::memcmp(m_sink->samples.data() + *from, recorded->data(), recorded->size() * sizeof(float)) == 0,
                 "the speakers did not get the file's frames");
        QVERIFY(play->isEnabled());  // to play again

        // gear › Recordings names it and reveals it.
        QMenu* menu = openRecordingsMenu();
        QVERIFY(menu);
        QCOMPARE(m_window->findChild<QAction*>(QStringLiteral("recordingsLastShown"))->text(),
                 QStringLiteral("Last: ") + files.first());
        m_window->findChild<QAction*>(QStringLiteral("recordingsReveal"))->trigger();
        QCOMPARE(m_revealed.size(), 1);
        QCOMPARE(QFileInfo(m_revealed.first()).canonicalFilePath(), QFileInfo(path).canonicalFilePath());
        menu->close();

        // Each REC goes where the folder setting says at the press.
        const QString elsewhere = m_recordings->filePath(QStringLiteral("Elsewhere"));
        m_folderChoice = elsewhere;
        m_window->findChild<QAction*>(QStringLiteral("recordingsFolder"))->trigger();
        record->click();
        // The status keeps the last recording's length until the next one starts, so wait for
        // this one's own file.
        QTRY_VERIFY_WITH_TIMEOUT(m_status.recording.active && m_status.recording.path.startsWith(elsewhere)
                                     && m_status.recording.seconds >= 0.3,
                                 5000);
        record->click();
        QTRY_VERIFY2_WITH_TIMEOUT(!m_status.recording.active && m_status.lastRecordingPath.startsWith(elsewhere),
                                  qPrintable(QStringLiteral("active %1, last %2, path %3, error %4, wanted %5")
                                                 .arg(m_status.recording.active)
                                                 .arg(m_status.lastRecordingPath, m_status.recording.path,
                                                      m_status.recording.error, elsewhere)),
                                  5000);
        QCOMPARE(QDir(elsewhere).entryList({QStringLiteral("*.wav")}, QDir::Files).size(), 1);
        QCOMPARE(QDir(folder).entryList({QStringLiteral("*.wav")}, QDir::Files).size(), 1);

        QVERIFY(notices().isEmpty());
        // Neither REC nor PLAY said anything to the radio.
        QTest::qWait(300);
        const QStringList sent = radio.commands.mid(commandsBefore);
        QVERIFY2(radioChangingCommands(sent).isEmpty(), qPrintable(sent.join(u' ')));
        QVERIFY2(sent.isEmpty(), qPrintable(sent.join(u' ')));
        QVERIFY(radio.muted);  // still this app's audio
    }

    // The PLAY clock's tooltip names the file that is playing, also once a newer recording
    // has finished while it plays: that one is for the next PLAY.
    void thePlayClockNamesTheFileItPlays()
    {
        MockExpert radio;
        QVERIFY(radio.listen());
        makeOnRadio(radio);
        if (QTest::currentTestFailed()) return;
        QPushButton* record = recordButton();
        QPushButton* play = playButton();
        auto* playClock = m_window->findChild<QLabel*>(QStringLiteral("playClock"));
        QVERIFY(playClock);

        record->click();
        QTRY_VERIFY_WITH_TIMEOUT(m_status.recording.active && m_status.recording.seconds >= 4.0, 10000);
        record->click();
        QTRY_VERIFY_WITH_TIMEOUT(!m_status.recording.active && play->isEnabled(), 5000);
        const QString first = m_status.lastRecordingPath;
        QVERIFY(!first.isEmpty());
        play->click();
        QTRY_VERIFY(m_status.playback.active && !m_status.playback.waiting);
        QTRY_VERIFY2(playClock->toolTip().contains(QDir::toNativeSeparators(first)), qPrintable(playClock->toolTip()));

        // Another recording, made and finished while the first plays.
        record->click();
        QTRY_VERIFY_WITH_TIMEOUT(m_status.recording.active && !m_status.recording.path.isEmpty()
                                     && m_status.recording.path != first && m_status.recording.seconds >= 0.3,
                                 5000);
        const QString second = m_status.recording.path;
        record->click();
        QTRY_COMPARE_WITH_TIMEOUT(m_status.lastRecordingPath, second, 5000);
        QVERIFY2(m_status.playback.active, "the first recording should still be playing");
        QCoreApplication::processEvents();
        QVERIFY2(playClock->toolTip().contains(QDir::toNativeSeparators(first)), qPrintable(playClock->toolTip()));
        QVERIFY2(!playClock->toolTip().contains(QFileInfo(second).fileName()), qPrintable(playClock->toolTip()));
        play->click();
        QTRY_VERIFY(!m_status.playback.active);
    }

    // ── Pieces ────────────────────────────────────────────────────────────────

    // The window is drawn in the built-in Inter, whose licence asks for credit wherever it
    // goes, with AetherSDR's widgets.
    void theAboutBoxCreditsTheTypefaceAndTheWidgets()
    {
        AboutDialog about;
        QStringList text;
        for (const QLabel* label : about.findChildren<QLabel*>()) text << label->text();
        const QString all = text.join(u' ');
        QVERIFY2(all.contains(QStringLiteral("Inter typeface (SIL Open Font License 1.1)")), qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("AetherRX chain and GUI widgets (GPL-3.0)")), qPrintable(all));
    }

    // Rounded to the spec's decimals before clamping, so float noise from a drag never
    // reaches the settings, and a value just past an edge lands on it.
    void aControlRoundsToItsDecimals()
    {
        QWidget host;
        auto* grid = new QGridLayout(&host);
        const ParamSpec spec{QStringLiteral("test.x"), QStringLiteral("X"), 0.1, 10.0, 1.0, 1};
        ParamControl control(spec, 1.0, grid, 0, &host);
        QSignalSpy changed(&control, &ParamControl::valueChanged);
        control.setValue(3.14159);
        QCOMPARE(control.value(), 3.1);
        QCOMPARE(changed.last().at(1).toDouble(), 3.1);
        control.setValue(0.04);  // rounds to 0.0, then clamps
        QCOMPARE(control.value(), 0.1);
        control.setValue(9.96);  // rounds to 10.0, inside the range
        QCOMPARE(control.value(), 10.0);
        control.setValueSilently(2.25001);
        QCOMPARE(control.value(), 2.3);
    }

    // So rounding before clamping can always reach both ends of a range.
    void everyRangeEndsOnItsOwnDecimals()
    {
        for (const ParamSpec& spec : allParamSpecs()) {
            const double scale = std::pow(10.0, spec.decimals);
            for (double v : {spec.min, spec.max}) {
                QVERIFY2(std::abs(std::round(v * scale) / scale - v) < 1e-9,
                         qPrintable(QStringLiteral("%1: %2").arg(spec.key).arg(v)));
            }
        }
    }

    // A stage page's exact values are one click from the header, the gear or the stage's
    // tab. Pages with none (Radio, AetherNR and Final Output) leave the button and the
    // entries off.
    void theExactValuesOpenFromTheHeaderTheGearAndTheTab()
    {
        make();
        activate();
        auto* button = m_window->findChild<QPushButton*>(QStringLiteral("exactValues"));
        QVERIFY(button);
        QCOMPARE(button->focusPolicy(), Qt::NoFocus);
        auto* gear = m_window->findChild<QMenu*>(QStringLiteral("gearMenu"));
        QAction* gearEntry = actionNamed(gear, QStringLiteral("Exact values..."));
        QVERIFY(gearEntry);
        for (const char* id : {"radio", "nr", "output"}) {
            m_window->showPage(QLatin1String(id));
            QVERIFY2(!button->isEnabled(), id);
            QVERIFY2(!gearEntry->isEnabled(), id);
            QCOMPARE(button->toolTip(), QStringLiteral("Exact values are for the stage pages."));
        }
        // The stages on AetherRX's pages have theirs; the EQ's hold its output gain.
        for (const char* id : {"eq", "comp", "tube", "exciter"}) {
            m_window->showPage(QLatin1String(id));
            QVERIFY2(button->isEnabled(), id);
            QVERIFY2(gearEntry->isEnabled(), id);
        }
        auto* details = m_window->findChild<DetailsWindow*>(QStringLiteral("details_gate"));
        QVERIFY(details && !details->isVisible());

        m_window->showPage(QStringLiteral("gate"));
        QVERIFY(button->isEnabled());
        shot(m_window->findChild<QWidget*>(QStringLiteral("headerBar")), QStringLiteral("header-gate"));
        button->click();
        QTRY_VERIFY(details->isVisible());
        shot(details, QStringLiteral("details-gate"));
        details->close();

        QVERIFY(gearEntry->isEnabled());
        gearEntry->trigger();
        QTRY_VERIFY(details->isVisible());
        details->close();

        // From the tab's menu, whichever page is showing.
        m_window->showPage(QStringLiteral("radio"));
        activate();
        emit railTab(QStringLiteral("Gate"))->customContextMenuRequested(QPoint(10, 10));
        QMenu* menu = openPopup();
        QVERIFY(menu);
        QAction* tabEntry = actionNamed(menu, QStringLiteral("Exact values..."));
        QVERIFY(tabEntry && tabEntry->isEnabled());
        tabEntry->trigger();
        menu->close();
        QTRY_VERIFY(details->isVisible());
        details->close();
        activate();
        emit railTab(QStringLiteral("Compressor"))->customContextMenuRequested(QPoint(10, 10));
        menu = openPopup();
        QVERIFY(menu);
        tabEntry = actionNamed(menu, QStringLiteral("Exact values..."));
        QVERIFY(tabEntry && tabEntry->isEnabled());
        tabEntry->trigger();
        menu->close();
        auto* compDetails = m_window->findChild<DetailsWindow*>(QStringLiteral("details_comp"));
        QTRY_VERIFY(compDetails && compDetails->isVisible());
        compDetails->close();
        // Every stage's tab now has exact values, the EQ's included.
        activate();
        emit railTab(QStringLiteral("EQ"))->customContextMenuRequested(QPoint(10, 10));
        menu = openPopup();
        QVERIFY(menu);
        tabEntry = actionNamed(menu, QStringLiteral("Exact values..."));
        QVERIFY(tabEntry && tabEntry->isEnabled());
        tabEntry->trigger();
        menu->close();
        auto* eqDetails = m_window->findChild<DetailsWindow*>(QStringLiteral("details_eq"));
        QTRY_VERIFY(eqDetails && eqDetails->isVisible());
        eqDetails->close();
    }

    // The header's button marks a page that leaves out a value moved from its default:
    // the gate's attack, or a look-ahead between the Peek slider's stops.
    void theExactValuesButtonMarksWhatThePageDoesNotShow()
    {
        make();
        auto* header = m_window->findChild<HeaderBar*>();
        QVERIFY(header);
        m_window->showPage(QStringLiteral("gate"));
        QVERIFY(!header->exactValuesFlagged());
        control(QStringLiteral("gate.attack"))->setValue(5.0);
        QVERIFY(header->exactValuesFlagged());
        control(QStringLiteral("gate.attack"))->setValue(control(QStringLiteral("gate.attack"))->spec().def);
        QVERIFY(!header->exactValuesFlagged());
        control(QStringLiteral("gate.lookahead"))->setValue(1.5);  // a stop: shown as it is
        QVERIFY(!header->exactValuesFlagged());
        control(QStringLiteral("gate.lookahead"))->setValue(2.3);  // shown as the nearest stop
        QVERIFY(header->exactValuesFlagged());
        // Only for the page that hides it.
        m_window->showPage(QStringLiteral("radio"));
        QVERIFY(!header->exactValuesFlagged());
        m_window->showPage(QStringLiteral("gate"));
        QVERIFY(header->exactValuesFlagged());
        // A profile load is a change too.
        m_window->loadProfile(QString());
        QVERIFY(!header->exactValuesFlagged());
    }

    void aDetailsWindowIsASecondaryWindow()
    {
        make();
        auto* details = new DetailsWindow(QStringLiteral("gate"), QStringLiteral("Gate: exact values"), m_window.get());
        QVERIFY(details->windowFlags() & Qt::Tool);
        QVERIFY(!details->isModal());
        QVERIFY(details->findChild<CanonBody*>());
        details->addHeading(QStringLiteral("Not on the page"));
        QCOMPARE(details->nextRow(), 1);
        details->present();
        QVERIFY(QTest::qWaitForWindowExposed(details));
        details->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(details));
        QTest::keyClick(details, Qt::Key_Escape);  // a secondary window: Escape closes it
        QTRY_VERIFY(!details->isVisible());
        QVERIFY(m_window->isVisible());
        QCOMPARE(m_quits, 0);
    }
};

QTEST_MAIN(TestShell)
#include "test_shell.moc"
