#pragma once

#include "gui/pages/Pages.h"
#include "tci/TciDiscovery.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace AetherSDR {
class ClientEqOutputFader;
}

namespace esa {

// The first step: which ExpertSDR3 to listen to. Today's connection box, moved here
// unchanged in what it does, with the level arriving from the radio and the Mute
// ExpertSDR3 choice beside it. Drawn in AetherSDR's canon chrome (ModemChrome).
class RadioPage : public Page {
    Q_OBJECT

public:
    explicit RadioPage(PageContext& context);

    void showStatus(const EngineStatus& s) override;

    // As if the operator had pressed Connect.
    void connectNow();
    // Leaves any current radio first, restoring its mute.
    void connectTo(const DiscoveredRadio& radio);
    // Port auto-detection found the server on another port: keep it.
    void showConnectedPort(int port);
    int port() const;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QWidget* buildConnection();
    QWidget* buildLevel();
    QWidget* buildMute();
    void onConnectToggled(bool on);

    QLineEdit* m_host{nullptr};
    QSpinBox* m_port{nullptr};
    QComboBox* m_receiver{nullptr};
    QPushButton* m_connect{nullptr};
    QPushButton* m_findRadios{nullptr};
    QLabel* m_radioLabel{nullptr};
    // AetherSDR's level strip, as a meter only, fed from LiveTaps::inputPeak.
    AetherSDR::ClientEqOutputFader* m_inputMeter{nullptr};
    QTimer* m_levelTimer{nullptr};
    QCheckBox* m_muteExpert{nullptr};
    QLabel* m_muteStatus{nullptr};
};

} // namespace esa
