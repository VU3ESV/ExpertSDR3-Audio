#include "gui/pages/RadioPage.h"

#include "gui/shell/StatusChip.h"
#include "radio/CatTransport.h"
#include "radio/UsbModels.h"

#include "CanonIndicators.h"
#include "ClientEqOutputFader.h"
#include "ModemChrome.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace esa {

namespace {

// AetherSDR's canon chrome, as its own AetherNR page wears it, plus the spin box steppers
// and a combo arrow, which that sheet leaves to the caller.
QString radioSheet()
{
    namespace MC = AetherSDR::ModemChrome;
    return MC::styleSheet(MC::Scale::Dialog, MC::Look::Canon) + AetherSDR::canonSpinArrowRules() + QStringLiteral(R"(
QComboBox::drop-down { border: none; width: 22px; }
QComboBox::down-arrow {
    width: 0;
    height: 0;
    border-top: 5px solid {{color.canon.inkSoft}};
    border-left: 4px solid {{color.canon.control}};
    border-right: 4px solid {{color.canon.control}};
    border-bottom: 0px solid {{color.canon.control}};
}
)");
}

// A canon panel with its heading, as AetherSDR's pages group their controls.
QFrame* section(const QString& heading, QGridLayout** grid)
{
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("ControlsFrame"));
    *grid = new QGridLayout(frame);
    (*grid)->setContentsMargins(14, 10, 14, 12);
    (*grid)->setHorizontalSpacing(10);
    (*grid)->setVerticalSpacing(8);
    auto* label = new QLabel(heading);
    label->setObjectName(QStringLiteral("SectionLabel"));
    (*grid)->addWidget(label, 0, 0, 1, -1);
    return frame;
}

// The width a line edit needs to show `text` whole, in the font, padding and frame its
// style sheet gives it; so it is only right once the edit is in its window.
int lineEditWidthFor(const QLineEdit* edit, const QString& text)
{
    edit->ensurePolished();
    QStyleOptionFrame option;
    option.initFrom(edit);
    option.lineWidth = edit->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, edit);
    // QLineEdit keeps 2 px clear either side of its text, and the cursor needs one more.
    const QFontMetrics metrics = edit->fontMetrics();
    const QSize content(metrics.horizontalAdvance(text) + 5, metrics.height());
    return edit->style()->sizeFromContents(QStyle::CT_LineEdit, &option, content, edit).width();
}

} // namespace

RadioPage::RadioPage(PageContext& ctx)
    : Page(ctx)
{
    auto* content = new QWidget;
    auto* column = new QVBoxLayout(content);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(8);
    column->addWidget(buildKind());
    column->addWidget(m_tciBox = buildConnection());
    column->addWidget(m_usbBox = buildUsb());
    column->addWidget(buildLevel());
    column->addWidget(m_muteBox = buildMute());
    column->addWidget(m_afBox = buildAfMute());
    column->addStretch(1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scrollable(content));

    AetherSDR::applyCanonSheet(this, radioSheet);
    showKind();
}

QWidget* RadioPage::buildKind()
{
    QGridLayout* grid = nullptr;
    QFrame* box = section(QStringLiteral("RADIO"), &grid);
    m_kind = new QComboBox;
    m_kind->setObjectName(QStringLiteral("radioKind"));
    m_kind->setAccessibleName(QStringLiteral("Radio"));
    m_kind->addItem(QStringLiteral("ExpertSDR3 (TCI)"), QStringLiteral("tci"));
    for (const UsbModel& model : usb::models()) {
        const QString label = model.hasCat() ? QStringLiteral("%1 (USB)").arg(model.label())
                                             : QStringLiteral("%1 (USB, audio only)").arg(model.label());
        m_kind->addItem(label, QStringLiteral("usb:") + model.id);
    }
    const QString current = config().radioKind == QLatin1String("usb") ? QStringLiteral("usb:") + config().usbModel
                                                                        : QStringLiteral("tci");
    m_kind->setCurrentIndex(std::max(0, m_kind->findData(current)));
    m_kind->setToolTip(QStringLiteral("How this app hears the radio: from ExpertSDR3 over the network, or "
                                      "from a radio's own USB audio."));
    grid->addWidget(m_kind, 1, 0);
    grid->setColumnStretch(0, 1);
    connect(m_kind, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QString choice = m_kind->itemData(index).toString();
        if (choice.startsWith(QLatin1String("usb:"))) {
            config().radioKind = QStringLiteral("usb");
            config().usbModel = choice.mid(4);
        } else {
            config().radioKind = QStringLiteral("tci");
        }
        showKind();
        // The output list leaves out a USB radio's own codec, so it is drawn again.
        context().post([](Engine* engine) { engine->refreshDevices(); });
    });
    return box;
}

