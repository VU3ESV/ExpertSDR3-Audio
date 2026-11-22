#pragma once

#include "gui/pages/Pages.h"
#include "tci/TciDiscovery.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace AetherSDR {
class ClientEqOutputFader;
}

namespace esa {

// The first step: which radio to listen to. ExpertSDR3 over TCI, with its connection box and
// the Mute ExpertSDR3 choice; or a radio with its own USB audio codec, such as a Kenwood
// TS-990S, with the input it appears as. The level arriving from the radio is shown for both.
// Drawn in AetherSDR's canon chrome (ModemChrome).
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
    // This computer's input devices, where a USB radio's codec is chosen.
    void showInputDevices(const QList<AudioDeviceInfo>& devices);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QWidget* buildKind();
    QWidget* buildConnection();
    QWidget* buildUsb();
    QWidget* buildLevel();
    QWidget* buildMute();
    void onConnectToggled(bool on);
    // Which sections show, from the radio chosen.
    void showKind();
    void fillUsbReceivers();
    void fillUsbInputs();
    bool usbRadio() const;

    QComboBox* m_kind{nullptr};
    QWidget* m_tciBox{nullptr};
    QWidget* m_usbBox{nullptr};
    QWidget* m_muteBox{nullptr};
    QComboBox* m_usbInput{nullptr};
    QComboBox* m_usbReceiver{nullptr};
    QDoubleSpinBox* m_usbTrim{nullptr};
    QPushButton* m_usbRefresh{nullptr};
    QPushButton* m_usbConnect{nullptr};
    QLabel* m_usbLabel{nullptr};
    QLabel* m_usbSetup{nullptr};
    QList<AudioDeviceInfo> m_inputs;

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
