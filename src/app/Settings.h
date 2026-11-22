#pragma once

#include "dsp/ParamSpec.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>

class QSettings;

namespace esa {

// Volume's range, as it has always been.
inline constexpr double kMinOutputGainDb = -60.0;
inline constexpr double kMaxOutputGainDb = 30.0;
// A USB radio's input trim.
inline constexpr double kMinUsbTrimDb = -20.0;
inline constexpr double kMaxUsbTrimDb = 20.0;
// The baud rates a Kenwood's virtual serial port offers (TS-990S menu 0-07-01).
inline const QList<int> kCatBauds{4800, 9600, 19200, 38400, 57600, 115200};

// Everything that describes how the app should run. The GUI edits this, `--flags`
// override it, and the engine consumes it.
struct AppConfig {
    // How the radio is reached: "tci" (ExpertSDR3 over the network) or "usb" (a radio's own
    // USB audio codec, with CAT on its serial port).
    QString radioKind{QStringLiteral("tci")};
    // A USB radio: which model (UsbModels), the input device its codec appears as, and a
    // trim for its level. `receiver` picks Main or Sub where the model has both.
    QString usbModel{QStringLiteral("ts990s")};
    QString usbInput;  // an input device key; empty: none chosen yet, so nothing is opened
    double usbTrimDb{0.0};
    // The radio's CAT port (QSerialPortInfo::portName) and baud rate, where the model has CAT.
    // Empty: audio only, and nothing is ever sent to the radio.
    QString usbCatPort;
    int usbCatBaud{115200};
    QString host{QStringLiteral("127.0.0.1")};
    int port{40001};
    // Tried if nothing answers on `port`: the ports ExpertSDR3 installs commonly use.
    // Passing --port empties this, so an explicit choice is never second-guessed.
    QList<int> fallbackPorts{40001, 50001, 60001};
    int receiver{0};
    QString deviceKey;  // empty: system default
    int chainRate{48000};
    int bufferMs{60};
    int deviceRate{0};  // 0: prefer the chain's rate, else the device's default
    double outputGainDb{0.0};
    bool boost{false};
    bool muteExpert{true};
    bool nrAutoBypass{true};
    QString nrMethod;  // empty: off
    QString profileName;  // the profile these settings came from; empty: the factory settings
    QStringList stageOrder;
    QHash<QString, double> params;  // only keys that differ from their spec defaults
};

// All parameters that exist in this build: stages plus every compiled-in NR method.
const QList<ParamSpec>& allParamSpecs();
const ParamSpec* findParamSpec(const QString& key);

// Spec value or stored override.
double paramValue(const AppConfig& config, const ParamSpec& spec);

// Persists to the platform's native settings store, or to an ini file at `path`
// when one is given (`--settings`, used to keep test runs away from real settings).
class Settings {
public:
    explicit Settings(const QString& path = QString());
    // Another application's entry in the native store. Real installs keep their settings there,
    // so tests use this to exercise that store without touching anyone's real settings.
    Settings(const QString& organization, const QString& application);
    ~Settings();

    AppConfig load() const;
    void save(const AppConfig& config);

    // Ownership of a radio's mute, kept apart from `save()`: it is written from the engine thread
    // and must reach the disk at once to survive a crash. Per radio, because a mute we owe radio A
    // says nothing about radio B, which its operator may well have muted themselves.
    struct MuteOwnership {
        bool owned{false};
        bool priorMuted{false};
    };
    MuteOwnership loadMuteOwnership(const QString& radioHost) const;
    void saveMuteOwnership(const QString& radioHost, const MuteOwnership& ownership);

    // The window's own state (ui/*). Outside AppConfig and every profile: it says how this
    // computer's window was left, not how the audio sounds. Empty means never saved.
    struct WindowState {
        QString page;      // ui/page: the page it showed last
        QString geometry;  // ui/geometry: "x,y,w,h" of the window when not maximised
        // ui/frame: "System" or "Custom", as last chosen from the gear's Window frame menu
        // (WindowChrome). Empty means the platform's default, so a later change of default
        // still reaches it.
        QString frame;
    };
    WindowState loadWindowState() const;
    void saveWindowState(const WindowState& state);

    // The method the AetherNR rail box turns back on (nr/lastMethod); empty if none was ever
    // chosen, or it is not in this build. Outside every profile, like the window's state.
    QString lastNrMethod() const;
    void setLastNrMethod(const QString& id);

    // Where REC saves its files (recording/folder). Outside AppConfig and every profile: it says
    // where files go on this computer, not how the audio sounds. Unset or empty means the default.
    QString recordingFolder() const;
    // An empty folder goes back to the default.
    void setRecordingFolder(const QString& folder);
    // Documents/<kAppDisplayName>/Recordings, or the home folder's when the system names no
    // Documents folder.
    static QString defaultRecordingFolder();

    // Display choices the AetherSDR pages keep under their own names, such as the EQ's
    // reference curve, stored as ui/aether/<name>. Outside AppConfig and every profile, like
    // the window's state. An invalid QVariant means never stored.
    QVariant aetherValue(const QString& name) const;
    void setAetherValue(const QString& name, const QVariant& value);
    void removeAetherValue(const QString& name);

    QString filePath() const;

private:
    explicit Settings(QSettings* store);
    void adoptSingleMuteRecord();

    QSettings* m_settings;
};

} // namespace esa