QWidget* RadioPage::buildUsb()
{
    QGridLayout* grid = nullptr;
    QFrame* box = section(QStringLiteral("USB RADIO"), &grid);
    m_usbInput = new QComboBox;
    m_usbInput->setObjectName(QStringLiteral("usbInput"));
    m_usbInput->setAccessibleName(QStringLiteral("Radio's audio input"));
    m_usbInput->setToolTip(QStringLiteral("The input this computer shows the radio's USB audio as."));
    m_usbRefresh = new QPushButton(QStringLiteral("Refresh"));
    m_usbRefresh->setObjectName(QStringLiteral("usbRefresh"));
    m_usbRefresh->setToolTip(QStringLiteral("Look again for audio devices, after plugging in or switching on the radio."));
    m_usbReceiver = new QComboBox;
    m_usbReceiver->setObjectName(QStringLiteral("usbReceiver"));
    m_usbReceiver->setAccessibleName(QStringLiteral("Receiver"));
    m_usbTrim = new QDoubleSpinBox;
    m_usbTrim->setObjectName(QStringLiteral("usbTrim"));
    m_usbTrim->setAccessibleName(QStringLiteral("Input trim"));
    m_usbTrim->setRange(kMinUsbTrimDb, kMaxUsbTrimDb);
    m_usbTrim->setSingleStep(0.5);
    m_usbTrim->setDecimals(1);
    m_usbTrim->setSuffix(QStringLiteral(" dB"));
    m_usbTrim->setValue(config().usbTrimDb);
    m_usbTrim->setToolTip(QStringLiteral("Raise or lower the radio's audio before processing. FROM RADIO shows the result."));
    m_usbConnect = new QPushButton(QStringLiteral("Connect"));
    m_usbConnect->setObjectName(QStringLiteral("usbConnect"));
    m_usbConnect->setCheckable(true);
    m_usbConnect->setMinimumWidth(130);
    m_usbSetup = hintLabel();
    m_usbSetup->setWordWrap(true);

    // CAT, for a model that has it: frequency and mode read from the radio. Ports are listed
    // only; nothing is opened until Test or Connect, and then only the one chosen.
    m_catPort = new QComboBox;
    m_catPort->setObjectName(QStringLiteral("catPort"));
    m_catPort->setAccessibleName(QStringLiteral("CAT port"));
    m_catPort->setToolTip(QStringLiteral("The radio's USB serial port, for reading its frequency and mode. "
                                         "None: audio only, and nothing is ever sent to the radio."));
    m_catBaud = new QComboBox;
    m_catBaud->setObjectName(QStringLiteral("catBaud"));
    m_catBaud->setAccessibleName(QStringLiteral("CAT baud rate"));
    for (const int baud : kCatBauds) m_catBaud->addItem(QString::number(baud), baud);
    m_catBaud->setCurrentIndex(std::max(0, m_catBaud->findData(config().usbCatBaud)));
    m_catBaud->setToolTip(QStringLiteral("As the radio's USB baud rate is set (TS-990S menu 0-07-01)."));
    m_catTest = new QPushButton(QStringLiteral("Test"));
    m_catTest->setObjectName(QStringLiteral("catTest"));
    m_catTest->setToolTip(QStringLiteral("Asks the radio who it is (ID;), and nothing else."));
    m_catLabel = hintLabel();
    m_catLabel->setObjectName(QStringLiteral("catStatus"));
    m_catLabel->setWordWrap(true);
    auto* catLabel = new QLabel(QStringLiteral("CAT"));
    auto* baudLabel = new QLabel(QStringLiteral("Baud"));
    m_catWidgets = {catLabel, m_catPort, baudLabel, m_catBaud, m_catTest, m_catLabel};
    m_usbLabel = hintLabel(colored(QStringLiteral("color.text.label"), QStringLiteral("Not connected")));

    grid->addWidget(new QLabel(QStringLiteral("Input")), 1, 0);
    grid->addWidget(m_usbInput, 1, 1, 1, 2);
    grid->addWidget(m_usbRefresh, 1, 3);
    grid->addWidget(new QLabel(QStringLiteral("Receiver")), 2, 0);
    grid->addWidget(m_usbReceiver, 2, 1);
    grid->addWidget(new QLabel(QStringLiteral("Trim")), 2, 2);
    grid->addWidget(m_usbTrim, 2, 3);
    grid->addWidget(catLabel, 3, 0);
    grid->addWidget(m_catPort, 3, 1, 1, 2);
    grid->addWidget(m_catTest, 3, 3);
    grid->addWidget(baudLabel, 4, 0);
    grid->addWidget(m_catBaud, 4, 1);
    grid->addWidget(m_catLabel, 5, 0, 1, 4);
    grid->addWidget(m_usbSetup, 6, 0, 1, 4);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_usbConnect);
    grid->addLayout(buttons, 7, 0, 1, 4);
    grid->addWidget(m_usbLabel, 8, 0, 1, 4);
    grid->setColumnStretch(1, 1);

    connect(m_usbConnect, &QPushButton::toggled, this, &RadioPage::onConnectToggled);
    connect(m_usbRefresh, &QPushButton::clicked, this, [this] {
        fillCatPorts();
        context().post([](Engine* engine) { engine->refreshDevices(); });
    });
    connect(m_catPort, &QComboBox::currentIndexChanged, this, [this](int index) {
        config().usbCatPort = m_catPort->itemData(index).toString();
        showCatChoice();
        showAfNote();
    });
    connect(m_catBaud, &QComboBox::currentIndexChanged, this,
            [this](int index) { config().usbCatBaud = m_catBaud->itemData(index).toInt(); });
    connect(m_catTest, &QPushButton::clicked, this, [this] {
        const QString port = config().usbCatPort;
        const int baud = config().usbCatBaud;
        if (port.isEmpty()) return;
        m_catTest->setEnabled(false);
        m_catLabel->setText(colored(QStringLiteral("color.text.label"), QStringLiteral("Asking the radio...")));
        context().post([port, baud](Engine* engine) { engine->testCat(port, baud); });
    });
    connect(m_usbInput, &QComboBox::currentIndexChanged, this, [this](int index) {
        config().usbInput = m_usbInput->itemData(index).toString();
        context().post([](Engine* engine) { engine->refreshDevices(); });  // the output list follows
    });
    connect(m_usbReceiver, &QComboBox::currentIndexChanged, this, [this](int index) {
        const int receiver = m_usbReceiver->itemData(index).toInt();
        config().receiver = receiver;
        context().post([receiver](Engine* engine) { engine->setReceiver(receiver); });
    });
    connect(m_usbTrim, &QDoubleSpinBox::valueChanged, this, [this](double db) {
        config().usbTrimDb = db;
        context().post([db](Engine* engine) { engine->setUsbTrimDb(db); });
    });
    return box;
}

