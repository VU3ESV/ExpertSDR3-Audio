#include "gui/MainWindow.h"

#include "app/AppInfo.h"
#include "dsp/NrProcessor.h"
#include "dsp/RxChain.h"
#include "gui/AboutDialog.h"
#include "gui/RadioDiscoveryDialog.h"
#include "gui/aether/Appearance.h"
#include "gui/pages/NrPage.h"
#include "gui/pages/OutputPage.h"
#include "gui/pages/RadioPage.h"
#include "gui/shell/HeaderBar.h"
#include "gui/shell/StatusChip.h"

#include "StageTabBar.h"
#include "core/AppSettings.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace esa {

namespace {

// What the window itself carries, over the application and canon body sheets that
// Appearance::apply() puts first: tooltips, its menus' unavailable commands, and today's
// look for the dialogs it opens. The pages bring their own sheets, so nothing here
// reaches the rail or a vendored page.
QString windowSheet()
{
    return QStringLiteral(R"(
/* A tooltip takes the sheet of the widget it explains; in the canon body that would
   leave it transparent. */
QToolTip, QWidget#canonBody QToolTip {
    color: {{color.canon.ink}};
    background: {{color.canon.raised}};
    border: 1px solid {{color.canon.lineHi}};
    padding: 4px 6px;
}
/* The application sheet's QWidget rule gives text one colour in every state, so without
   this a command that cannot run (Save with nothing changed, Move up at the top) would
   look like one that can. The gear and stage menus are the window's children. */
QMenu::item:disabled { color: {{color.button.foreground.disabled}}; }
)") + dialogSheet();
}

QString rectText(const QRect& r)
{
    return QStringLiteral("%1,%2,%3,%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

QRect parseRect(const QString& text)
{
    const QStringList parts = text.split(QLatin1Char(','));
    if (parts.size() != 4) return {};
    int v[4];
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        v[i] = parts.at(i).trimmed().toInt(&ok);
        if (!ok) return {};
    }
    return v[2] > 0 && v[3] > 0 ? QRect(v[0], v[1], v[2], v[3]) : QRect();
}

const PageEntry* pageOfRail(int railId)
{
    for (const PageEntry& page : pageTable()) {
        if (page.railId == railId) return &page;
    }
    return nullptr;
}

const PageEntry* pageOfStage(const QString& stage)
{
    for (const PageEntry& page : pageTable()) {
        if (!page.stage.isEmpty() && page.stage == stage) return &page;
    }
    return nullptr;
}

// The rail names its widgets after the row's label: aetherRxTabFinalOutput.
QString railSlug(const PageEntry& page)
{
    return QString(page.label).remove(QLatin1Char(' '));
}

} // namespace

