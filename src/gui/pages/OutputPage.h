#pragma once

#include "audio/AudioSink.h"
#include "gui/pages/Pages.h"

#include <QList>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;

namespace esa {

// Final Output. Interim: today's audio output controls, meter and stats, until AetherSDR's
// output panel and waveform take the page over.
class OutputPage : public Page {
    Q_OBJECT

public:
    explicit OutputPage(PageContext& context);

    void showStatus(const EngineStatus& s) override;
    void showDevices(const QList<AudioDeviceInfo>& devices);

private:
    QWidget* buildOutputBox();
    QWidget* buildMeter();
    void onDeviceSelected(int index);

    QComboBox* m_device{nullptr};
    QSpinBox* m_buffer{nullptr};
    QSlider* m_gain{nullptr};
    QLabel* m_gainLabel{nullptr};
    QCheckBox* m_boost{nullptr};
    QPushButton* m_muteOutput{nullptr};
    QLabel* m_outputStatus{nullptr};
    QLabel* m_devicesNote{nullptr};
    QProgressBar* m_outputMeter{nullptr};
    QLabel* m_outputValue{nullptr};
    QLabel* m_stats{nullptr};
};

} // namespace esa
