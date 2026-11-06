#include "gui/MainWindow.h"

#include "dsp/NrProcessor.h"
#include "dsp/RxChain.h"
#include "gui/AboutDialog.h"
#include "gui/RadioDiscoveryDialog.h"

#include "ClientEq.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabWidget>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace esa {

namespace {

const QString kGreen = QStringLiteral("#3fb950");
const QString kAmber = QStringLiteral("#d29922");
const QString kRed = QStringLiteral("#f85149");
const QString kMuted = QStringLiteral("#8b949e");

QString colored(const QString& color, const QString& text)
{
    return QStringLiteral("<span style='color:%1'>%2</span>").arg(color, text.toHtmlEscaped());
}

QLabel* hintLabel(const QString& text = QString())
{
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    return label;
}

QString formatFrequency(qint64 hz)
{
    return QStringLiteral("%1 MHz").arg(double(hz) / 1e6, 0, 'f', 3);
}

// Wraps a parameter panel so a short window scrolls instead of squashing rows.
QWidget* scrollable(QWidget* content)
{
    auto* scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

} // namespace

MainWindow::MainWindow(Engine* engine, Settings* settings, ProfileLibrary* profiles, const AppConfig& config)
    : m_engine(engine), m_settings(settings), m_profiles(profiles), m_config(config)
{
    setWindowTitle(QStringLiteral("ExpertSDR3 Audio"));
    applyTheme();

    auto* central = new QWidget;
    auto* layout = new QVBoxLayout(central);
    layout->addWidget(buildConnectionBox());
    layout->addWidget(buildOutputBox());
    layout->addWidget(buildProfileBar());
    layout->addWidget(buildTabs(), 1);
    layout->addWidget(buildMeters());
    setCentralWidget(central);
    buildMenus();

    // The profile may have been deleted since these settings were saved.
    if (!m_config.profileName.isEmpty() && !m_profiles->find(m_config.profileName)) m_config.profileName.clear();
    m_profileNote = m_profiles->loadWarning();
    refreshProfileList();
    updateProfileState();

    // Fits a small display such as a Raspberry Pi touchscreen. The tab panels scroll,
    // so the window can shrink a long way below this.
    const QScreen* screen = QApplication::primaryScreen();
    resize(720, screen ? std::min(860, screen->availableGeometry().height() - 60) : 860);

    connect(m_engine, &Engine::statusChanged, this, &MainWindow::onStatus, Qt::QueuedConnection);
    connect(m_engine, &Engine::devicesChanged, this, &MainWindow::onDevices, Qt::QueuedConnection);
    connect(m_engine, &Engine::connectedPortChanged, this, [this](int port) {
        // Port auto-detection may have found the server on another port. Keep it.
        const QSignalBlocker block(m_port);
        m_port->setValue(port);
        m_config.port = port;
    }, Qt::QueuedConnection);

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

void MainWindow::applyTheme()
{
    // One look on every platform, and meters that read well in a dim shack.
    QPalette p;
    // Base is lighter than the window so an unchecked box stands out against it.
    const QColor window(32, 34, 37), base(52, 56, 62), text(220, 223, 228);
    const QColor mid(60, 64, 70), disabled(120, 124, 130), accent(46, 134, 193);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(48, 51, 56));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Mid, mid);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    QApplication::setPalette(p);
    setStyleSheet(QStringLiteral(
        "QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { background: #2a2d32; }"
        "QGroupBox { border: 1px solid #3c4046; border-radius: 6px; margin-top: 10px; padding-top: 6px; font-weight: 600; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }"
        "QProgressBar { border: 1px solid #3c4046; border-radius: 3px; background: #16181b; max-height: 12px; }"
        "QProgressBar::chunk { background: #3fb950; border-radius: 2px; }"));
}

