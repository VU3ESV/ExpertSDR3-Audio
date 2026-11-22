#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace esa {

// What the app knows of a radio with its own USB audio codec. It fails closed: a field is set
// only where it has been checked, and a model with no CAT fields is reached by audio alone.
struct UsbModel {
    QString id;             // stored in the settings: radio/usb/model
    QString make;           // "Kenwood"
    QString name;           // "TS-990S", as the window shows it
    // The receivers its codec carries, in order: the receiver index picks one. Each is a
    // channel of the stereo stream: 0 left, 1 right.
    QStringList receivers;  // "Main", "Sub"
    QList<int> channels;
    // How the codec shows up among this computer's input devices, as a hint only.
    QString codecHint;      // "USB Audio CODEC"
    // CAT over the radio's virtual serial port; empty where this app has none for the model.
    QString catId;          // the radio's answer to ID;, such as "022"
    int catBaud{0};
    QString bridgeHint;     // the USB serial bridge's VID:PID, as a hint for the port list
    // The main receiver's AF can be read and set over CAT, so its speaker can be turned down
    // while this app plays. `afMuteVerified` once that has been bench-tested on a real radio.
    bool afMute{false};
    bool afMuteVerified{false};
    // Things to set on the radio, shown where the radio is chosen.
    QString setup;

    QString label() const { return make.isEmpty() ? name : make + QLatin1Char(' ') + name; }
    bool hasCat() const { return !catId.isEmpty(); }
};

namespace usb {
const QList<UsbModel>& models();
// The model with this id, or the generic one when the id is unknown.
const UsbModel& model(const QString& id);
} // namespace usb

} // namespace esa