MainWindow::MainWindow(Engine* engine, std::shared_ptr<LiveTaps> taps, Settings* settings, ProfileLibrary* profiles,
                       const AppConfig& config)
    : m_engine(engine), m_taps(std::move(taps)), m_settings(settings), m_profiles(profiles), m_config(config),
      m_window(settings->loadWindowState()), m_quit([] { QApplication::quit(); })
{
    setWindowTitle(QString::fromUtf8(kAppDisplayName));
    // AetherRX's minimum is 600x400. The height is kept. The width is left to the layout,
    // whose own (the rail beside the page stack's 400) is a little wider: a fixed 600 would
    // let the window shrink past it and push the page over the right-hand margin.
    setMinimumHeight(400);
    applyTheme();
    buildActions();
    buildPageContext();

    // Before any AetherSDR page exists: the few display choices they keep for good
    // (AppSettings::persists) go to this computer's settings. The rest stays in memory.
    AetherSDR::AppSettings::Store store;
    store.read = [settings](const QString& name) { return settings->aetherValue(name); };
    store.write = [settings](const QString& name, const QVariant& value) { settings->setAetherValue(name, value); };
    store.remove = [settings](const QString& name) { settings->removeAetherValue(name); };
    AetherSDR::AppSettings::instance().setStore(store);

    auto* central = new CanonBody;
    auto* outer = new QVBoxLayout(central);
    // AetherRX's 14, 12, 14, 14 sit inside its window's 1 px hairline; this window has none,
    // so 1 more each side puts the rail and the pages on the same pixels as upstream's.
    outer->setContentsMargins(15, 13, 15, 15);
    outer->setSpacing(10);
    m_header = new HeaderBar(QString::fromUtf8(kAppDisplayName));
    outer->addWidget(m_header);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(8);
    outer->addLayout(body, 1);
    body->addWidget(buildRail());
    m_stack = new QStackedWidget;
    // Set outright, so the window's minimum is this rather than the widest page's natural
    // size. A page that wants more scrolls or, like upstream's, shrinks its graphics.
    m_stack->setMinimumSize(400, 300);
    body->addWidget(m_stack, 1);
    setCentralWidget(central);

    buildPages();
    wireGatePreset();
    buildMenus();

    connect(m_header->profileList(), &QComboBox::activated, this, &MainWindow::onProfileSelected);
    connect(m_header->statusChip(), &StatusChip::pageRequested, this, &MainWindow::showPage);

    // The profile may have been deleted since these settings were saved.
    if (!m_config.profileName.isEmpty() && !m_profiles->find(m_config.profileName)) m_config.profileName.clear();
    m_profileNote = m_profiles->loadWarning();
    refreshProfileList();
    updateProfileState();

    // The first run starts at the first step, Radio; later runs where they left off.
    showPage(findPage(m_window.page) ? m_window.page : QStringLiteral("radio"));
    m_rail->refreshFromHost();
    restoreWindowGeometry();

    connect(m_engine, &Engine::statusChanged, this, &MainWindow::onStatus, Qt::QueuedConnection);
    connect(m_engine, &Engine::devicesChanged, m_output, &OutputPage::showDevices, Qt::QueuedConnection);
    connect(m_engine, &Engine::connectedPortChanged, m_radio, &RadioPage::showConnectedPort, Qt::QueuedConnection);

    onEngine([engine = m_engine, config = m_config] {
        engine->configure(config);
        engine->refreshDevices();
    });

    // Parameters are saved as they change, so a crash loses at most a few seconds.
    auto* autosave = new QTimer(this);
    autosave->setInterval(5000);
    connect(autosave, &QTimer::timeout, this, &MainWindow::saveSettings);
    autosave->start();
}

MainWindow::~MainWindow()
{
    // The pages first, while the context they were built with is still here.
    delete centralWidget();
    // The settings may go before the next window comes; until then the pages' choices stay
    // in memory.
    AetherSDR::AppSettings::instance().setStore({});
}

void MainWindow::applyTheme()
{
    // AetherRX's look on every platform, from AetherSDR's theme tokens. The style, the
    // font and the palette are the application's, set by Appearance::install().
    Appearance::apply(this, windowSheet());
}

void MainWindow::buildPageContext()
{
    m_context.config = &m_config;
    m_context.taps = m_taps;
    m_context.addParam = [this](const ParamSpec& spec, QGridLayout* grid, int row) {
        return registerControl(new ParamControl(spec, paramValue(m_config, spec), grid, row, this));
    };
    m_context.addParamCell = [this](const ParamSpec& spec, QGridLayout* grid, int row, int column) {
        return registerControl(new ParamControl(spec, paramValue(m_config, spec), grid, row, column, this));
    };
    m_context.post = [this](std::function<void(Engine*)> call) {
        onEngine([engine = m_engine, call = std::move(call)] { call(engine); });
    };
    m_context.chainChanged = [this] {
        updateProfileState();
        m_rail->refreshFromHost();
    };
    m_context.selectNrMethod = [this](const QString& id) { selectNrMethod(id); };
    m_context.findRadios = [this] { findRadios(); };
}

ParamControl* MainWindow::registerControl(ParamControl* control)
{
    // One control per key: whatever changes a parameter goes through it.
    Q_ASSERT(!m_controls.contains(control->spec().key));
    m_controls.insert(control->spec().key, control);
    connect(control, &ParamControl::valueChanged, this, &MainWindow::onParamChanged);
    return control;
}

