#include "app/Profiles.h"

#include "dsp/NrProcessor.h"
#include "dsp/RxChain.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace esa {

namespace {

const QLatin1String kFormat("ExpertSDR3Audio.profiles");
constexpr qint64 kMaxFileBytes = 4 * 1024 * 1024;
constexpr int kMaxProfilesInFile = 500;
constexpr int kMaxParamsInProfile = 2000;

QJsonObject toJson(const Profile& profile)
{
    QJsonObject params;
    for (auto it = profile.chain.params.constBegin(); it != profile.chain.params.constEnd(); ++it) {
        params.insert(it.key(), it.value());
    }
    QJsonArray order;
    for (const QString& stage : profile.chain.stageOrder) order.append(stage);
    return QJsonObject{
        {QStringLiteral("name"), profile.name},
        {QStringLiteral("createdBy"), profile.createdBy},
        {QStringLiteral("createdAt"), profile.createdAt},
        {QStringLiteral("nr"), QJsonObject{{QStringLiteral("method"), profile.chain.nrMethod},
                                           {QStringLiteral("autoBypass"), profile.chain.nrAutoBypass}}},
        {QStringLiteral("order"), order},
        {QStringLiteral("params"), params},
    };
}

QByteArray toDocument(const QList<Profile>& profiles)
{
    QJsonArray list;
    for (const Profile& profile : profiles) list.append(toJson(profile));
    const QJsonObject root{
        {QStringLiteral("format"), QString(kFormat)},
        {QStringLiteral("version"), ProfileLibrary::kFormatVersion},
        {QStringLiteral("profiles"), list},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

// Unusable entries return false, and are skipped by the caller.
bool fromJson(const QJsonValue& value, Profile* out)
{
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    const QString name = ProfileLibrary::cleanName(object.value(QStringLiteral("name")).toString());
    if (name.isEmpty()) return false;

    Profile profile;
    profile.name = name;
    profile.createdBy = object.value(QStringLiteral("createdBy")).toString().left(100);
    profile.createdAt = object.value(QStringLiteral("createdAt")).toString().left(40);

    const QJsonObject nr = object.value(QStringLiteral("nr")).toObject();
    // Kept even if this build lacks it, so the profile survives a trip through this computer.
    profile.chain.nrMethod = nr.value(QStringLiteral("method")).toString().left(40);
    profile.chain.nrAutoBypass = nr.value(QStringLiteral("autoBypass")).toBool(true);

    const QJsonArray order = object.value(QStringLiteral("order")).toArray();
    for (const QJsonValue& stage : order) {
        if (stage.isString()) profile.chain.stageOrder.append(stage.toString().left(40));
    }
    profile.chain.stageOrder = RxChain::normalizedOrder(profile.chain.stageOrder);

    const QJsonObject params = object.value(QStringLiteral("params")).toObject();
    if (params.size() > kMaxParamsInProfile) return false;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        if (!it.value().isDouble() || !std::isfinite(it.value().toDouble())) continue;
        const ParamSpec* spec = findParamSpec(it.key());
        const double value = it.value().toDouble();
        profile.chain.params.insert(it.key().left(80), spec ? std::clamp(value, spec->min, spec->max) : value);
    }
    *out = profile;
    return true;
}

// Complete and in range for this build. Keys it does not know are kept for the same reason as a
// method it does not have.
ChainSettings completed(const ChainSettings& chain)
{
    ChainSettings result = chain;
    result.stageOrder = RxChain::normalizedOrder(chain.stageOrder);
    for (const ParamSpec& spec : allParamSpecs()) {
        result.params.insert(spec.key, std::clamp(chain.params.value(spec.key, spec.def), spec.min, spec.max));
    }
    return result;
}

} // namespace

ChainSettings chainOf(const AppConfig& config)
{
    ChainSettings chain;
    chain.nrMethod = config.nrMethod;
    chain.nrAutoBypass = config.nrAutoBypass;
    chain.stageOrder = RxChain::normalizedOrder(config.stageOrder);
    chain.params = config.params;
    return chain;
}

void applyChain(AppConfig& config, const ChainSettings& chain)
{
    // A method this build lacks plays as no method, rather than silently picking another.
    config.nrMethod = findNrMethod(chain.nrMethod) ? chain.nrMethod : QString();
    config.nrAutoBypass = chain.nrAutoBypass;
    config.stageOrder = RxChain::normalizedOrder(chain.stageOrder);
    config.params.clear();
    for (const ParamSpec& spec : allParamSpecs()) {
        if (chain.params.contains(spec.key)) {
            config.params.insert(spec.key, std::clamp(chain.params.value(spec.key), spec.min, spec.max));
        }
    }
}

ChainSettings defaultChain()
{
    ChainSettings chain;
    chain.stageOrder = RxChain::stageIds();
    return chain;
}

bool sameChain(const ChainSettings& a, const ChainSettings& b)
{
    const auto method = [](const QString& id) { return findNrMethod(id) ? id : QString(); };
    if (method(a.nrMethod) != method(b.nrMethod) || a.nrAutoBypass != b.nrAutoBypass
        || RxChain::normalizedOrder(a.stageOrder) != RxChain::normalizedOrder(b.stageOrder)) {
        return false;
    }
    // Each value as it shows, at the table's decimals. A control holds no more than those, so
    // a default kept with more (EQ's Q of 0.707) and the 0.71 a Reset sets are the same value,
    // and a page's Reset leaves the window unmodified.
    for (const ParamSpec& spec : allParamSpecs()) {
        const double scale = std::pow(10.0, spec.decimals);
        const double left = std::round(a.params.value(spec.key, spec.def) * scale);
        const double right = std::round(b.params.value(spec.key, spec.def) * scale);
        if (left != right) return false;
    }
    return true;
}

QString ProfileLibrary::defaultPath(const QString& settingsPath)
{
    if (!settingsPath.isEmpty()) return settingsPath + QStringLiteral(".profiles.json");
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("profiles.json"));
}