QWidget* MainWindow::buildConnectionBox()
{
    auto* box = new QGroupBox(QStringLiteral("ExpertSDR3 (TCI)"));
    auto* grid = new QGridLayout(box);

    m_host = new QLineEdit(m_config.host);
    m_host->setToolTip(QStringLiteral("Computer running ExpertSDR3. Enable TCI in its Options."));
    m_port = new QSpinBox;
    m_port->setRange(1, 65535);
    m_port->setValue(m_config.port);
    m_port->setToolTip(QStringLiteral("If nothing answers here, the usual TCI ports are tried automatically."));
    m_receiver = new QComboBox;
    for (int i = 0; i < 2; ++i) m_receiver->addItem(QStringLiteral("RX %1").arg(i + 1), i);
    m_receiver->setCurrentIndex(std::max(0, m_receiver->findData(m_config.receiver)));
    m_connect = new QPushButton(QStringLiteral("Connect"));
    m_connect->setCheckable(true);
    m_connect->setMinimumWidth(110);
    m_findRadios = new QPushButton(QStringLiteral("Find Radios..."));
    m_findRadios->setToolTip(QStringLiteral("Look on your network for computers running ExpertSDR3 with TCI enabled."));
    connect(m_findRadios, &QPushButton::clicked, this, &MainWindow::findRadios);
    m_radioLabel = new QLabel(colored(kMuted, QStringLiteral("Not connected")));
    m_radioLabel->setTextFormat(Qt::RichText);

    grid->addWidget(new QLabel(QStringLiteral("Host")), 0, 0);
    grid->addWidget(m_host, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("Port")), 0, 2);
    grid->addWidget(m_port, 0, 3);
    grid->addWidget(new QLabel(QStringLiteral("Receiver")), 0, 4);
    grid->addWidget(m_receiver, 0, 5);
    grid->addWidget(m_findRadios, 0, 6);
    grid->addWidget(m_connect, 0, 7);
    grid->addWidget(m_radioLabel, 1, 0, 1, 8);
    grid->setColumnStretch(1, 1);

    connect(m_connect, &QPushButton::toggled, this, &MainWindow::onConnectToggled);
    connect(m_receiver, &QComboBox::currentIndexChanged, this, [this](int index) {
        const int receiver = m_receiver->itemData(index).toInt();
        m_config.receiver = receiver;
        onEngine([engine = m_engine, receiver] { engine->setReceiver(receiver); });
    });
    return box;
}

QWidget* MainWindow::buildOutputBox()
{
    auto* box = new QGroupBox(QStringLiteral("Audio output"));
    auto* grid = new QGridLayout(box);

    m_device = new QComboBox;
    m_device->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    auto* refresh = new QPushButton(QStringLiteral("Refresh"));
    refresh->setToolTip(QStringLiteral("Look again for audio devices, e.g. after plugging one in."));

    m_buffer = new QSpinBox;
    m_buffer->setRange(10, 500);
    m_buffer->setSuffix(QStringLiteral(" ms"));
    m_buffer->setValue(m_config.bufferMs);
    m_buffer->setToolTip(QStringLiteral("Lower is less delay; raise it if you hear dropouts. Grows by itself after an underrun."));

    m_gain = new QSlider(Qt::Horizontal);
    m_gain->setRange(-60, 30);
    m_gain->setValue(int(m_config.outputGainDb));
    m_gainLabel = new QLabel;
    m_gainLabel->setMinimumWidth(52);
    m_gainLabel->setText(QStringLiteral("%1 dB").arg(m_gain->value()));

    m_boost = new QCheckBox(QStringLiteral("Soft boost"));
    m_boost->setChecked(m_config.boost);
    m_boost->setToolTip(QStringLiteral("About 2x louder on quiet signals, never past full scale."));
    m_muteOutput = new QPushButton(QStringLiteral("Mute"));
    m_muteOutput->setCheckable(true);

    m_muteExpert = new QCheckBox(QStringLiteral("Mute ExpertSDR3's own audio while this app is playing"));
    m_muteExpert->setChecked(m_config.muteExpert);
    m_muteExpert->setToolTip(QStringLiteral(
        "ExpertSDR3 is restored to how it was found when you disconnect or quit."));
    m_muteStatus = hintLabel();
    m_outputStatus = hintLabel();

    grid->addWidget(new QLabel(QStringLiteral("Device")), 0, 0);
    grid->addWidget(m_device, 0, 1, 1, 3);
    grid->addWidget(refresh, 0, 4);
    grid->addWidget(new QLabel(QStringLiteral("Volume")), 1, 0);
    grid->addWidget(m_gain, 1, 1);
    grid->addWidget(m_gainLabel, 1, 2);
    grid->addWidget(m_boost, 1, 3);
    grid->addWidget(m_muteOutput, 1, 4);
    grid->addWidget(new QLabel(QStringLiteral("Buffer")), 2, 0);
    grid->addWidget(m_buffer, 2, 1);
    grid->addWidget(m_outputStatus, 2, 2, 1, 3);
    grid->addWidget(m_muteExpert, 3, 0, 1, 3);
    grid->addWidget(m_muteStatus, 3, 3, 1, 2);
    grid->setColumnStretch(1, 1);

    connect(m_device, &QComboBox::currentIndexChanged, this, &MainWindow::onDeviceSelected);
    connect(refresh, &QPushButton::clicked, this, [this] {
        onEngine([engine = m_engine] { engine->refreshDevices(); });
    });
    connect(m_buffer, &QSpinBox::valueChanged, this, [this](int ms) {
        m_config.bufferMs = ms;
        onEngine([engine = m_engine, ms] { engine->setBufferMs(ms); });
    });
    connect(m_gain, &QSlider::valueChanged, this, [this](int db) {
        m_gainLabel->setText(QStringLiteral("%1 dB").arg(db));
        m_config.outputGainDb = db;
        onEngine([engine = m_engine, db] { engine->setOutputGainDb(db); });
    });
    connect(m_boost, &QCheckBox::toggled, this, [this](bool on) {
        m_config.boost = on;
        onEngine([engine = m_engine, on] { engine->setBoost(on); });
    });
    connect(m_muteOutput, &QPushButton::toggled, this, [this](bool on) {
        onEngine([engine = m_engine, on] { engine->setOutputMuted(on); });
    });
    connect(m_muteExpert, &QCheckBox::toggled, this, [this](bool on) {
        m_config.muteExpert = on;
        onEngine([engine = m_engine, on] { engine->setMuteExpert(on); });
    });
    return box;
}