bool RadioPage::usbRadio() const
{
    return config().radioKind == QLatin1String("usb");
}

void RadioPage::showKind()
{
    const bool usb = usbRadio();
    m_tciBox->setVisible(!usb);
    m_usbBox->setVisible(usb);
    // Only ExpertSDR3's own audio can be muted from here, so far.
    m_muteBox->setVisible(!usb);
    m_inputMeter->setToolTip(usb ? QStringLiteral("The level of the radio's USB audio, before any processing.")
                                 : QStringLiteral("The level of the audio arriving from ExpertSDR3, before any processing."));
    const UsbModel& model = usb::model(config().usbModel);
    const bool cat = usb && model.hasCat();
    for (QWidget* widget : std::as_const(m_catWidgets)) widget->setVisible(cat);
    if (!cat) config().usbCatPort.clear();  // a model with no CAT is never sent anything
    m_afBox->setVisible(cat && model.afMute);
    if (cat && model.afMute) {
        m_afMute->setText(QStringLiteral("Turn the %1's AF down while this app is playing").arg(model.name));
        showAfNote();
    }
    if (usb) {
        m_usbSetup->setText(colored(QStringLiteral("color.text.label"), usb::model(config().usbModel).setup));
        fillUsbReceivers();
        fillUsbInputs();
        if (cat) fillCatPorts();
    }
}

