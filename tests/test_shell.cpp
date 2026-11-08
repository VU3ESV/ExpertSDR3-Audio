#include "app/Engine.h"
#include "app/Profiles.h"
#include "app/Settings.h"
#include "dsp/NrProcessor.h"
#include "dsp/RxChain.h"
#include "gui/AboutDialog.h"
#include "gui/DetailsWindow.h"
#include "gui/MainWindow.h"
#include "gui/ParamWidgets.h"
#include "gui/aether/Appearance.h"
#include "gui/shell/StatusChip.h"

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
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QWindow>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <memory>

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

} // namespace

// The window's shell: the rail, the header, the menus, where it opens and every way it
// closes. Built against a real, unconnected engine, so it never reaches a radio.
class TestShell : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    std::unique_ptr<Settings> m_settings;
    std::unique_ptr<ProfileLibrary> m_profiles;
    std::unique_ptr<Engine> m_engine;
    std::unique_ptr<MainWindow> m_window;
    int m_quits{0};

    QString iniPath() const { return m_dir.filePath(QStringLiteral("shell.ini")); }

    // `window` is what the last session left in ui/*.
    void make(const Settings::WindowState& window = {}, const AppConfig& config = {})
    {
        m_window.reset();
        m_engine = std::make_unique<Engine>(iniPath());
        m_settings = std::make_unique<Settings>(iniPath());
        m_settings->saveWindowState(window);
        m_profiles = std::make_unique<ProfileLibrary>(ProfileLibrary::defaultPath(iniPath()));
        m_window = std::make_unique<MainWindow>(m_engine.get(), m_engine->taps(), m_settings.get(), m_profiles.get(),
                                                config);
        m_window->setQuitHandler([this] { ++m_quits; });
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
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

        m_window->showPage(QStringLiteral("gate"));
        auto* page = m_window->findChild<QWidget*>(QStringLiteral("page_gate"));
        QVERIFY(page);
        auto* field = page->findChild<QDoubleSpinBox*>();  // a parameter's exact value
        QVERIFY(field);
        field->setFocus();
        QTest::keyClick(field, Qt::Key_Escape);

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
        window.geometry = QStringLiteral("120,90,700,650");
        make(window);
        // Maximised or not (a small screen opens it maximised), this is where it returns to.
        QCOMPARE(m_window->normalGeometry(), QRect(120, 90, 700, 650));
        m_window->close();
        QCOMPARE(savedWindow().geometry, QStringLiteral("120,90,700,650"));
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
    // its one screen is 800x600, small enough that the window always opens maximised there.
    void whereTheWindowOpens_data()
    {
        QTest::addColumn<QString>("saved");
        QTest::addColumn<QList<QRect>>("screens");  // the main screen first
        QTest::addColumn<QRect>("geometry");
        QTest::addColumn<bool>("positioned");
        QTest::addColumn<bool>("maximised");
        const QRect desk(0, 25, 1920, 1055);    // a desktop under a menu bar
        const QRect side(1920, 0, 1280, 720);   // a small second screen to its right
        const QRect pi(0, 0, 800, 444);         // a Pi touchscreen above its panel
        const QRect wide(800, 0, 1920, 1080);   // a big screen beside the Pi's
        QTest::newRow("first run, a big screen: the default size")
            << QString() << QList<QRect>{desk} << QRect(0, 0, 724, 752) << false << false;
        QTest::newRow("first run, a small screen: maximised")
            << QString() << QList<QRect>{pi} << QRect(0, 0, 724, 444) << false << true;
        QTest::newRow("first run, no screen at all")
            << QString() << QList<QRect>{} << QRect(0, 0, 724, 752) << false << false;
        QTest::newRow("a saved geometry that does not parse")
            << QStringLiteral("700x650") << QList<QRect>{desk} << QRect(0, 0, 724, 752) << false << false;
        QTest::newRow("saved on the main screen")
            << QStringLiteral("100,100,900,800") << QList<QRect>{desk, side} << QRect(100, 100, 900, 800) << true << false;
        QTest::newRow("saved on a small second screen: maximised there")
            << QStringLiteral("2000,50,700,600") << QList<QRect>{desk, side} << QRect(2000, 50, 700, 600) << true << true;
        QTest::newRow("saved on a big second screen, the main one small")
            << QStringLiteral("900,100,724,752") << QList<QRect>{pi, wide} << QRect(900, 100, 724, 752) << true << false;
        QTest::newRow("its screen has gone: the same size, centred on the main one")
            << QStringLiteral("2000,50,700,600") << QList<QRect>{desk} << QRect(610, 253, 700, 600) << true << false;
        QTest::newRow("its screen has gone, and it is bigger than the main one")
            << QStringLiteral("-5000,-5000,2500,1400") << QList<QRect>{desk} << desk << true << false;
    }
    void whereTheWindowOpens()
    {
        QFETCH(QString, saved);
        QFETCH(QList<QRect>, screens);
        QFETCH(QRect, geometry);
        QFETCH(bool, positioned);
        QFETCH(bool, maximised);
        const MainWindow::Placement place = MainWindow::placement(saved, screens);
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

    // Until the custom frame exists, ui/frame is kept as it was found and changes nothing.
    void theFrameSettingIsStoredAndTheFrameIsTheSystems_data()
    {
        QTest::addColumn<QString>("frame");
        QTest::newRow("never chosen") << QString();
        QTest::newRow("System") << QStringLiteral("System");
        QTest::newRow("Custom") << QStringLiteral("Custom");
    }
    void theFrameSettingIsStoredAndTheFrameIsTheSystems()
    {
        QFETCH(QString, frame);
        Settings::WindowState window;
        window.frame = frame;
        make(window);
        QVERIFY(!(m_window->windowFlags() & Qt::FramelessWindowHint));
        QVERIFY(!m_window->testAttribute(Qt::WA_TranslucentBackground));
        m_window->close();
        QCOMPARE(savedWindow().frame, frame);  // an unset frame stays unset, for its default
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

    void theRailHasItsRowsAndOnlyTheGearBelowThem()
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

        // The footer: only what works. REC, PLAY and BYPASS come with what they do.
        QVERIFY(rail()->findChild<QPushButton*>(QStringLiteral("aetherRxSettingsButton")));
        for (const char* absent : {"aetherRxRecord", "aetherRxPlay", "aetherRxBypass"}) {
            QVERIFY2(!m_window->findChild<QWidget*>(QString::fromLatin1(absent)), absent);
        }
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
        make();
        auto* box = rail()->findChild<QCheckBox*>(QStringLiteral("aetherRxEnableAetherNR"));
        auto* method = m_window->findChild<QComboBox*>(QStringLiteral("nrMethod"));
        QVERIFY(box && method);
        QVERIFY(!box->isChecked());

        box->click();  // never chosen before: NR2
        QCOMPARE(method->currentData().toString(), QStringLiteral("nr2"));
        method->setCurrentIndex(method->findData(QStringLiteral("rn2")));
        QVERIFY(box->isChecked());
        box->click();
        QCOMPARE(method->currentData().toString(), QString());
        QVERIFY(!box->isChecked());
        QCOMPARE(Settings(iniPath()).lastNrMethod(), QStringLiteral("rn2"));
        box->click();
        QCOMPARE(method->currentData().toString(), QStringLiteral("rn2"));

        // A later session remembers it too.
        method->setCurrentIndex(0);  // Off
        make();
        box = rail()->findChild<QCheckBox*>(QStringLiteral("aetherRxEnableAetherNR"));
        box->click();
        QCOMPARE(m_window->findChild<QComboBox*>(QStringLiteral("nrMethod"))->currentData().toString(), QStringLiteral("rn2"));
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

    // A recording that stopped early is red until the next REC, above everything from the
    // connection down; one losing audio to a slow disk is amber while it lasts. Neither has a
    // page to open. A REC that was turned down is neither.
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