QTabWidget* MainWindow::buildTabs()
{
    // The same order, on the same edge, as the AetherRX window upstream.
    auto* tabs = new QTabWidget;
    tabs->setTabPosition(QTabWidget::West);
    tabs->setUsesScrollButtons(true);  // lets the tab bar shrink on a short screen
    tabs->addTab(buildNrTab(), QStringLiteral("NR"));
    tabs->addTab(buildStageTab(QStringLiteral("gate")), QStringLiteral("Gate"));
    tabs->addTab(buildEqTab(), QStringLiteral("EQ"));
    tabs->addTab(buildStageTab(QStringLiteral("comp")), QStringLiteral("Comp"));
    tabs->addTab(buildStageTab(QStringLiteral("tube")), QStringLiteral("Tube"));
    tabs->addTab(buildStageTab(QStringLiteral("pudu")), QStringLiteral("AetherVoice"));
    tabs->addTab(buildOrderTab(), QStringLiteral("Order"));
    return tabs;
}

QWidget* MainWindow::buildProfileBar()
{
    auto* bar = new QWidget;
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(4, 0, 4, 0);

    m_profileList = new QComboBox;
    m_profileList->setToolTip(QStringLiteral("A saved set of everything in the tabs below: noise reduction, its settings, and the stages."));
    m_profileStatus = new QLabel;
    m_profileStatus->setTextFormat(Qt::RichText);
    connect(m_profileList, &QComboBox::activated, this, &MainWindow::onProfileSelected);

    m_saveAction = new QAction(QStringLiteral("Save"), this);
    m_saveAsAction = new QAction(QStringLiteral("Save As..."), this);
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

    const auto addButton = [&](QAction* action) {
        auto* button = new QPushButton(action->text());
        connect(button, &QPushButton::clicked, action, &QAction::trigger);
        connect(action, &QAction::changed, button, [button, action] { button->setEnabled(action->isEnabled()); });
        layout->addWidget(button);
    };
    layout->addWidget(new QLabel(QStringLiteral("Profile")));
    layout->addWidget(m_profileList, 1);
    layout->addWidget(m_profileStatus);
    addButton(m_saveAction);
    addButton(m_saveAsAction);
    addButton(m_importAction);
    addButton(m_exportAction);
    addButton(m_deleteAction);
    return bar;
}

