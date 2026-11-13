#pragma once

#include "app/Engine.h"
#include "audio/AudioSink.h"

#include <QList>
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace esa {

struct PageContext;

// This computer's side of Final Output, in the band AetherRX leaves empty between its output
// meter and its waveform: which device plays, how much it buffers and what it reports, and
// how the engine is keeping up. None of it is part of a profile, which can move to another
// computer; all of it is saved with this computer's settings.
class StationBand : public QWidget {
    Q_OBJECT

public:
    explicit StationBand(PageContext& context, QWidget* parent = nullptr);

    void showStatus(const EngineStatus& s);
    // The devices the engine found: "System default" first, the system's own marked "*",
    // and a saved device that is not plugged in kept as "(not found)", so plugging it back
    // in restores it. The null device that discards audio is never listed; chosen, it is
    // named for what it is.
    void showDevices(const QList<AudioDeviceInfo>& devices);

    QComboBox* deviceList() const { return m_device; }
    QPushButton* refreshButton() const { return m_refresh; }
    QSpinBox* bufferBox() const { return m_buffer; }

private:
    void onDeviceSelected(int index);

    PageContext& m_context;
    QComboBox* m_device{nullptr};
    QPushButton* m_refresh{nullptr};
    QSpinBox* m_buffer{nullptr};
    QLabel* m_buffered{nullptr};
    QLabel* m_status{nullptr};
    QLabel* m_devicesNote{nullptr};
    QLabel* m_stats{nullptr};
};

} // namespace esa