QString ProfileLibrary::cleanName(const QString& name)
{
    QString cleaned;
    for (const QChar c : name) {
        if (!c.isNull() && c.category() != QChar::Other_Control && c.category() != QChar::Other_Format) cleaned.append(c);
    }
    return cleaned.simplified().left(kMaxNameLength).trimmed();
}

ProfileLibrary::ProfileLibrary(const QString& path)
    : m_path(path)
{
    load();
}

void ProfileLibrary::load()
{
    QFile file(m_path);
    if (!file.exists()) return;

    const auto setAside = [&](const QString& why) {
        // Never overwrite what could not be read: it may be the operator's only copy.
        const QString backup = m_path + QStringLiteral(".unreadable-")
                               + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        const bool moved = QFile::rename(m_path, backup);
        m_loadWarning = QStringLiteral("Your saved profiles could not be read (%1).").arg(why);
        if (moved) m_loadWarning += QStringLiteral(" The file was kept as %1.").arg(backup);
    };

    if (!file.open(QIODevice::ReadOnly)) {
        m_loadWarning = QStringLiteral("Your saved profiles could not be opened: %1").arg(file.errorString());
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    const QJsonObject root = document.object();
    if (parseError.error != QJsonParseError::NoError || root.value(QStringLiteral("format")).toString() != kFormat) {
        setAside(QStringLiteral("not a profile file"));
        return;
    }
    if (root.value(QStringLiteral("version")).toInt() > kFormatVersion) {
        setAside(QStringLiteral("written by a newer version"));
        return;
    }
    const QJsonArray list = root.value(QStringLiteral("profiles")).toArray();
    for (const QJsonValue& value : list) {
        Profile profile;
        if (fromJson(value, &profile) && !find(profile.name)) m_profiles.append(profile);
    }
}

bool ProfileLibrary::write(QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        return fail(QStringLiteral("Could not create the folder for %1").arg(m_path));
    }
    // All or nothing: a crash or a full disk leaves the previous file intact.
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) return fail(file.errorString());
    file.write(toDocument(m_profiles));
    if (!file.commit()) return fail(file.errorString());
    return true;
}