void MainWindow::buildMenus()
{
    auto* radio = menuBar()->addMenu(QStringLiteral("Radio"));
    connect(radio->addAction(QStringLiteral("Find Radios...")), &QAction::triggered, this, &MainWindow::findRadios);

    auto* profiles = menuBar()->addMenu(QStringLiteral("Profiles"));
    profiles->addAction(m_saveAction);
    profiles->addAction(m_saveAsAction);
    profiles->addSeparator();
    profiles->addAction(m_importAction);
    profiles->addAction(m_exportAction);
    profiles->addAction(m_exportAllAction);
    profiles->addSeparator();
    profiles->addAction(m_deleteAction);

    auto* help = menuBar()->addMenu(QStringLiteral("Help"));
    connect(help->addAction(QStringLiteral("Project Website")), &QAction::triggered, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/VU3ESV/ExpertSDR3-Audio")));
    });
    auto* about = help->addAction(QStringLiteral("About ExpertSDR3 Audio"));
    about->setMenuRole(QAction::AboutRole);  // on a Mac, this belongs in the application menu
    connect(about, &QAction::triggered, this, &MainWindow::showAbout);
}

ParamControl* MainWindow::addParam(const ParamSpec& spec, QGridLayout* grid, int row)
{
    auto* control = new ParamControl(spec, paramValue(m_config, spec), grid, row, this);
    m_controls.insert(spec.key, control);
    m_editors.insert(spec.key, [control](double value) { control->setValueSilently(value); });
    connect(control, &ParamControl::valueChanged, this, &MainWindow::onParamChanged);
    return control;
}

QWidget* MainWindow::buildNrTab()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("Method")));
    m_nrMethod = new QComboBox;
    m_nrMethod->addItem(QStringLiteral("Off"), QString());
    for (const NrMethod& method : nrMethods()) m_nrMethod->addItem(method.label, method.id);
    top->addWidget(m_nrMethod, 1);
    layout->addLayout(top);

    m_nrDescription = hintLabel();
    layout->addWidget(m_nrDescription);

    m_nrParams = new QStackedWidget;
    m_nrParams->addWidget(new QWidget);  // "Off" has no parameters
    for (const NrMethod& method : nrMethods()) {
        auto* panel = new QWidget;
        auto* grid = new QGridLayout(panel);
        int row = 0;
        for (const ParamSpec& spec : method.params) addParam(spec, grid, row++);
        grid->setRowStretch(row, 1);
        grid->setColumnStretch(1, 1);
        m_nrParams->addWidget(scrollable(panel));
    }
    layout->addWidget(m_nrParams, 1);

    m_autoBypass = new QCheckBox(QStringLiteral("Bypass NR automatically on CW and digital modes"));
    m_autoBypass->setChecked(m_config.nrAutoBypass);
    m_autoBypass->setToolTip(QStringLiteral(
        "These are not speech. A speech model treats a steady carrier as noise and can remove the signal itself."));
    connect(m_autoBypass, &QCheckBox::toggled, this, [this](bool on) {
        m_config.nrAutoBypass = on;
        onEngine([engine = m_engine, on] { engine->setNrAutoBypass(on); });
        updateProfileState();
    });
    layout->addWidget(m_autoBypass);

    m_nrStatus = hintLabel();
    layout->addWidget(m_nrStatus);

    connect(m_nrMethod, &QComboBox::currentIndexChanged, this, &MainWindow::onNrSelected);
    showNrMethod(m_config.nrMethod);
    return page;
}

QWidget* MainWindow::buildStageTab(const QString& group)
{
    auto* panel = new QWidget;
    auto* grid = new QGridLayout(panel);
    int row = 0;
    for (const ParamSpec& spec : RxChain::stageParams()) {
        if (spec.key.section(QLatin1Char('.'), 0, 0) == group) addParam(spec, grid, row++);
    }
    grid->setRowStretch(row, 1);
    grid->setColumnStretch(1, 1);

    if (group == QLatin1String("gate")) {
        // Upstream's setMode() is a preset that overwrites ratio and floor, so show
        // what it did. Going through the controls also re-sends them in order.
        connect(m_controls.value(QStringLiteral("gate.mode")), &ParamControl::valueChanged, this,
                [this](const QString&, double mode) {
                    m_controls.value(QStringLiteral("gate.ratio"))->setValue(mode == 0.0 ? 2.0 : 10.0);
                    m_controls.value(QStringLiteral("gate.floor"))->setValue(mode == 0.0 ? -15.0 : -40.0);
                });
    }
    return scrollable(panel);
}