void RadioPage::fillUsbReceivers()
{
    const QSignalBlocker block(m_usbReceiver);
    m_usbReceiver->clear();
    const UsbModel& model = usb::model(config().usbModel);
    for (int i = 0; i < model.receivers.size(); ++i) m_usbReceiver->addItem(model.receivers.at(i), i);
    m_usbReceiver->setCurrentIndex(std::max(0, m_usbReceiver->findData(config().receiver)));
    if (m_usbReceiver->currentIndex() >= 0) config().receiver = m_usbReceiver->currentData().toInt();
}

void RadioPage::showCatChoice()
{
    // Test asks the chosen port only, so with none chosen there is nothing to test.
    const bool chosen = !config().usbCatPort.isEmpty();
    m_catTest->setEnabled(chosen && !m_usbConnect->isChecked());
    m_catLabel->setText(chosen ? QString()
                               : colored(QStringLiteral("color.text.label"),
                                         QStringLiteral("Choose the radio's CAT port to read its frequency and mode. "
                                                        "The likely one is marked ◂; nothing is chosen for you.")));
}

void RadioPage::fillCatPorts()
{
    const UsbModel& model = usb::model(config().usbModel);
    const QString selected = config().usbCatPort;
    const QSignalBlocker block(m_catPort);
    m_catPort->clear();
    m_catPort->addItem(QStringLiteral("None (audio only)"), QString());
    const auto ports = listSerialPorts();
    for (const QSerialPortInfo& port : ports) {
        QString label = port.portName();
        if (!port.description().isEmpty()) label += QStringLiteral("  ·  %1").arg(port.description());
        if (port.hasVendorIdentifier() && port.hasProductIdentifier()) {
            const QString id = QStringLiteral("%1:%2")
                                   .arg(port.vendorIdentifier(), 4, 16, QLatin1Char('0'))
                                   .arg(port.productIdentifier(), 4, 16, QLatin1Char('0'))
                                   .toUpper();
            label += QStringLiteral(" (%1)").arg(id);
            // A hint only: never chosen for the operator, since another device may share the bridge.
            if (!model.bridgeHint.isEmpty() && id == model.bridgeHint) label += QStringLiteral("  ◂ %1?").arg(model.name);
        }
        // A port cannot be shared, so one a logger holds is marked before anyone chooses it.
        if (const QStringList users = otherProgramsUsing(port.systemLocation()); !users.isEmpty()) {
            label += QStringLiteral("  (in use by %1)").arg(users.join(QStringLiteral(", ")));
        }
        m_catPort->addItem(label, port.portName());
    }
    const int index = m_catPort->findData(selected);
    if (index >= 0) {
        m_catPort->setCurrentIndex(index);
    } else {
        m_catPort->addItem(QStringLiteral("%1 (not found)").arg(selected), selected);
        m_catPort->setCurrentIndex(m_catPort->count() - 1);
    }
    showCatChoice();
}

void RadioPage::showCatTest(bool ok, const QString& text)
{
    m_catTest->setEnabled(!config().usbCatPort.isEmpty() && !m_usbConnect->isChecked());
    m_catLabel->setText(colored(ok ? QStringLiteral("color.accent.success") : QStringLiteral("color.accent.danger"), text));
}

void RadioPage::showInputDevices(const QList<AudioDeviceInfo>& devices)
{
    m_inputs = devices;
    // Kept for when a USB radio is chosen; until then nothing is suggested or changed.
    if (usbRadio()) fillUsbInputs();
}

void RadioPage::fillUsbInputs()
{
    const UsbModel& model = usb::model(config().usbModel);
    // Nothing chosen yet: offer the one input that looks like this model's codec, never more
    // than a suggestion of which input it is. It is only ever listened to.
    if (config().usbInput.isEmpty() && !model.codecHint.isEmpty()) {
        QString only;
        int matches = 0;
        for (const AudioDeviceInfo& device : std::as_const(m_inputs)) {
            if (device.name.contains(model.codecHint, Qt::CaseInsensitive)) {
                only = device.key;
                ++matches;
            }
        }
        if (matches == 1) config().usbInput = only;
    }
    const QString selected = config().usbInput;
    const QSignalBlocker block(m_usbInput);
    m_usbInput->clear();
    if (selected.isEmpty()) m_usbInput->addItem(QStringLiteral("Choose the radio's audio input"), QString());
    for (const AudioDeviceInfo& device : std::as_const(m_inputs)) {
        m_usbInput->addItem(QStringLiteral("%1  (%2)").arg(device.name, device.hostApi), device.key);
    }
    const int index = m_usbInput->findData(selected);
    if (index >= 0) {
        m_usbInput->setCurrentIndex(index);
    } else {
        // Not plugged in, or the radio is off: shown, so switching it on picks it up again.
        m_usbInput->addItem(QStringLiteral("%1 (not found)").arg(selected.section(QLatin1Char('|'), 1)), selected);
        m_usbInput->setCurrentIndex(m_usbInput->count() - 1);
    }
}

