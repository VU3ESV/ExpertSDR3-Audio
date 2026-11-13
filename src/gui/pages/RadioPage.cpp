#include "gui/pages/RadioPage.h"

#include "gui/shell/StatusChip.h"

#include "CanonIndicators.h"
#include "ClientEqOutputFader.h"
#include "ModemChrome.h"

#include <QCheckBox>
#include <QComboBox>
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
    column->addWidget(buildConnection());
    column->addWidget(buildLevel());
    column->addWidget(buildMute());
    column->addStretch(1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scrollable(content));

    AetherSDR::applyCanonSheet(this, radioSheet);
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
    m_connect->setChecked(true);
}

void RadioPage::onConnectToggled(bool on)
{
    if (on) {
        config().host = m_host->text().trimmed();
        config().port = m_port->value();
        const AppConfig snapshot = config();
        context().post([snapshot](Engine* engine) {
            engine->configure(snapshot);
            engine->connectRadio();
        });
    } else {
        context().post([](Engine* engine) { engine->disconnectRadio(); });
    }
    m_connect->setText(on ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
    m_host->setEnabled(!on);
    m_port->setEnabled(!on);
}

void RadioPage::connectTo(const DiscoveredRadio& radio)
{
    if (m_connect->isChecked()) m_connect->setChecked(false);
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
    if (radio != m_radioLabel->text()) m_radioLabel->setText(radio);

    // The server knows how many receivers there are. Match it.
    if (s.trxCount > 0 && s.trxCount != m_receiver->count()) {
        const QSignalBlocker block(m_receiver);
        m_receiver->clear();
        for (int i = 0; i < s.trxCount; ++i) m_receiver->addItem(QStringLiteral("RX %1").arg(i + 1), i);
        m_receiver->setCurrentIndex(std::max(0, m_receiver->findData(s.receiver)));
    }

    const QString mute = statusText::mute(s);
    if (mute != m_muteStatus->text()) m_muteStatus->setText(mute);
    m_muteStatus->setVisible(!mute.isEmpty());
}

} // namespace esa