QWidget* MainWindow::buildRail()
{
    using AetherSDR::StageTabBar;
    m_rail = new StageTabBar(QStringLiteral("aetherRx"), this);

    // Set before the rows: each row asks it whether it gets a grip and a box.
    StageTabBar::Host host;
    host.isChainStage = [](int id) { return id >= kRailGate && id <= kRailExciter; };
    host.stageEnabled = [this](int id) { return stageEnabled(id); };
    host.setStageEnabled = [this](int id, bool on) { setStageEnabled(id, on); };
    host.chainOrder = [this] {
        QVector<int> ids;
        for (const QString& stage : stageOrder()) {
            if (const PageEntry* page = pageOfStage(stage)) ids.append(page->railId);
        }
        return ids;
    };
    host.commitChainOrder = [this](const QVector<int>& ids) {
        QStringList order;
        for (int id : ids) {
            const PageEntry* page = pageOfRail(id);
            if (page && !page->stage.isEmpty()) order.append(page->stage);
        }
        setStageOrder(order);
    };
    m_rail->setHost(host);

    for (const PageEntry& page : pageTable()) m_rail->addStage(page.railId, page.label, page.checkbox);
    // REC, PLAY and BYPASS join the footer once they do something; until then the gear has
    // the row to itself.
    m_rail->addFooterGearButton(QStringLiteral("Settings"), QStringLiteral("aetherRxSettingsButton"),
                                QStringLiteral("Profiles, Find Radios, stage order and About."));

    connect(m_rail, &StageTabBar::stageSelected, this, [this](int railId) {
        if (const PageEntry* page = pageOfRail(railId)) showPage(page->id);
    });
    connect(m_rail, &StageTabBar::footerButtonClicked, this, &MainWindow::popUpGearMenu);

    // The grips need a mouse. A chain stage's tab also moves it from a menu, and with
    // Alt+Up and Alt+Down; the rows that are not in the chain never move at all.
    for (const PageEntry& page : pageTable()) {
        if (page.stage.isEmpty()) continue;
        auto* tab = m_rail->findChild<QPushButton*>(QStringLiteral("aetherRxTab") + railSlug(page));
        if (!tab) continue;
        m_chainTabs.insert(tab, page.stage);
        tab->installEventFilter(this);
        tab->setAccessibleDescription(QStringLiteral(
            "Alt+Up and Alt+Down move this stage in the chain; its menu has the same."));
        tab->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tab, &QWidget::customContextMenuRequested, this, [this, tab, stage = page.stage](const QPoint& pos) {
            popUpStageMenu(stage, tab->mapToGlobal(pos));
        });
    }
    return m_rail;
}

void MainWindow::buildPages()
{
    for (const PageEntry& entry : pageTable()) {
        Page* page = entry.make(m_context);
        page->setObjectName(QStringLiteral("page_") + entry.id);
        m_stack->addWidget(page);
        m_pages.append(page);
    }
    m_radio = m_stack->findChild<RadioPage*>();
    m_output = m_stack->findChild<OutputPage*>();
    m_nr = m_stack->findChild<NrPage*>();
    Q_ASSERT(m_radio && m_output && m_nr);
}

void MainWindow::wireGatePreset()
{
    // Upstream's setMode() is a preset that overwrites ratio and floor, so show what it
    // did. Going through the controls also re-sends them in order.
    connect(m_controls.value(QStringLiteral("gate.mode")), &ParamControl::valueChanged, this,
            [this](const QString&, double mode) {
                m_controls.value(QStringLiteral("gate.ratio"))->setValue(mode == 0.0 ? 2.0 : 10.0);
                m_controls.value(QStringLiteral("gate.floor"))->setValue(mode == 0.0 ? -15.0 : -40.0);
            });
}

