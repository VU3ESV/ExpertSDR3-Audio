#pragma once

#include "dsp/ParamSpec.h"

#include <QHash>
#include <QString>
#include <QStringList>

class QSettings;

namespace esa {

// Everything that describes how the app should run. The GUI edits this, `--flags`
// override it, and the engine consumes it.
struct AppConfig {
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

    QString filePath() const;

private:
    explicit Settings(QSettings* store);
    void adoptSingleMuteRecord();

    QSettings* m_settings;
};

} // namespace esa