QWidget* MainWindow::buildEqTab()
{
    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);

    auto* general = new QGridLayout;
    int row = 0;
    addParam(*findParamSpec(QStringLiteral("eq.enabled")), general, row++);
    addParam(*findParamSpec(QStringLiteral("eq.master")), general, row++);
    addParam(*findParamSpec(QStringLiteral("eq.family")), general, row++);
    general->setColumnStretch(1, 1);
    layout->addLayout(general);

    // A compact table rather than the full slider row per value: ten bands of three
    // parameters each would otherwise be a very long scroll.
    auto* table = new QGridLayout;
    const QStringList headers = {QStringLiteral("Band"), QStringLiteral("Frequency"), QStringLiteral("Gain"), QStringLiteral("Q")};
    for (int c = 0; c < headers.size(); ++c) {
        table->addWidget(new QLabel(colored(kMuted, headers.at(c))), 0, c);
    }
    using AetherSDR::ClientEq;
    for (int band = 0; band < ClientEq::kDefaultBandCount; ++band) {
        const int tableRow = band + 1;
        const auto keyFor = [band](const char* leaf) {
            return QStringLiteral("eq.b%1.%2").arg(band).arg(QLatin1String(leaf));
        };

        const ParamSpec* onSpec = findParamSpec(keyFor("on"));
        auto* on = new QCheckBox(onSpec->label);
        on->setChecked(paramValue(m_config, *onSpec) != 0.0);
        table->addWidget(on, tableRow, 0);
        m_editors.insert(onSpec->key, [on](double value) {
            const QSignalBlocker block(on);
            on->setChecked(value != 0.0);
        });
        connect(on, &QCheckBox::toggled, this, [this, key = onSpec->key](bool checked) {
            onParamChanged(key, checked ? 1.0 : 0.0);
        });

        const auto addSpin = [&](const char* leaf, int column) {
            const ParamSpec* spec = findParamSpec(keyFor(leaf));
            auto* spin = new QDoubleSpinBox;
            spin->setRange(spec->min, spec->max);
            spin->setDecimals(spec->decimals);
            spin->setKeyboardTracking(false);
            spin->setValue(paramValue(m_config, *spec));
            if (!spec->unit.isEmpty()) spin->setSuffix(QLatin1Char(' ') + spec->unit);
            table->addWidget(spin, tableRow, column);
            m_editors.insert(spec->key, [spin](double value) {
                const QSignalBlocker block(spin);
                spin->setValue(value);
            });
            connect(spin, &QDoubleSpinBox::valueChanged, this, [this, key = spec->key](double value) {
                onParamChanged(key, value);
            });
            return spin;
        };
        addSpin("freq", 1);
        QDoubleSpinBox* gain = addSpin("gain", 2);
        addSpin("q", 3);

        // High- and low-pass have no gain to set.
        const auto type = ClientEq::defaultBand(band).type;
        if (type == ClientEq::FilterType::HighPass || type == ClientEq::FilterType::LowPass) {
            gain->setEnabled(false);
        }
    }
    table->setColumnStretch(0, 1);
    layout->addLayout(table);
    layout->addStretch(1);
    return scrollable(panel);
}

QWidget* MainWindow::buildOrderTab()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(hintLabel(colored(kMuted, QStringLiteral(
        "NR always runs first. The stages after it run top to bottom."))));

    auto* row = new QHBoxLayout;
    m_order = new QListWidget;
    setOrderList(m_config.stageOrder);
    row->addWidget(m_order, 1);

    auto* buttons = new QVBoxLayout;
    auto* up = new QPushButton(QStringLiteral("Move up"));
    auto* down = new QPushButton(QStringLiteral("Move down"));
    auto* reset = new QPushButton(QStringLiteral("Default"));
    buttons->addWidget(up);
    buttons->addWidget(down);
    buttons->addWidget(reset);
    buttons->addStretch(1);
    row->addLayout(buttons);
    layout->addLayout(row, 1);

    connect(up, &QPushButton::clicked, this, [this] { moveStage(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveStage(1); });
    connect(reset, &QPushButton::clicked, this, [this] {
        setOrderList(RxChain::stageIds());
        pushStageOrder();
    });
    return page;
}

