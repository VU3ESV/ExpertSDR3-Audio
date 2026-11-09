#include "gui/pages/StationBand.h"

#include "gui/pages/Pages.h"
#include "gui/shell/StatusChip.h"

#include "CanonIndicators.h"
#include "ModemChrome.h"

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

namespace esa {

namespace {

const QString kLabel = QStringLiteral("color.text.label");

// AetherSDR's canon chrome, as the Radio page wears it, with the spin box steppers and the
// combo arrow that sheet leaves to the caller. On this band alone: the panels above and
// below it keep their own sheets.
QString bandSheet()
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

} // namespace

StationBand::StationBand(PageContext& ctx, QWidget* parent)
    : QWidget(parent), m_context(ctx)
{
    setObjectName(QStringLiteral("stationBand"));
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("ControlsFrame"));
    auto* grid = new QGridLayout(frame);
    grid->setContentsMargins(14, 10, 14, 12);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    auto* heading = new QLabel(QStringLiteral("AUDIO OUTPUT"));
    heading->setObjectName(QStringLiteral("SectionLabel"));

    AppConfig& config = *m_context.config;
    m_device = new QComboBox;
    m_device->setObjectName(QStringLiteral("outputDevice"));
    m_device->setAccessibleName(QStringLiteral("Output device"));
    m_device->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_refresh = new QPushButton(QStringLiteral("Refresh"));
    m_refresh->setObjectName(QStringLiteral("outputRefresh"));
    m_refresh->setToolTip(QStringLiteral("Look again for audio devices, e.g. after plugging one in."));

    m_buffer = new QSpinBox;
    m_buffer->setObjectName(QStringLiteral("outputBuffer"));
    m_buffer->setAccessibleName(QStringLiteral("Buffer"));
    m_buffer->setRange(10, 500);
    m_buffer->setSuffix(QStringLiteral(" ms"));
    m_buffer->setValue(config.bufferMs);
    m_buffer->setToolTip(QStringLiteral("Lower is less delay; raise it if you hear dropouts. Grows by itself after an underrun."));
    m_buffered = new QLabel;
    m_buffered->setObjectName(QStringLiteral("outputBuffered"));
    m_buffered->setTextFormat(Qt::RichText);

    // Full width, so a long error wraps here instead of pushing the controls above about.
    m_status = hintLabel();
    m_status->setObjectName(QStringLiteral("outputStatus"));
    m_status->hide();  // until there is something to say
    m_devicesNote = hintLabel(colored(QStringLiteral("color.accent.danger"), QStringLiteral("No audio output devices found")));
    m_devicesNote->setObjectName(QStringLiteral("outputDevicesNote"));
    m_devicesNote->hide();
    m_stats = hintLabel();
    m_stats->setObjectName(QStringLiteral("outputStats"));

    grid->addWidget(heading, 0, 0, 1, -1);
    grid->addWidget(new QLabel(QStringLiteral("Device")), 1, 0);
    grid->addWidget(m_device, 1, 1, 1, 2);
    grid->addWidget(m_refresh, 1, 3);
    grid->addWidget(new QLabel(QStringLiteral("Buffer")), 2, 0);
    grid->addWidget(m_buffer, 2, 1);
    grid->addWidget(m_buffered, 2, 2, 1, 2);
    grid->addWidget(m_status, 3, 0, 1, -1);
    grid->addWidget(m_devicesNote, 4, 0, 1, -1);
    grid->addWidget(m_stats, 5, 0, 1, -1);
    grid->setColumnStretch(2, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(frame);
    AetherSDR::applyCanonSheet(this, bandSheet);

    connect(m_device, &QComboBox::currentIndexChanged, this, &StationBand::onDeviceSelected);
    connect(m_refresh, &QPushButton::clicked, this, [this] {
        m_context.post([](Engine* engine) { engine->refreshDevices(); });
    });
    connect(m_buffer, &QSpinBox::valueChanged, this, [this](int ms) {
        m_context.config->bufferMs = ms;
        m_context.post([ms](Engine* engine) { engine->setBufferMs(ms); });
    });
}

void StationBand::showDevices(const QList<AudioDeviceInfo>& devices)
{
    const QSignalBlocker block(m_device);
    const QString selected = m_context.config->deviceKey;
    m_device->clear();
    m_device->addItem(QStringLiteral("System default"), QString());
    for (const AudioDeviceInfo& device : devices) {
        QString label = QStringLiteral("%1  (%2)").arg(device.name, device.hostApi);
        if (device.isDefault) label += QStringLiteral("  *");
        m_device->addItem(label, device.key);
    }
    m_devicesNote->setVisible(devices.isEmpty());
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

void StationBand::onDeviceSelected(int index)
{
    const QString key = m_device->itemData(index).toString();
    m_context.config->deviceKey = key;
    m_context.post([key](Engine* engine) { engine->setOutputDevice(key); });
}

void StationBand::showStatus(const EngineStatus& s)
{
    // While a device is open: what it holds and the rate it runs at, beside the buffer that
    // sets it. Otherwise why none is open, if something went wrong, on a line of its own.
    const QString output = statusText::output(s);
    const QString buffered = s.outputOpen ? output : QString();
    const QString status = s.outputOpen ? QString() : output;
    if (buffered != m_buffered->text()) m_buffered->setText(buffered);
    if (status != m_status->text()) m_status->setText(status);
    m_status->setVisible(!status.isEmpty());

    // Wrapped, when it must be, between figures and never inside one.
    const QStringList figures = {
        QStringLiteral("DSP %1%").arg(s.dspLoad * 100.0, 0, 'f', 1),
        QStringLiteral("clock correction %1 ppm").arg(s.sink.driftPpm, 0, 'f', 0),
        QStringLiteral("underruns %1").arg(s.sink.underruns),
        QStringLiteral("lost blocks %1").arg(s.droppedBlocks),
        QStringLiteral("chain %1 Hz").arg(s.chainRate),
    };
    QString line;
    for (const QString& figure : figures) {
        if (!line.isEmpty()) line += QStringLiteral(" · ");
        line += QString(figure).replace(QLatin1Char(' '), QChar(0x00A0));
    }
    const QString stats = colored(kLabel, line);
    if (stats != m_stats->text()) m_stats->setText(stats);
}

} // namespace esa