void MainWindow::buildActions()
{
    m_saveAction = new QAction(QStringLiteral("Save"), this);
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAsAction = new QAction(QStringLiteral("Save As..."), this);
    m_saveAsAction->setShortcut(QKeySequence::SaveAs);
    m_deleteAction = new QAction(QStringLiteral("Delete"), this);
    m_importAction = new QAction(QStringLiteral("Import..."), this);
    m_exportAction = new QAction(QStringLiteral("Export..."), this);
    m_exportAllAction = new QAction(QStringLiteral("Export All..."), this);
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::onSaveProfile);
    connect(m_saveAsAction, &QAction::triggered, this, &MainWindow::onSaveProfileAs);
    connect(m_deleteAction, &QAction::triggered, this, &MainWindow::onDeleteProfile);
    connect(m_importAction, &QAction::triggered, this, &MainWindow::onImportProfiles);
    connect(m_exportAction, &QAction::triggered, this, &MainWindow::onExportProfile);
    connect(m_exportAllAction, &QAction::triggered, this, &MainWindow::onExportAllProfiles);

    m_findRadiosAction = new QAction(QStringLiteral("Find Radios..."), this);
    connect(m_findRadiosAction, &QAction::triggered, this, &MainWindow::findRadios);
    m_resetOrderAction = new QAction(QStringLiteral("Reset stage order"), this);
    m_resetOrderAction->setToolTip(QStringLiteral("Put the stages back in AetherRX's order: Gate, EQ, Compressor, Tube, Exciter."));
    connect(m_resetOrderAction, &QAction::triggered, this, [this] { setStageOrder(RxChain::stageIds()); });

    m_websiteAction = new QAction(QStringLiteral("Project Website"), this);
    connect(m_websiteAction, &QAction::triggered, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/VU3ESV/ExpertSDR3-Audio")));
    });
    m_aboutAction = new QAction(QStringLiteral("About %1").arg(QString::fromUtf8(kAppDisplayName)), this);
    m_aboutAction->setMenuRole(QAction::AboutRole);  // on a Mac, this belongs in the application menu
    connect(m_aboutAction, &QAction::triggered, this, &MainWindow::showAbout);

#ifndef Q_OS_MACOS
    // A Mac's application menu has its own; elsewhere the gear is where Quit lives. Through
    // close(), like every other way out, so closeEvent saves and the mute is restored.
    m_quitAction = new QAction(QStringLiteral("Quit"), this);
    m_quitAction->setShortcut(QKeySequence::Quit);
    m_quitAction->setMenuRole(QAction::QuitRole);
    connect(m_quitAction, &QAction::triggered, this, &QWidget::close);
#endif
}

void MainWindow::buildMenus()
{
    const auto addProfileActions = [this](QMenu* menu) {
        menu->addAction(m_saveAction);
        menu->addAction(m_saveAsAction);
        menu->addSeparator();
        menu->addAction(m_importAction);
        menu->addAction(m_exportAction);
        menu->addAction(m_exportAllAction);
        menu->addSeparator();
        menu->addAction(m_deleteAction);
    };

    menuBar()->addMenu(QStringLiteral("Radio"))->addAction(m_findRadiosAction);
    addProfileActions(menuBar()->addMenu(QStringLiteral("Profiles")));
    auto* help = menuBar()->addMenu(QStringLiteral("Help"));
    help->addAction(m_websiteAction);
    help->addAction(m_aboutAction);

    // The gear's menu: the same actions as the menu bar, and the rail's own command.
    m_gearMenu = new QMenu(this);
    m_gearMenu->setObjectName(QStringLiteral("gearMenu"));
    addProfileActions(m_gearMenu->addMenu(QStringLiteral("Profiles")));
    m_gearMenu->addAction(m_findRadiosAction);
    m_gearMenu->addAction(m_resetOrderAction);
    m_gearMenu->addSeparator();
    m_gearMenu->addAction(m_aboutAction);
    m_gearMenu->addAction(m_websiteAction);
    if (m_quitAction) {
        m_gearMenu->addSeparator();
        m_gearMenu->addAction(m_quitAction);
        addAction(m_quitAction);  // no menu bar has it, so the window keeps its shortcut live
    }

    // A native menu bar (a Mac's) costs the window nothing. Anywhere else it would paint a
    // strip across the top of the ground, so it stays hidden, and the window carries its
    // actions: a hidden menu's shortcuts would otherwise never fire.
    if (!menuBar()->isNativeMenuBar()) {
        menuBar()->hide();
        const auto menus = menuBar()->actions();
        for (QAction* menu : menus) {
            if (menu->menu()) addActions(menu->menu()->actions());
        }
    }
}

void MainWindow::popUpGearMenu()
{
    // Upwards from the gear at the foot of the rail, over the column it belongs to.
    auto* gear = m_rail->findChild<QPushButton*>(QStringLiteral("aetherRxSettingsButton"));
    const QPoint at = gear ? gear->mapToGlobal(QPoint(0, -m_gearMenu->sizeHint().height())) : QCursor::pos();
    m_gearMenu->popup(at);
}