QWidget* MainWindow::buildMeters()
{
    auto* box = new QFrame;
    auto* grid = new QGridLayout(box);
    grid->setContentsMargins(4, 0, 4, 0);

    const auto makeMeter = [] {
        auto* meter = new QProgressBar;
        meter->setRange(-60, 0);
        meter->setValue(-60);
        meter->setTextVisible(false);
        return meter;
    };
    m_inputMeter = makeMeter();
    m_outputMeter = makeMeter();
    m_inputValue = new QLabel(QStringLiteral("-- dB"));
    m_outputValue = new QLabel(QStringLiteral("-- dB"));
    m_inputValue->setMinimumWidth(60);
    m_outputValue->setMinimumWidth(60);
    m_stats = hintLabel();

    grid->addWidget(new QLabel(QStringLiteral("From radio")), 0, 0);
    grid->addWidget(m_inputMeter, 0, 1);
    grid->addWidget(m_inputValue, 0, 2);
    grid->addWidget(new QLabel(QStringLiteral("To speakers")), 1, 0);
    grid->addWidget(m_outputMeter, 1, 1);
    grid->addWidget(m_outputValue, 1, 2);
    grid->addWidget(m_stats, 2, 0, 1, 3);
    grid->setColumnStretch(1, 1);
    return box;
}

// ── Actions ─────────────────────────────────────────────────────────────────

void MainWindow::connectNow()
{
    m_connect->setChecked(true);
}