QWidget* RadioPage::buildConnection()
{
    QGridLayout* grid = nullptr;
    QFrame* box = section(QStringLiteral("EXPERTSDR3 (TCI)"), &grid);

    m_host = new QLineEdit(config().host);
    m_host->setObjectName(QStringLiteral("host"));
    m_host->setToolTip(QStringLiteral("Computer running ExpertSDR3. Enable TCI in its Options."));
    m_port = new QSpinBox;
    m_port->setRange(1, 65535);
    m_port->setValue(config().port);
    m_port->setToolTip(QStringLiteral("If nothing answers here, the usual TCI ports are tried automatically."));
    m_receiver = new QComboBox;
    for (int i = 0; i < 2; ++i) m_receiver->addItem(QStringLiteral("RX %1").arg(i + 1), i);
    m_receiver->setCurrentIndex(std::max(0, m_receiver->findData(config().receiver)));
    m_connect = new QPushButton(QStringLiteral("Connect"));
    m_connect->setObjectName(QStringLiteral("connect"));
    m_connect->setCheckable(true);
    m_connect->setMinimumWidth(130);
    m_findRadios = new QPushButton(QStringLiteral("Find Radios..."));
    m_findRadios->setToolTip(QStringLiteral("Look on your network for computers running ExpertSDR3 with TCI enabled."));
    connect(m_findRadios, &QPushButton::clicked, this, [this] { context().findRadios(); });
    m_radioLabel = hintLabel(colored(QStringLiteral("color.text.label"), QStringLiteral("Not connected")));

    grid->addWidget(new QLabel(QStringLiteral("Host")), 1, 0);
    grid->addWidget(m_host, 1, 1, 1, 3);
    grid->addWidget(new QLabel(QStringLiteral("Port")), 2, 0);
    grid->addWidget(m_port, 2, 1);
    grid->addWidget(new QLabel(QStringLiteral("Receiver")), 2, 2);
    grid->addWidget(m_receiver, 2, 3);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_findRadios);
    buttons->addStretch(1);
    buttons->addWidget(m_connect);
    grid->addLayout(buttons, 3, 0, 1, 4);
    grid->addWidget(m_radioLabel, 4, 0, 1, 4);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);

    connect(m_connect, &QPushButton::toggled, this, &RadioPage::onConnectToggled);
    connect(m_receiver, &QComboBox::currentIndexChanged, this, [this](int index) {
        const int receiver = m_receiver->itemData(index).toInt();
        config().receiver = receiver;
        context().post([receiver](Engine* engine) { engine->setReceiver(receiver); });
    });
    return box;
}

QWidget* RadioPage::buildLevel()
{
    QGridLayout* grid = nullptr;
    QFrame* box = section(QStringLiteral("FROM RADIO"), &grid);
    // A meter with no handle: the radio's level is set in ExpertSDR3, not here.
    m_inputMeter = new AetherSDR::ClientEqOutputFader;
    m_inputMeter->setObjectName(QStringLiteral("inputLevel"));
    m_inputMeter->setOrientation(Qt::Horizontal);
    m_inputMeter->setGainControlEnabled(false);
    m_inputMeter->setAccessibleName(QStringLiteral("From radio"));
    m_inputMeter->setToolTip(QStringLiteral("The level of the audio arriving from ExpertSDR3, before any processing."));
    // Its end cap names the output it was made for; this one measures the input.
    for (QLabel* cap : m_inputMeter->findChildren<QLabel*>()) {
        if (cap->text() == QLatin1String("OUT")) cap->setText(QStringLiteral("IN"));
    }
    grid->addWidget(m_inputMeter, 1, 0);

    // At AetherSDR's 25 Hz, and only while this page is on screen. Taking the peak resets
    // it, so each tick shows the loudest moment since the last one.
    m_levelTimer = new QTimer(this);
    m_levelTimer->setInterval(40);
    connect(m_levelTimer, &QTimer::timeout, this, [this] {
        const auto& taps = context().taps;
        m_inputMeter->setPeakLinear(taps ? taps->inputPeak.exchange(0.0f, std::memory_order_relaxed) : 0.0f);
    });
    return box;
}

