#include "app/Settings.h"

#include "app/AppInfo.h"
#include "dsp/NrProcessor.h"
#include "dsp/RxChain.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace esa {

const QList<ParamSpec>& allParamSpecs()
{
    static const QList<ParamSpec> specs = [] {
        QList<ParamSpec> list = RxChain::stageParams();
        for (const NrMethod& method : nrMethods()) list.append(method.params);
        return list;
    }();
    return specs;
}

const ParamSpec* findParamSpec(const QString& key)
{
    const auto& specs = allParamSpecs();
    const auto it = std::find_if(specs.begin(), specs.end(),
                                 [&](const ParamSpec& s) { return s.key == key; });
    return it == specs.end() ? nullptr : &*it;
}

double paramValue(const AppConfig& config, const ParamSpec& spec)
{
    return config.params.value(spec.key, spec.def);
}

Settings::Settings(QSettings* store)
    : m_settings(store)
{
    adoptSingleMuteRecord();
}

Settings::Settings(const QString& path)
    : Settings(path.isEmpty()
                   ? new QSettings(QStringLiteral("ExpertSDR3Audio"), QStringLiteral("ExpertSDR3Audio"))
                   : new QSettings(path, QSettings::IniFormat))
{
}

Settings::Settings(const QString& organization, const QString& application)
    : Settings(new QSettings(organization, application))
{
}

// v0.1.0 kept one record for whichever radio came next, naming none. Whoever it crashed on while
// holding the mute should still get that radio back, and the saved host is the best guide there
// is: the window saved it within seconds of connecting. Only a headless `--host` run went unsaved.
void Settings::adoptSingleMuteRecord()
{
    const QString ownedKey = QStringLiteral("muteOwnership/owned");
    const QString priorKey = QStringLiteral("muteOwnership/priorMuted");
    if (!m_settings->contains(ownedKey)) return;

    const MuteOwnership record{m_settings->value(ownedKey).toBool(), m_settings->value(priorKey).toBool()};
    // Written before the old keys go, so dying in between only means doing this again.
    if (record.owned) saveMuteOwnership(load().host, record);
    m_settings->remove(ownedKey);
    m_settings->remove(priorKey);
    m_settings->sync();
}

Settings::~Settings()
{
    delete m_settings;
}

QString Settings::filePath() const
{
    return m_settings->fileName();
}

AppConfig Settings::load() const
{
    AppConfig c;
    QSettings& s = *m_settings;
    c.host = s.value(QStringLiteral("radio/host"), c.host).toString();
    c.port = s.value(QStringLiteral("radio/port"), c.port).toInt();
    c.receiver = s.value(QStringLiteral("radio/receiver"), c.receiver).toInt();
    c.deviceKey = s.value(QStringLiteral("output/device")).toString();
    c.bufferMs = s.value(QStringLiteral("output/bufferMs"), c.bufferMs).toInt();
    // A damaged or hand-edited file can hold "nan", which no clamp after this stops on its way
    // to the speakers; that is no Volume at all, so the default.
    const double gainDb = s.value(QStringLiteral("output/gainDb"), c.outputGainDb).toDouble();
    if (std::isfinite(gainDb)) c.outputGainDb = std::clamp(gainDb, kMinOutputGainDb, kMaxOutputGainDb);
    c.boost = s.value(QStringLiteral("output/boost"), c.boost).toBool();
    c.muteExpert = s.value(QStringLiteral("output/muteExpert"), c.muteExpert).toBool();
    c.chainRate = s.value(QStringLiteral("chain/rate"), c.chainRate).toInt();
    if (c.chainRate != 24000 && c.chainRate != 48000) c.chainRate = 48000;
    c.nrMethod = s.value(QStringLiteral("nr/method")).toString();
    if (!findNrMethod(c.nrMethod)) c.nrMethod.clear();  // e.g. settings moved from another platform
    c.nrAutoBypass = s.value(QStringLiteral("nr/autoBypass"), c.nrAutoBypass).toBool();
    c.profileName = s.value(QStringLiteral("chain/profile")).toString();
    c.stageOrder = s.value(QStringLiteral("chain/order"), RxChain::stageIds()).toStringList();

    s.beginGroup(QStringLiteral("params"));
    const QStringList keys = s.childKeys();
    for (const QString& stored : keys) {
        const QString key = QString(stored).replace(QLatin1Char('/'), QLatin1Char('.'));
        if (const ParamSpec* spec = findParamSpec(key)) {
            c.params.insert(key, std::clamp(s.value(stored).toDouble(), spec->min, spec->max));
        }
    }
    s.endGroup();
    return c;
}

