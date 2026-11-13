#include "core/AppSettings.h"

#include <utility>

namespace AetherSDR {

namespace {

// ThemeManager reads this at start-up and writes it when the theme changes.
constexpr QLatin1String kActiveTheme("ActiveTheme");

} // namespace

AppSettings& AppSettings::instance()
{
    static AppSettings settings;
    return settings;
}

bool AppSettings::persists(const QString& key)
{
    // Choices an operator makes once and expects back. Geometry is left out on purpose:
    // the panels live inside our window, which keeps its own.
    return key == QLatin1String("ClientEqReferenceCurve")
        || key == QLatin1String("ClientEqSmoothingFraction")
        || key == QLatin1String("AetherialStripWaveformRxWindowSec");
}

void AppSettings::setStore(Store store)
{
    m_store = std::move(store);
}

bool AppSettings::stored(const QString& key) const
{
    return persists(key) && m_store.read && m_store.write && m_store.remove;
}

QVariant AppSettings::value(const QString& key, const QVariant& defaultValue) const
{
    // The name inside vendor/aethersdr/resources/themes/default-dark.json.
    if (key == kActiveTheme) return QStringLiteral("Default Dark");
    if (stored(key)) {
        const QVariant v = m_store.read(key);
        return v.isValid() ? v : defaultValue;
    }
    return m_values.value(key, defaultValue);
}

void AppSettings::setValue(const QString& key, const QVariant& value)
{
    if (key == kActiveTheme) return;
    if (stored(key)) {
        m_store.write(key, value);
        return;
    }
    m_values.insert(key, value);
}

void AppSettings::remove(const QString& key)
{
    if (key == kActiveTheme) return;
    if (stored(key)) {
        m_store.remove(key);
        return;
    }
    m_values.remove(key);
}

bool AppSettings::contains(const QString& key) const
{
    if (key == kActiveTheme) return true;
    if (stored(key)) return m_store.read(key).isValid();
    return m_values.contains(key);
}

} // namespace AetherSDR
