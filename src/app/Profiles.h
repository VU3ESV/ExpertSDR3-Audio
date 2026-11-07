#pragma once

#include "app/Settings.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace esa {

// The audio chain's settings: everything that shapes the sound, and nothing about this station.
// The radio, output device, volume and buffer stay out, so a profile can move to another computer
// without dragging its hardware along.
struct ChainSettings {
    QString nrMethod;  // empty: off
    bool nrAutoBypass{true};
    QStringList stageOrder;
    QHash<QString, double> params;
};

ChainSettings chainOf(const AppConfig& config);
void applyChain(AppConfig& config, const ChainSettings& chain);

// The factory settings.
ChainSettings defaultChain();

// Whether they would sound the same in this build. Keys and methods this build lacks are ignored,
// so a profile from a Mac does not show as modified on Linux just for carrying MNR's settings.
bool sameChain(const ChainSettings& a, const ChainSettings& b);

struct Profile {
    QString name;
    QString createdBy;
    QString createdAt;  // ISO 8601, UTC
    ChainSettings chain;
};

struct ImportResult {
    // The name each one ended up with, which differs from its name in the file if that was taken.
    QStringList imported;
    QStringList renamed;  // "Old name → New name"
    QString error;        // set only if nothing could be imported
};

// The operator's saved profiles, in one JSON file. An export is the same format holding fewer
// profiles, so importing a whole library from another computer needs nothing extra.
class ProfileLibrary {
public:
    static constexpr int kFormatVersion = 1;
    static constexpr int kMaxNameLength = 64;

    // `path` is created when first needed. A file that cannot be read is moved aside, not
    // overwritten: see loadWarning().
    explicit ProfileLibrary(const QString& path);

    // Next to `settingsPath` if one is given, so test runs stay away from real profiles.
    static QString defaultPath(const QString& settingsPath = QString());

    // A name as it will be stored: trimmed, single-spaced, no control characters, length-limited.
    // Empty means unusable.
    static QString cleanName(const QString& name);

    QString path() const { return m_path; }
    QString loadWarning() const { return m_loadWarning; }

    QStringList names() const;  // sorted, ignoring case
    const Profile* find(const QString& name) const;
    // "Rain Static" -> "Rain Static (2)": the first spelling not in use.
    QString uniqueName(const QString& desired) const;

    // Overwrites a profile of the same name, whatever its case. Stores every parameter this build
    // has, so a later change to a default cannot alter how a saved profile sounds.
    bool save(const QString& name, const ChainSettings& chain, QString* error = nullptr);
    bool remove(const QString& name, QString* error = nullptr);

    // The file is treated as untrusted: sizes are capped, values are clamped to their ranges, and
    // anything malformed is rejected. Never overwrites an existing profile.
    ImportResult importFile(const QString& path);
    bool exportFile(const QString& path, const QStringList& names, QString* error = nullptr) const;
    // The same content as exportFile() writes; empty if none of the names exist.
    QByteArray exportDocument(const QStringList& names) const;

private:
    void load();
    bool write(QString* error);

    QString m_path;
    QString m_loadWarning;
    QList<Profile> m_profiles;
};

} // namespace esa