void MainWindow::onConnectToggled(bool on)
{
    if (on) {
        m_config.host = m_host->text().trimmed();
        m_config.port = m_port->value();
        const AppConfig config = m_config;
        onEngine([engine = m_engine, config] {
            engine->configure(config);
            engine->connectRadio();
        });
    } else {
        onEngine([engine = m_engine] { engine->disconnectRadio(); });
    }
    m_connect->setText(on ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
    m_host->setEnabled(!on);
    m_port->setEnabled(!on);
}

void MainWindow::onDevices(const QList<AudioDeviceInfo>& devices)
{
    const QSignalBlocker block(m_device);
    const QString selected = m_config.deviceKey;
    m_device->clear();
    m_device->addItem(QStringLiteral("System default"), QString());
    for (const AudioDeviceInfo& device : devices) {
        QString label = QStringLiteral("%1  (%2)").arg(device.name, device.hostApi);
        if (device.isDefault) label += QStringLiteral("  *");
        m_device->addItem(label, device.key);
    }
    if (devices.isEmpty()) {
        m_outputStatus->setText(colored(kRed, QStringLiteral("No audio output devices found")));
    }
    const int index = m_device->findData(selected);
    if (index >= 0) {
        m_device->setCurrentIndex(index);
    } else {
        // The saved device is not plugged in right now. Show it rather than silently
        // switching the setting, so plugging it back in restores it.
        m_device->addItem(QStringLiteral("%1 (not found)").arg(selected.section(QLatin1Char('|'), 1)), selected);
        m_device->setCurrentIndex(selected.isEmpty() ? 0 : m_device->count() - 1);
    }
}

void MainWindow::onDeviceSelected(int index)
{
    const QString key = m_device->itemData(index).toString();
    m_config.deviceKey = key;
    onEngine([engine = m_engine, key] { engine->setOutputDevice(key); });
}

void MainWindow::showNrMethod(const QString& id)
{
    const int index = std::max(0, m_nrMethod->findData(id));
    {
        const QSignalBlocker block(m_nrMethod);
        m_nrMethod->setCurrentIndex(index);
    }
    m_nrParams->setCurrentIndex(index);
    const NrMethod* method = findNrMethod(id);
    m_nrDescription->setText(colored(kMuted, method ? method->description : QStringLiteral("Audio passes through unchanged.")));
}

void MainWindow::onNrSelected(int index)
{
    const QString id = m_nrMethod->itemData(index).toString();
    m_config.nrMethod = id;
    showNrMethod(id);
    onEngine([engine = m_engine, id] { engine->setNrMethod(id); });
    updateProfileState();
}

void MainWindow::onParamChanged(const QString& key, double value)
{
    m_config.params.insert(key, value);
    onEngine([engine = m_engine, key, value] { engine->setParam(key, value); });
    updateProfileState();
}

void MainWindow::moveStage(int direction)
{
    const int from = m_order->currentRow();
    const int to = from + direction;
    if (from < 0 || to < 0 || to >= m_order->count()) return;
    QListWidgetItem* item = m_order->takeItem(from);
    m_order->insertItem(to, item);
    m_order->setCurrentRow(to);
    pushStageOrder();
}

void MainWindow::pushStageOrder()
{
    QStringList order;
    for (int i = 0; i < m_order->count(); ++i) order.append(m_order->item(i)->data(Qt::UserRole).toString());
    m_config.stageOrder = order;
    onEngine([engine = m_engine, order] { engine->setStageOrder(order); });
    updateProfileState();
}

void MainWindow::setOrderList(const QStringList& order)
{
    m_order->clear();
    const QStringList ids = RxChain::normalizedOrder(order);
    for (const QString& id : ids) {
        auto* item = new QListWidgetItem(RxChain::stageLabel(id));
        item->setData(Qt::UserRole, id);
        m_order->addItem(item);
    }
}

// ── Finding radios ──────────────────────────────────────────────────────────

void MainWindow::findRadios()
{
    RadioDiscoveryDialog dialog(TciDiscovery::localNetworkOptions(m_port->value()), this);
    if (dialog.exec() != QDialog::Accepted) return;
    if (const auto radio = dialog.selectedRadio()) connectToRadio(*radio);
}

void MainWindow::connectToRadio(const DiscoveredRadio& radio)
{
    if (m_connect->isChecked()) m_connect->setChecked(false);
    m_host->setText(radio.host);
    {
        const QSignalBlocker block(m_port);
        m_port->setValue(radio.port);
    }
    // This is the radio the operator picked. If it goes away, wait for it rather than wandering
    // off to whatever else answers on the usual ports.
    m_config.fallbackPorts.clear();
    m_connect->setChecked(true);
}

// ── Profiles ────────────────────────────────────────────────────────────────

bool MainWindow::chainModified() const
{
    const Profile* profile = m_profiles->find(m_config.profileName);
    return !sameChain(chainOf(m_config), profile ? profile->chain : defaultChain());
}

void MainWindow::refreshProfileList()
{
    const QSignalBlocker block(m_profileList);
    m_profileList->clear();
    m_profileList->addItem(QStringLiteral("Factory settings"), QString());
    const QStringList names = m_profiles->names();
    for (const QString& name : names) m_profileList->addItem(name, name);
    m_profileList->setCurrentIndex(std::max(0, m_profileList->findData(m_config.profileName)));
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
    if (!m_profileNote.isEmpty()) {
        m_profileStatus->setText(colored(kAmber, QStringLiteral("Note")));
        m_profileStatus->setToolTip(m_profileNote);
    } else {
        m_profileStatus->setText(modified ? colored(kAmber, QStringLiteral("Modified")) : QString());
        m_profileStatus->setToolTip(QString());
    }
}

void MainWindow::showChain()
{
    showNrMethod(m_config.nrMethod);
    {
        const QSignalBlocker block(m_autoBypass);
        m_autoBypass->setChecked(m_config.nrAutoBypass);
    }
    setOrderList(m_config.stageOrder);
    for (const ParamSpec& spec : allParamSpecs()) {
        const auto editor = m_editors.constFind(spec.key);
        if (editor != m_editors.constEnd()) (*editor)(paramValue(m_config, spec));
    }
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
    const QString name = m_profileList->itemData(index).toString();
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

void MainWindow::saveSettings()
{
    m_settings->save(m_config);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveSettings();
    event->accept();
    QApplication::quit();  // main() then stops the engine, restoring ExpertSDR3's mute
}

// ── Status ──────────────────────────────────────────────────────────────────

void MainWindow::onStatus(const EngineStatus& s)
{
    // Connection line.
    QString radio;
    switch (s.connection) {
    case TciClient::State::Disconnected:
        radio = colored(kMuted, QStringLiteral("Not connected"));
        break;
    case TciClient::State::Connecting: {
        radio = colored(kAmber, QStringLiteral("Looking for ExpertSDR3 on port %1...").arg(s.connectedPort));
        if (!s.connectionError.isEmpty()) {
            radio += QStringLiteral(" ") + colored(kMuted, QStringLiteral("(is TCI enabled in its options?)"));
        }
        break;
    }
    case TciClient::State::Initializing:
        radio = colored(kAmber, QStringLiteral("Connected, reading radio state..."));
        break;
    case TciClient::State::Ready: {
        QString line = QStringLiteral("%1 %2  ·  %3").arg(s.serverName, s.serverVersion, s.device);
        if (s.frequencyHz > 0) {
            line += QStringLiteral("  ·  %1 %2").arg(formatFrequency(s.frequencyHz), s.modulation);
        }
        if (s.transmitting) line += QStringLiteral("  ·  TX");
        radio = colored(kGreen, line);
        if (!s.receiverEnabled) {
            radio += QStringLiteral("<br>") + colored(kAmber, QStringLiteral("This receiver is switched off in ExpertSDR3, so it has no audio."));
        } else if (!s.audioFlowing) {
            radio += QStringLiteral("<br>") + colored(kAmber, QStringLiteral("Waiting for audio. Is ExpertSDR3 started?"));
        }
        break;
    }
    }
    m_radioLabel->setText(radio);

    // The server knows how many receivers there are. Match it.
    if (s.trxCount > 0 && s.trxCount != m_receiver->count()) {
        const QSignalBlocker block(m_receiver);
        m_receiver->clear();
        for (int i = 0; i < s.trxCount; ++i) m_receiver->addItem(QStringLiteral("RX %1").arg(i + 1), i);
        m_receiver->setCurrentIndex(std::max(0, m_receiver->findData(s.receiver)));
    }

    // Mute status: say what is true now, and whether anything needs doing.
    QString mute;
    switch (s.mutePhase) {
    case MuteController::Phase::Holding:
        mute = colored(kGreen, QStringLiteral("ExpertSDR3 muted"));
        break;
    case MuteController::Phase::Engaging:
        mute = colored(kAmber, QStringLiteral("Muting ExpertSDR3..."));
        break;
    case MuteController::Phase::Restoring:
        mute = colored(kAmber, QStringLiteral("Restoring ExpertSDR3..."));
        break;
    case MuteController::Phase::OperatorOverride:
        mute = colored(kAmber, QStringLiteral("You unmuted ExpertSDR3; leaving it alone"));
        break;
    case MuteController::Phase::Failed:
        mute = colored(kRed, QStringLiteral("ExpertSDR3 did not respond to mute"));
        break;
    case MuteController::Phase::Idle:
        break;
    }
    m_muteStatus->setText(mute);

    // Output.
    QString output;
    if (!s.outputError.isEmpty() && !s.outputOpen) {
        output = colored(kRed, s.outputError);
    } else if (s.outputOpen) {
        output = colored(kMuted, QStringLiteral("%1 ms buffered, %2 Hz")
                                     .arg(s.sink.bufferedMs, 0, 'f', 0)
                                     .arg(s.sink.deviceRate));
    }
    m_outputStatus->setText(output);

    // NR.
    QString nr;
    if (s.dspLoad > 0.85) {
        // Past this the stream falls behind and audio breaks up. A Raspberry Pi can get
        // here on the neural methods, so say what to do about it.
        nr = colored(kRed, QStringLiteral("This computer cannot keep up with this method. Try NR2, which uses far less CPU."));
    } else if (s.nrFailed) {
        nr = colored(kRed, QStringLiteral("Could not start this method. Audio is passing through unprocessed."));
    } else if (s.nrPreparing) {
        nr = colored(kAmber, QStringLiteral("Preparing..."));
    } else if (s.nrBypassedByMode) {
        nr = colored(kAmber, QStringLiteral("Bypassed: %1 is not a speech mode").arg(s.modulation));
    } else if (!s.nrMethod.isEmpty() && s.audioFlowing) {
        nr = colored(kGreen, QStringLiteral("Active, removing %1 dB").arg(-s.nrGainDb, 0, 'f', 1));
    }
    m_nrStatus->setText(nr);

    // Meters. The output meter shows what actually reaches the device, after volume.
    const float outputDb = s.sink.peak > 1e-6f ? 20.0f * std::log10(s.sink.peak) : -120.0f;
    m_inputMeter->setValue(int(std::clamp(s.inputPeakDb, -60.0f, 0.0f)));
    m_outputMeter->setValue(int(std::clamp(outputDb, -60.0f, 0.0f)));
    m_inputValue->setText(s.inputPeakDb > -100.0f ? QStringLiteral("%1 dB").arg(s.inputPeakDb, 0, 'f', 0) : QStringLiteral("-- dB"));
    m_outputValue->setText(outputDb > -100.0f ? QStringLiteral("%1 dB").arg(outputDb, 0, 'f', 0) : QStringLiteral("-- dB"));

    m_stats->setText(colored(kMuted, QStringLiteral("DSP %1%   ·   clock correction %2 ppm   ·   underruns %3   ·   lost blocks %4")
                                         .arg(s.dspLoad * 100.0, 0, 'f', 1)
                                         .arg(s.sink.driftPpm, 0, 'f', 0)
                                         .arg(s.sink.underruns)
                                         .arg(s.droppedBlocks)));
}

} // namespace esa