QStringList ProfileLibrary::names() const
{
    QStringList list;
    for (const Profile& profile : m_profiles) list.append(profile.name);
    std::sort(list.begin(), list.end(), [](const QString& a, const QString& b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return list;
}

const Profile* ProfileLibrary::find(const QString& name) const
{
    for (const Profile& profile : m_profiles) {
        if (profile.name.compare(name, Qt::CaseInsensitive) == 0) return &profile;
    }
    return nullptr;
}

QString ProfileLibrary::uniqueName(const QString& desired) const
{
    if (!find(desired)) return desired;
    for (int n = 2;; ++n) {
        const QString suffix = QStringLiteral(" (%1)").arg(n);
        const QString candidate = desired.left(kMaxNameLength - suffix.size()).trimmed() + suffix;
        if (!find(candidate)) return candidate;
    }
}

bool ProfileLibrary::save(const QString& name, const ChainSettings& chain, QString* error)
{
    const QString cleaned = cleanName(name);
    if (cleaned.isEmpty()) {
        if (error) *error = QStringLiteral("A profile needs a name.");
        return false;
    }
    Profile profile;
    profile.name = cleaned;
    profile.createdBy = QStringLiteral("ExpertSDR3 Audio %1").arg(QCoreApplication::applicationVersion());
    profile.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    profile.chain = completed(chain);

    const QList<Profile> before = m_profiles;
    for (Profile& existing : m_profiles) {
        if (existing.name.compare(cleaned, Qt::CaseInsensitive) == 0) {
            profile.name = existing.name;
            existing = profile;
            if (write(error)) return true;
            m_profiles = before;
            return false;
        }
    }
    m_profiles.append(profile);
    if (write(error)) return true;
    m_profiles = before;
    return false;
}

bool ProfileLibrary::remove(const QString& name, QString* error)
{
    const QList<Profile> before = m_profiles;
    m_profiles.removeIf([&](const Profile& p) { return p.name.compare(name, Qt::CaseInsensitive) == 0; });
    if (m_profiles.size() == before.size()) return true;
    if (write(error)) return true;
    m_profiles = before;
    return false;
}

ImportResult ProfileLibrary::importFile(const QString& path)
{
    ImportResult result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Could not open the file: %1").arg(file.errorString());
        return result;
    }
    if (file.size() > kMaxFileBytes) {
        result.error = QStringLiteral("That file is too large to be a profile file.");
        return result;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = document.object();
    if (parseError.error != QJsonParseError::NoError || root.value(QStringLiteral("format")).toString() != kFormat) {
        result.error = QStringLiteral("That is not an ExpertSDR3 Audio profile file.");
        return result;
    }
    if (root.value(QStringLiteral("version")).toInt() > kFormatVersion) {
        result.error = QStringLiteral("That file was written by a newer version of ExpertSDR3 Audio.");
        return result;
    }
    const QJsonArray list = root.value(QStringLiteral("profiles")).toArray();
    if (list.size() > kMaxProfilesInFile) {
        result.error = QStringLiteral("That file holds too many profiles.");
        return result;
    }

    const QList<Profile> before = m_profiles;
    for (const QJsonValue& value : list) {
        Profile profile;
        if (!fromJson(value, &profile)) continue;
        const QString original = profile.name;
        profile.name = uniqueName(original);
        profile.chain = completed(profile.chain);  // pinned, exactly as a saved one is
        if (profile.name != original) {
            result.renamed.append(QStringLiteral("%1 → %2").arg(original, profile.name));
        }
        m_profiles.append(profile);
        result.imported.append(profile.name);
    }
    if (result.imported.isEmpty()) {
        result.error = QStringLiteral("That file holds no usable profiles.");
        return result;
    }
    QString writeError;
    if (!write(&writeError)) {
        m_profiles = before;
        result = ImportResult{};
        result.error = QStringLiteral("Could not save the imported profiles: %1").arg(writeError);
    }
    return result;
}

QByteArray ProfileLibrary::exportDocument(const QStringList& names) const
{
    QList<Profile> selected;
    for (const QString& name : names) {
        if (const Profile* profile = find(name)) selected.append(*profile);
    }
    return selected.isEmpty() ? QByteArray() : toDocument(selected);
}

bool ProfileLibrary::exportFile(const QString& path, const QStringList& names, QString* error) const
{
    const QByteArray document = exportDocument(names);
    if (document.isEmpty()) {
        if (error) *error = QStringLiteral("There is nothing to export.");
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    file.write(document);
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace esa