void MainWindow::popUpStageMenu(const QString& stage, const QPoint& globalPos)
{
    const QStringList order = stageOrder();
    const int at = int(order.indexOf(stage));

    // The window's child, not the tab's: the rail's sheet would make a menu transparent.
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("stageMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QAction* up = menu->addAction(QStringLiteral("Move up"));
    up->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    up->setShortcutContext(Qt::WidgetShortcut);  // shown as a hint; the tab handles the keys
    up->setEnabled(at > 0);
    connect(up, &QAction::triggered, this, [this, stage] { moveStage(stage, -1); });
    QAction* down = menu->addAction(QStringLiteral("Move down"));
    down->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Down));
    down->setShortcutContext(Qt::WidgetShortcut);
    down->setEnabled(at >= 0 && at < int(order.size()) - 1);
    connect(down, &QAction::triggered, this, [this, stage] { moveStage(stage, 1); });
    menu->addSeparator();
    menu->addAction(m_resetOrderAction);
    menu->popup(globalPos);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        const auto stage = m_chainTabs.constFind(watched);
        if (stage != m_chainTabs.constEnd()) {
            const auto* key = static_cast<QKeyEvent*>(event);
            // A Mac marks arrow keys as keypad keys.
            const Qt::KeyboardModifiers modifiers = key->modifiers() & ~Qt::KeypadModifier;
            if (modifiers == Qt::AltModifier && (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)) {
                moveStage(*stage, key->key() == Qt::Key_Up ? -1 : 1);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::restoreWindowGeometry()
{
    QList<QRect> screens;
    QScreen* primary = QGuiApplication::primaryScreen();
    if (primary) screens.append(primary->availableGeometry());
    const auto all = QGuiApplication::screens();
    for (QScreen* screen : all) {
        if (screen != primary) screens.append(screen->availableGeometry());
    }

    const Placement place = placement(m_window.geometry, screens);
    if (place.positioned) {
        setGeometry(place.geometry);
    } else {
        resize(place.geometry.size());
    }
    if (place.maximised) setWindowState(windowState() | Qt::WindowMaximized);
}

MainWindow::Placement MainWindow::placement(const QString& savedGeometry, const QList<QRect>& screens)
{
    Placement place;
    const QRect primary = screens.value(0);  // empty when there is no screen at all
    QRect home = primary;
    const QRect saved = parseRect(savedGeometry);
    if (saved.isValid()) {
        // The screen it was on is the one under its top edge. If that screen has gone (a
        // monitor unplugged), keep the size and centre it on the main screen.
        const QPoint top(saved.center().x(), saved.top() + 8);
        const auto on = std::find_if(screens.begin(), screens.end(), [top](const QRect& s) { return s.contains(top); });
        if (on != screens.end()) {
            home = *on;
            place.geometry = saved;
            place.positioned = true;
        } else if (!primary.isEmpty()) {
            place.geometry = QRect(QPoint(), saved.size().boundedTo(primary.size()));
            place.geometry.moveCenter(primary.center());
            place.positioned = true;
        } else {
            place.geometry = QRect(QPoint(), saved.size());
        }
    } else {
        place.geometry = QRect(QPoint(), primary.isEmpty() ? defaultSize() : defaultSize().boundedTo(primary.size()));
    }
    place.maximised = !home.isEmpty() && isSmallScreen(home.size());
    return place;
}

bool MainWindow::isSmallScreen(const QSize& available)
{
    // Room for the usual size with a frame round it.
    return available.width() < 760 || available.height() < 790;
}

// ── Pages and the rail ──────────────────────────────────────────────────────

QStringList MainWindow::pageIds() const
{
    QStringList ids;
    for (const PageEntry& page : pageTable()) ids.append(page.id);
    return ids;
}

QString MainWindow::currentPage() const
{
    const int index = m_stack->currentIndex();
    return index >= 0 && index < pageTable().size() ? pageTable().at(index).id : QString();
}

void MainWindow::showPage(const QString& id)
{
    const QStringList ids = pageIds();
    const int index = int(ids.indexOf(id));
    if (index < 0) return;
    m_stack->setCurrentIndex(index);
    m_rail->setCurrentStage(pageTable().at(index).railId);
    m_window.page = id;
}

bool MainWindow::stageEnabled(int railId) const
{
    if (railId == kRailNr) return !m_config.nrMethod.isEmpty();
    const PageEntry* page = pageOfRail(railId);
    const ParamSpec* spec = page && !page->stage.isEmpty() ? findParamSpec(page->stage + QStringLiteral(".enabled")) : nullptr;
    return spec && paramValue(m_config, *spec) != 0.0;
}

void MainWindow::setStageEnabled(int railId, bool on)
{
    if (railId == kRailNr) {
        if (!on) {
            // Remembered, so ticking the box again brings back the same method.
            m_settings->setLastNrMethod(m_config.nrMethod);
            selectNrMethod(QString());
            return;
        }
        QString id = m_settings->lastNrMethod();
        if (id.isEmpty() && findNrMethod(QStringLiteral("nr2"))) id = QStringLiteral("nr2");
        if (id.isEmpty() && !nrMethods().isEmpty()) id = nrMethods().constFirst().id;
        selectNrMethod(id);
        return;
    }
    const PageEntry* page = pageOfRail(railId);
    if (!page || page->stage.isEmpty()) return;
    if (ParamControl* control = m_controls.value(page->stage + QStringLiteral(".enabled"))) {
        control->setValue(on ? 1.0 : 0.0);
    }
}

void MainWindow::selectNrMethod(const QString& id)
{
    m_config.nrMethod = id;
    if (!id.isEmpty()) m_settings->setLastNrMethod(id);
    m_nr->showMethod(id);
    onEngine([engine = m_engine, id] { engine->setNrMethod(id); });
    updateProfileState();
    m_rail->refreshFromHost();
}

QStringList MainWindow::stageOrder() const
{
    return RxChain::normalizedOrder(m_config.stageOrder);
}

void MainWindow::setStageOrder(const QStringList& requested)
{
    const QStringList order = RxChain::normalizedOrder(requested);
    if (order != stageOrder()) {
        m_config.stageOrder = order;
        onEngine([engine = m_engine, order] { engine->setStageOrder(order); });
        updateProfileState();
    }
    m_rail->refreshFromHost();
}

void MainWindow::moveStage(const QString& stage, int direction)
{
    QStringList order = stageOrder();
    const int from = int(order.indexOf(stage));
    const int to = from + direction;
    if (from < 0 || to < 0 || to >= int(order.size())) return;
    order.move(from, to);
    setStageOrder(order);
}

// ── Actions ─────────────────────────────────────────────────────────────────

void MainWindow::connectNow()
{
    m_radio->connectNow();
}

void MainWindow::onParamChanged(const QString& key, double value)
{
    m_config.params.insert(key, value);
    onEngine([engine = m_engine, key, value] { engine->setParam(key, value); });
    updateProfileState();
    if (key.endsWith(QLatin1String(".enabled"))) m_rail->refreshFromHost();
}

// ── Finding radios ──────────────────────────────────────────────────────────

void MainWindow::findRadios()
{
    RadioDiscoveryDialog dialog(TciDiscovery::localNetworkOptions(m_radio->port()), this);
    if (dialog.exec() != QDialog::Accepted) return;
    if (const auto radio = dialog.selectedRadio()) connectToRadio(*radio);
}

void MainWindow::connectToRadio(const DiscoveredRadio& radio)
{
    m_radio->connectTo(radio);
}

// ── Profiles ────────────────────────────────────────────────────────────────

bool MainWindow::chainModified() const
{
    const Profile* profile = m_profiles->find(m_config.profileName);
    return !sameChain(chainOf(m_config), profile ? profile->chain : defaultChain());
}

void MainWindow::refreshProfileList()
{
    QComboBox* list = m_header->profileList();
    const QSignalBlocker block(list);
    list->clear();
    list->addItem(QStringLiteral("Factory settings"), QString());
    const QStringList names = m_profiles->names();
    for (const QString& name : names) list->addItem(name, name);
    list->setCurrentIndex(std::max(0, list->findData(m_config.profileName)));
}

void MainWindow::updateProfileState()
{
    const bool named = m_profiles->find(m_config.profileName) != nullptr;
    const bool modified = chainModified();
    if (modified) m_profileNote.clear();  // whatever it said was about the state before this change
    m_saveAction->setEnabled(modified);
    m_deleteAction->setEnabled(named);
    m_exportAction->setEnabled(named);
    m_exportAllAction->setEnabled(!m_profiles->names().isEmpty());
    m_resetOrderAction->setEnabled(stageOrder() != RxChain::stageIds());
    if (!m_profileNote.isEmpty()) {
        m_header->setProfileState(QStringLiteral("Note"), m_profileNote);
    } else {
        m_header->setProfileState(modified ? QStringLiteral("Modified") : QString(), QString());
    }
}

void MainWindow::showChain()
{
    for (Page* page : std::as_const(m_pages)) page->showChain();
    for (const ParamSpec& spec : allParamSpecs()) {
        if (ParamControl* control = m_controls.value(spec.key)) control->setValueSilently(paramValue(m_config, spec));
    }
    m_rail->refreshFromHost();
}

void MainWindow::loadProfile(const QString& name)
{
    const Profile* profile = m_profiles->find(name);
    const ChainSettings chain = profile ? profile->chain : defaultChain();

    applyChain(m_config, chain);
    m_config.profileName = profile ? profile->name : QString();
    m_profileNote.clear();
    if (!chain.nrMethod.isEmpty() && !findNrMethod(chain.nrMethod)) {
        m_profileNote = QStringLiteral("This profile uses %1 noise reduction, which is not available on this computer, so NR is off.")
                            .arg(chain.nrMethod.toUpper());
    }

    showChain();
    onEngine([engine = m_engine, chain] { engine->applyChainSettings(chain); });
    refreshProfileList();
    updateProfileState();
}

bool MainWindow::saveProfileAs(const QString& name, QString* error)
{
    const QString cleaned = ProfileLibrary::cleanName(name);
    if (!m_profiles->save(cleaned, chainOf(m_config), error)) return false;
    m_config.profileName = m_profiles->find(cleaned)->name;
    m_profileNote.clear();
    refreshProfileList();
    updateProfileState();
    return true;
}

bool MainWindow::deleteProfile(const QString& name, QString* error)
{
    if (!m_profiles->remove(name, error)) return false;
    // What is playing does not change. It just no longer has a name.
    if (m_config.profileName.compare(name, Qt::CaseInsensitive) == 0) m_config.profileName.clear();
    refreshProfileList();
    updateProfileState();
    return true;
}

ImportResult MainWindow::importProfiles(const QString& path)
{
    ImportResult result = m_profiles->importFile(path);
    refreshProfileList();
    updateProfileState();
    return result;
}

void MainWindow::onProfileSelected(int index)
{
    const QString name = m_header->profileList()->itemData(index).toString();
    if (name == m_config.profileName) return;
    if (chainModified()) {
        const QString from = m_config.profileName.isEmpty() ? QStringLiteral("the current settings") : m_config.profileName;
        const auto answer = QMessageBox::question(this, QStringLiteral("Unsaved changes"),
            QStringLiteral("Discard your changes to %1?").arg(from),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Discard) {
            refreshProfileList();  // back to the one still in use
            return;
        }
    }
    loadProfile(name);
}

void MainWindow::onSaveProfile()
{
    if (m_config.profileName.isEmpty()) {
        onSaveProfileAs();
        return;
    }
    QString error;
    if (!saveProfileAs(m_config.profileName, &error)) {
        QMessageBox::warning(this, QStringLiteral("Could not save the profile"), error);
    }
}

void MainWindow::onSaveProfileAs()
{
    bool ok = false;
    const QString name = ProfileLibrary::cleanName(QInputDialog::getText(this, QStringLiteral("Save Profile"),
        QStringLiteral("Name for this profile:"), QLineEdit::Normal, m_config.profileName, &ok));
    if (!ok || name.isEmpty()) return;
    if (m_profiles->find(name) && name.compare(m_config.profileName, Qt::CaseInsensitive) != 0) {
        const auto answer = QMessageBox::question(this, QStringLiteral("Replace profile"),
            QStringLiteral("There is already a profile called %1. Replace it?").arg(name),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) return;
    }
    QString error;
    if (!saveProfileAs(name, &error)) {
        QMessageBox::warning(this, QStringLiteral("Could not save the profile"), error);
    }
}

void MainWindow::onDeleteProfile()
{
    const QString name = m_config.profileName;
    if (name.isEmpty()) return;
    const auto answer = QMessageBox::question(this, QStringLiteral("Delete profile"),
        QStringLiteral("Delete the profile %1? The current sound does not change.").arg(name),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;
    QString error;
    if (!deleteProfile(name, &error)) {
        QMessageBox::warning(this, QStringLiteral("Could not delete the profile"), error);
    }
}

void MainWindow::onImportProfiles()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import Profiles"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        QStringLiteral("ExpertSDR3 Audio profiles (*.json);;All files (*)"));
    if (path.isEmpty()) return;

    const ImportResult result = importProfiles(path);
    if (!result.error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Could not import"), result.error);
        return;
    }
    QString message = result.imported.size() == 1
                          ? QStringLiteral("Imported the profile %1.").arg(result.imported.first())
                          : QStringLiteral("Imported %1 profiles: %2.").arg(result.imported.size()).arg(result.imported.join(QStringLiteral(", ")));
    if (!result.renamed.isEmpty()) {
        message += QStringLiteral("\n\nRenamed, because the name was taken:\n") + result.renamed.join(QLatin1Char('\n'));
    }
    message += QStringLiteral("\n\nChoose one from the Profile list to use it.");
    QMessageBox::information(this, QStringLiteral("Profiles imported"), message);
}

bool MainWindow::exportWithDialog(const QStringList& names, const QString& suggestedName)
{
    QString fileName = suggestedName;
    fileName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._ -]")), QStringLiteral("-"));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export Profiles"),
        QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(fileName + QStringLiteral(".json")),
        QStringLiteral("ExpertSDR3 Audio profiles (*.json)"));
    if (path.isEmpty()) return false;
    QString error;
    if (!m_profiles->exportFile(path, names, &error)) {
        QMessageBox::warning(this, QStringLiteral("Could not export"), error);
        return false;
    }
    return true;
}

