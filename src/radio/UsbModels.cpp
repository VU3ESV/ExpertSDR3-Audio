#include "radio/UsbModels.h"

namespace esa::usb {

const QList<UsbModel>& models()
{
    static const QList<UsbModel> all = [] {
        QList<UsbModel> list;

        UsbModel ts990;
        ts990.id = QStringLiteral("ts990s");
        ts990.make = QStringLiteral("Kenwood");
        ts990.name = QStringLiteral("TS-990S");
        // In Normal mode the left channel is the main receiver and the right the sub
        // (menu 7-16, "USB Audio" stereo by band).
        ts990.receivers = {QStringLiteral("Main"), QStringLiteral("Sub")};
        ts990.channels = {0, 1};
        ts990.codecHint = QStringLiteral("USB Audio CODEC");
        ts990.catId = QStringLiteral("022");
        ts990.catBaud = 115200;
        ts990.bridgeHint = QStringLiteral("10C4:EA60");  // the CP2102 beside its codec
        ts990.setup = QStringLiteral(
            "On the TS-990S: USB audio is \"USB Audio CODEC\" in this computer's inputs. Its level is set "
            "by the radio's USB audio output menus, not by AF, so turn AF down to silence the radio's "
            "own speaker. Beeps and sidetone are in the USB audio unless the radio's USB output is set "
            "to received audio only.");
        list.append(ts990);

        UsbModel generic;
        generic.id = QStringLiteral("generic");
        generic.name = QStringLiteral("Other USB radio");
        generic.receivers = {QStringLiteral("Left channel"), QStringLiteral("Right channel")};
        generic.channels = {0, 1};
        generic.setup = QStringLiteral(
            "Audio only: choose the radio's codec among this computer's inputs, and turn the radio's "
            "own volume down by hand.");
        list.append(generic);
        return list;
    }();
    return all;
}

const UsbModel& model(const QString& id)
{
    const QList<UsbModel>& all = models();
    for (const UsbModel& m : all) {
        if (m.id == id) return m;
    }
    return all.last();  // generic
}

} // namespace esa::usb
