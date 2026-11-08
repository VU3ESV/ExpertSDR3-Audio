#pragma once

// Stands in for AetherSDR's core/AppSettings.h, for the vendored pages and ThemeManager.
// Upstream keeps a settings database; this keeps everything in memory for the session
// (window geometry, the output trim and so on), except a short allowlist of display
// preferences that the host may persist through a Store. "ActiveTheme" always reads as
// the built-in dark theme and ignores writes: no other theme is vendored.
//
// GUI thread only.

#include <QHash>
#include <QString>
#include <QVariant>

#include <functional>

namespace AetherSDR {

class AppSettings {
public:
    static AppSettings& instance();

    QVariant value(const QString& key, const QVariant& defaultValue = {}) const;
    void setValue(const QString& key, const QVariant& value);
    void remove(const QString& key);
    bool contains(const QString& key) const;
    // Upstream flushes its database here. Persisted keys are written as they change,
    // so there is nothing left to do.
    void save() {}

    // Where the allowlisted keys go. Each function gets the upstream key unchanged;
    // esa::Settings keeps them under ui/aether/<key>. An invalid QVariant from `read`
    // means "never stored". Without a store these keys live in memory like the rest.
    struct Store {
        std::function<QVariant(const QString& key)> read;
        std::function<void(const QString& key, const QVariant& value)> write;
        std::function<void(const QString& key)> remove;
    };
    void setStore(Store store);

    // The keys that persist: the EQ's reference curve and smoothing, and the length of
    // the waveform's window.
    static bool persists(const QString& key);

    AppSettings(const AppSettings&) = delete;
    AppSettings& operator=(const AppSettings&) = delete;

private:
    AppSettings() = default;

    bool stored(const QString& key) const;

    QHash<QString, QVariant> m_values;
    Store m_store;
};

} // namespace AetherSDR