void Settings::save(const AppConfig& c)
{
    QSettings& s = *m_settings;
    s.setValue(QStringLiteral("radio/host"), c.host);
    s.setValue(QStringLiteral("radio/port"), c.port);
    s.setValue(QStringLiteral("radio/receiver"), c.receiver);
    s.setValue(QStringLiteral("output/device"), c.deviceKey);
    s.setValue(QStringLiteral("output/bufferMs"), c.bufferMs);
    s.setValue(QStringLiteral("output/gainDb"), c.outputGainDb);
    s.setValue(QStringLiteral("output/boost"), c.boost);
    s.setValue(QStringLiteral("output/muteExpert"), c.muteExpert);
    s.setValue(QStringLiteral("chain/rate"), c.chainRate);
    s.setValue(QStringLiteral("chain/order"), c.stageOrder);
    s.setValue(QStringLiteral("chain/profile"), c.profileName);
    s.setValue(QStringLiteral("nr/method"), c.nrMethod);
    s.setValue(QStringLiteral("nr/autoBypass"), c.nrAutoBypass);

    s.remove(QStringLiteral("params"));
    s.beginGroup(QStringLiteral("params"));
    // A dot in a QSettings key is fine on every backend, and '/' would nest groups.
    for (auto it = c.params.constBegin(); it != c.params.constEnd(); ++it) {
        s.setValue(it.key(), it.value());
    }
    s.endGroup();
    s.sync();
}

namespace {

QString ownershipGroup(const QString& radioHost)
{
    return QStringLiteral("muteOwnership/")
           + QString::fromLatin1(QUrl::toPercentEncoding(radioHost.toLower()));
}

} // namespace

Settings::MuteOwnership Settings::loadMuteOwnership(const QString& radioHost) const
{
    const QString group = ownershipGroup(radioHost);
    MuteOwnership o;
    o.owned = m_settings->value(group + QStringLiteral("/owned"), false).toBool();
    o.priorMuted = m_settings->value(group + QStringLiteral("/priorMuted"), false).toBool();
    return o;
}

void Settings::saveMuteOwnership(const QString& radioHost, const MuteOwnership& o)
{
    const QString group = ownershipGroup(radioHost);
    if (o.owned) {
        m_settings->setValue(group + QStringLiteral("/owned"), true);
        m_settings->setValue(group + QStringLiteral("/priorMuted"), o.priorMuted);
    } else {
        m_settings->remove(group);  // nothing owed: leave no trace of every radio ever used
    }
    m_settings->sync();  // must survive a crash a moment later
}

Settings::WindowState Settings::loadWindowState() const
{
    WindowState w;
    w.page = m_settings->value(QStringLiteral("ui/page")).toString();
    w.geometry = m_settings->value(QStringLiteral("ui/geometry")).toString();
    const QString frame = m_settings->value(QStringLiteral("ui/frame")).toString();
    if (frame == QLatin1String("System") || frame == QLatin1String("Custom")) w.frame = frame;
    return w;
}

void Settings::saveWindowState(const WindowState& w)
{
    const auto put = [this](const QString& key, const QString& value) {
        // Never written empty, so "not saved yet" stays distinguishable from a choice.
        if (value.isEmpty()) {
            m_settings->remove(key);
        } else {
            m_settings->setValue(key, value);
        }
    };
    put(QStringLiteral("ui/page"), w.page);
    put(QStringLiteral("ui/geometry"), w.geometry);
    put(QStringLiteral("ui/frame"), w.frame);
    m_settings->sync();
}

QString Settings::lastNrMethod() const
{
    const QString id = m_settings->value(QStringLiteral("nr/lastMethod")).toString();
    return findNrMethod(id) ? id : QString();
}

void Settings::setLastNrMethod(const QString& id)
{
    if (id.isEmpty() || id == lastNrMethod()) return;
    m_settings->setValue(QStringLiteral("nr/lastMethod"), id);
    m_settings->sync();
}

QString Settings::recordingFolder() const
{
    const QString stored = m_settings->value(QStringLiteral("recording/folder")).toString();
    return stored.isEmpty() ? defaultRecordingFolder() : stored;
}

void Settings::setRecordingFolder(const QString& folder)
{
    if (folder.isEmpty()) {
        m_settings->remove(QStringLiteral("recording/folder"));
    } else {
        m_settings->setValue(QStringLiteral("recording/folder"), QDir::cleanPath(folder));
    }
    m_settings->sync();
}

namespace {

QString aetherKey(const QString& name)
{
    return QStringLiteral("ui/aether/") + name;
}

} // namespace

QVariant Settings::aetherValue(const QString& name) const
{
    return m_settings->value(aetherKey(name));
}

void Settings::setAetherValue(const QString& name, const QVariant& value)
{
    m_settings->setValue(aetherKey(name), value);
}

void Settings::removeAetherValue(const QString& name)
{
    m_settings->remove(aetherKey(name));
}

QString Settings::defaultRecordingFolder()
{
    QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (documents.isEmpty()) documents = QDir::homePath();
    return QDir(documents).filePath(QString::fromUtf8(kAppDisplayName) + QStringLiteral("/Recordings"));
}

} // namespace esa