QWidget* RadioPage::buildMute()
{
    QGridLayout* grid = nullptr;
    QFrame* box = section(QStringLiteral("EXPERTSDR3'S OWN AUDIO"), &grid);
    m_muteExpert = new QCheckBox(QStringLiteral("Mute ExpertSDR3's own audio while this app is playing"));
    m_muteExpert->setChecked(config().muteExpert);
    m_muteExpert->setToolTip(QStringLiteral(
        "ExpertSDR3 is restored to how it was found when you disconnect or quit."));
    m_muteStatus = hintLabel();
    m_muteStatus->hide();  // until there is something to say
    grid->addWidget(m_muteExpert, 1, 0);
    grid->addWidget(m_muteStatus, 2, 0);
    connect(m_muteExpert, &QCheckBox::toggled, this, [this](bool on) {
        config().muteExpert = on;
        context().post([on](Engine* engine) { engine->setMuteExpert(on); });
    });
    return box;
}

QWidget* RadioPage::buildAfMute()
{
    QGridLayout* grid = nullptr;
    QFrame* box = section(QStringLiteral("THE RADIO'S SPEAKER"), &grid);
    m_afMute = new QCheckBox;  // named for the radio in showKind()
    m_afMute->setObjectName(QStringLiteral("usbAfMute"));
    m_afMute->setChecked(config().usbAfMute);
    m_afMute->setToolTip(QStringLiteral(
        "The main receiver's AF only. It goes back to where it was when you disconnect or quit, and "
        "turning the AF knob yourself hands it back to you."));
    m_afNote = hintLabel();
    m_afNote->setWordWrap(true);
    m_afStatus = hintLabel();
    m_afStatus->setObjectName(QStringLiteral("usbAfStatus"));
    m_afStatus->hide();  // until there is something to say
    m_afRestore = new QPushButton(QStringLiteral("Restore radio volume now"));
    m_afRestore->setObjectName(QStringLiteral("usbAfRestore"));
    m_afRestore->setToolTip(QStringLiteral("Gives the radio its AF back now, and stops turning it down."));
    m_afRestore->hide();
    grid->addWidget(m_afMute, 1, 0, 1, 2);
    grid->addWidget(m_afNote, 2, 0, 1, 2);
    grid->addWidget(m_afStatus, 3, 0);
    grid->addWidget(m_afRestore, 3, 1, Qt::AlignRight);
    grid->setColumnStretch(0, 1);
    connect(m_afMute, &QCheckBox::toggled, this, [this](bool on) {
        config().usbAfMute = on;
        context().post([on](Engine* engine) { engine->setUsbAfMute(on); });
    });
    connect(m_afRestore, &QPushButton::clicked, m_afMute, [this] { m_afMute->setChecked(false); });
    return box;
}

void RadioPage::showAfNote()
{
    const UsbModel& model = usb::model(config().usbModel);
    QStringList lines;
    if (config().usbCatPort.isEmpty()) lines << QStringLiteral("Needs the CAT port above: the AF is set over it.");
    if (!model.afMuteVerified) {
        lines << QStringLiteral("Not yet bench-tested on a real %1: watch its AF the first time.").arg(model.name);
    }
    m_afNote->setText(colored(QStringLiteral("color.text.label"), lines.join(QStringLiteral("<br>"))));
    m_afNote->setVisible(!lines.isEmpty());
}

void RadioPage::showEvent(QShowEvent* event)
{
    Page::showEvent(event);
    // Room for a whole IPv4 address, so a narrow window cannot squeeze the field to a
    // fragment of one. Measured here, where the field has its final sheet and font.
    m_host->setMinimumWidth(lineEditWidthFor(m_host, QStringLiteral("255.255.255.255")));

    // Start from now, not from the loudest moment while the page was away.
    if (const auto& taps = context().taps) taps->inputPeak.exchange(0.0f, std::memory_order_relaxed);
    m_levelTimer->start();
}

