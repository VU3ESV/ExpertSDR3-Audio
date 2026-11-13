#pragma once

#include "audio/AudioSink.h"
#include "gui/pages/Pages.h"

#include <QList>

class QLabel;

namespace AetherSDR {
class ClientCompKnob;
class StripRxOutputPanel;
class StripWaveformPanel;
class WaveformWidget;
}

namespace esa {

class OutputFeed;
class StationBand;

// Final Output, as AetherRX's last tab: upstream's output panel (its meter, MUTE, BOOST,
// and its knob made our Volume), this computer's output device and figures, and upstream's
// waveform, one above the other. The meter and the scope show what leaves this app, after
// Volume, BOOST and MUTE (OutputFeed).
class OutputPage : public Page {
    Q_OBJECT

public:
    explicit OutputPage(PageContext& context);

    void showStatus(const EngineStatus& s) override;
    void showChain() override;
    void attach() override;
    void showDevices(const QList<AudioDeviceInfo>& devices);

    AetherSDR::StripRxOutputPanel* outputPanel() const { return m_output; }
    AetherSDR::StripWaveformPanel* waveformPanel() const { return m_waveform; }
    // The output panel's one knob, which upstream makes a ±12 dB Trim: here the Volume.
    AetherSDR::ClientCompKnob* volumeKnob() const { return m_volume; }
    StationBand* stationBand() const { return m_band; }
    OutputFeed* feed() const { return m_feed; }
    // Over the scope while it is fed silence, saying why nothing reaches the output.
    QLabel* scopeNote() const { return m_scopeNote; }

    // The Volume knob's travel: the lower half covers -60 to 0 dB and the upper half 0 to
    // +30 dB, so 0 dB sits at 12 o'clock, where upstream's Trim has it.
    static float volumeFromNorm(float norm);
    static float normFromVolume(float db);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void makeVolume();
    void showScopeNote();
    void placeScopeNote();

    AetherSDR::StripRxOutputPanel* m_output{nullptr};
    AetherSDR::StripWaveformPanel* m_waveform{nullptr};
    AetherSDR::WaveformWidget* m_scope{nullptr};
    AetherSDR::ClientCompKnob* m_volume{nullptr};
    StationBand* m_band{nullptr};
    OutputFeed* m_feed{nullptr};
    QLabel* m_scopeNote{nullptr};
    bool m_radioConnected{false};
};

} // namespace esa
