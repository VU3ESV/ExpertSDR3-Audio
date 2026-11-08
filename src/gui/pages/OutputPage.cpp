#include "gui/pages/OutputPage.h"

#include "gui/shell/StatusChip.h"

#include "core/ThemeManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace esa {

namespace {
const QString kLabel = QStringLiteral("color.text.label");
}

OutputPage::OutputPage(PageContext& ctx)
    : Page(ctx)
{
    auto* content = new QWidget;
    auto* column = new QVBoxLayout(content);
    column->addWidget(buildOutputBox());
    column->addWidget(buildMeter());
    column->addStretch(1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scrollable(content));
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, interimPageSheet());
}

QWidget* OutputPage::buildOutputBox()
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
    m_buffer->setValue(config().bufferMs);
    m_buffer->setToolTip(QStringLiteral("Lower is less delay; raise it if you hear dropouts. Grows by itself after an underrun."));

    m_gain = new QSlider(Qt::Horizontal);
    m_gain->setObjectName(QStringLiteral("volume"));
    m_gain->setAccessibleName(QStringLiteral("Volume"));
    m_gain->setRange(-60, 30);
    m_gain->setValue(int(config().outputGainDb));
    m_gainLabel = new QLabel;
    m_gainLabel->setMinimumWidth(52);
    m_gainLabel->setText(QStringLiteral("%1 dB").arg(m_gain->value()));

    m_boost = new QCheckBox(QStringLiteral("Soft boost"));
    m_boost->setChecked(config().boost);
    m_boost->setToolTip(QStringLiteral("About 2x louder on quiet signals, never past full scale."));
    m_muteOutput = new QPushButton(QStringLiteral("Mute"));
    m_muteOutput->setCheckable(true);

    // Full width, so a long error wraps here instead of pushing the controls above about.
    m_outputStatus = hintLabel();
    m_outputStatus->hide();  // until there is something to say
    m_devicesNote = hintLabel(colored(QStringLiteral("color.accent.danger"), QStringLiteral("No audio output devices found")));
    m_devicesNote->hide();

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
    grid->addWidget(m_outputStatus, 3, 0, 1, 5);
    grid->addWidget(m_devicesNote, 4, 0, 1, 5);
    grid->setColumnStretch(1, 1);

    connect(m_device, &QComboBox::currentIndexChanged, this, &OutputPage::onDeviceSelected);
    connect(refresh, &QPushButton::clicked, this, [this] {
        context().post([](Engine* engine) { engine->refreshDevices(); });
    });
    connect(m_buffer, &QSpinBox::valueChanged, this, [this](int ms) {
        config().bufferMs = ms;
        context().post([ms](Engine* engine) { engine->setBufferMs(ms); });
    });
    connect(m_gain, &QSlider::valueChanged, this, [this](int db) {
        m_gainLabel->setText(QStringLiteral("%1 dB").arg(db));
        config().outputGainDb = db;
        context().post([db](Engine* engine) { engine->setOutputGainDb(db); });
    });
    connect(m_boost, &QCheckBox::toggled, this, [this](bool on) {
        config().boost = on;
        context().post([on](Engine* engine) { engine->setBoost(on); });
    });
    connect(m_muteOutput, &QPushButton::toggled, this, [this](bool on) {
        context().post([on](Engine* engine) { engine->setOutputMuted(on); });
    });
    return box;
}

QWidget* OutputPage::buildMeter()
{
    auto* box = new QFrame;
    auto* grid = new QGridLayout(box);
    grid->setContentsMargins(4, 0, 4, 0);

    m_outputMeter = new QProgressBar;
    m_outputMeter->setRange(-60, 0);
    m_outputMeter->setValue(-60);
    m_outputMeter->setTextVisible(false);
    m_outputMeter->setAccessibleName(QStringLiteral("To speakers"));
    m_outputValue = new QLabel(QStringLiteral("-- dB"));
    m_outputValue->setMinimumWidth(60);
    m_stats = hintLabel();

    grid->addWidget(new QLabel(QStringLiteral("To speakers")), 0, 0);
    grid->addWidget(m_outputMeter, 0, 1);
    grid->addWidget(m_outputValue, 0, 2);
    grid->addWidget(m_stats, 1, 0, 1, 3);
    grid->setColumnStretch(1, 1);
    return box;
}

void OutputPage::showDevices(const QList<AudioDeviceInfo>& devices)
{
    const QSignalBlocker block(m_device);
    const QString selected = config().deviceKey;
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

void OutputPage::onDeviceSelected(int index)
{
    const QString key = m_device->itemData(index).toString();
    config().deviceKey = key;
    context().post([key](Engine* engine) { engine->setOutputDevice(key); });
}

void OutputPage::showStatus(const EngineStatus& s)
{
    const QString output = statusText::output(s);
    if (output != m_outputStatus->text()) m_outputStatus->setText(output);
    m_outputStatus->setVisible(!output.isEmpty());

    // The output meter shows what actually reaches the device, after volume.
    const float outputDb = s.sink.peak > 1e-6f ? 20.0f * std::log10(s.sink.peak) : -120.0f;
    m_outputMeter->setValue(int(std::clamp(outputDb, -60.0f, 0.0f)));
    m_outputValue->setText(outputDb > -100.0f ? QStringLiteral("%1 dB").arg(outputDb, 0, 'f', 0)
                                              : QStringLiteral("-- dB"));

    m_stats->setText(colored(kLabel, QStringLiteral("DSP %1%   ·   clock correction %2 ppm   ·   underruns %3   ·   lost blocks %4")
                                         .arg(s.dspLoad * 100.0, 0, 'f', 1)
                                         .arg(s.sink.driftPpm, 0, 'f', 0)
                                         .arg(s.sink.underruns)
                                         .arg(s.droppedBlocks)));
}

} // namespace esa