void RadioPage::hideEvent(QHideEvent* event)
{
    m_levelTimer->stop();
    Page::hideEvent(event);
}

void RadioPage::connectNow()
{
    (usbRadio() ? m_usbConnect : m_connect)->setChecked(true);
}

void RadioPage::onConnectToggled(bool on)
{
    // One Connect for each kind of radio, and only the chosen kind's shows: they move together.
    for (QPushButton* button : {m_connect, m_usbConnect}) {
        if (button->isChecked() == on) continue;
        const QSignalBlocker block(button);
        button->setChecked(on);
    }
    for (QPushButton* button : {m_connect, m_usbConnect}) {
        button->setText(on ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
    }
    m_kind->setEnabled(!on);
    m_usbInput->setEnabled(!on);
    m_catPort->setEnabled(!on);
    m_catBaud->setEnabled(!on);
    m_catTest->setEnabled(!on && !config().usbCatPort.isEmpty());  // the session has the port
    if (on) {
        if (!usbRadio()) {
            config().host = m_host->text().trimmed();
            config().port = m_port->value();
        }
        const AppConfig snapshot = config();
        context().post([snapshot](Engine* engine) {
            engine->configure(snapshot);
            engine->connectRadio();
        });
    } else {
        context().post([](Engine* engine) { engine->disconnectRadio(); });
    }
    m_host->setEnabled(!on);
    m_port->setEnabled(!on);
}

void RadioPage::connectTo(const DiscoveredRadio& radio)
{
    if (m_connect->isChecked()) m_connect->setChecked(false);
    // A radio found on the network is a TCI one.
    if (usbRadio()) m_kind->setCurrentIndex(m_kind->findData(QStringLiteral("tci")));
    m_host->setText(radio.host);
    {
        const QSignalBlocker block(m_port);
        m_port->setValue(radio.port);
    }
    // This is the radio the operator picked. If it goes away, wait for it rather than wandering
    // off to whatever else answers on the usual ports.
    config().fallbackPorts.clear();
    m_connect->setChecked(true);
}

void RadioPage::showConnectedPort(int port)
{
    const QSignalBlocker block(m_port);
    m_port->setValue(port);
    config().port = port;
}

int RadioPage::port() const
{
    return m_port->value();
}

void RadioPage::showStatus(const EngineStatus& s)
{
    const QString radio = statusText::radio(s);
    QLabel* label = s.radioKind == QLatin1String("usb") ? m_usbLabel : m_radioLabel;
    if (radio != label->text()) label->setText(radio);
    if (s.radioKind == QLatin1String("usb") && s.catUsed && s.connection != RadioBackend::Link::Disconnected) {
        const QString cat = s.catReady
                                ? colored(QStringLiteral("color.accent.success"), QStringLiteral("CAT: reading the radio on %1").arg(s.catPort))
                                : colored(QStringLiteral("color.accent.warning"),
                                          QStringLiteral("CAT: %1").arg(s.catError.isEmpty() ? QStringLiteral("connecting...") : s.catError));
        if (cat != m_catLabel->text()) m_catLabel->setText(cat);
    }

    // The server knows how many receivers there are. Match it.
    if (s.radioKind == QLatin1String("tci") && s.trxCount > 0 && s.trxCount != m_receiver->count()) {
        const QSignalBlocker block(m_receiver);
        m_receiver->clear();
        for (int i = 0; i < s.trxCount; ++i) m_receiver->addItem(QStringLiteral("RX %1").arg(i + 1), i);
        m_receiver->setCurrentIndex(std::max(0, m_receiver->findData(s.receiver)));
    }

    const bool usb = s.radioKind == QLatin1String("usb");
    QLabel* muteLabel = usb ? m_afStatus : m_muteStatus;
    const QString mute = statusText::mute(s);
    if (mute != muteLabel->text()) muteLabel->setText(mute);
    muteLabel->setVisible(!mute.isEmpty());
    // While the radio's AF is down on this app's account, it can be given back at once.
    const bool down = s.mutePhase == MuteController::Phase::Holding || s.mutePhase == MuteController::Phase::Engaging;
    m_afRestore->setVisible(usb && down && s.mutePriorLevel > 0 && m_afMute->isChecked());
}

} // namespace esa