void MainWindow::onExportProfile()
{
    // The saved profile, not what is playing: unsaved changes are not part of it yet.
    if (!m_config.profileName.isEmpty()) exportWithDialog({m_config.profileName}, m_config.profileName);
}

void MainWindow::onExportAllProfiles()
{
    exportWithDialog(m_profiles->names(), QStringLiteral("ExpertSDR3 Audio profiles"));
}

void MainWindow::showAbout()
{
    AboutDialog(this).exec();
}

// ── Saving and closing ──────────────────────────────────────────────────────

void MainWindow::saveSettings()
{
    m_settings->save(m_config);
    if (!m_savesWindowState) return;
    m_window.page = currentPage();
    // Not maximised, so a window maximised on a small screen comes back at its own size.
    if (isVisible()) m_window.geometry = rectText(normalGeometry());
    m_settings->saveWindowState(m_window);
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (m_shownOnce) return;
    m_shownOnce = true;
    // The keyboard starts on the rail, at the page showing.
    if (const PageEntry* page = findPage(currentPage())) {
        if (auto* tab = m_rail->findChild<QPushButton*>(QStringLiteral("aetherRxTab") + railSlug(*page))) {
            tab->setFocus(Qt::OtherFocusReason);
        }
    }
}

// Every way out comes here: close(), the frame's close button, Alt+F4, Cmd+Q, Quit, and
// main()'s SIGINT and SIGTERM handling. Escape is not one: a main window has no Escape.
void MainWindow::closeEvent(QCloseEvent* event)
{
    saveSettings();
    event->accept();
    if (m_quit) m_quit();  // main() then stops the engine, restoring ExpertSDR3's mute
}

// ── Status ──────────────────────────────────────────────────────────────────

void MainWindow::onStatus(const EngineStatus& s)
{
    for (Page* page : std::as_const(m_pages)) page->showStatus(s);
    m_header->statusChip()->showStatus(s);
    m_rail->refreshFromHost();
}

} // namespace esa
