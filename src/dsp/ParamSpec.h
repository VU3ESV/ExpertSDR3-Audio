#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace esa {

// One user-adjustable parameter. This table entry is the single source of truth:
// the GUI builds its control from it, settings persist under `key`, and
// `--set key=value` addresses it. Adding a parameter means adding one entry.
struct ParamSpec {
    QString key;  // "comp.threshold", group before the dot
    QString label;
    double min{0.0};
    double max{1.0};
    double def{0.0};
    int decimals{1};
    QString unit{};
    QStringList choices{};  // non-empty: a combo box whose value is the index
    bool toggle{false};     // a checkbox; value 0 or 1
};

inline ParamSpec toggleParam(const QString& key, const QString& label, bool def)
{
    ParamSpec spec{key, label, 0.0, 1.0, def ? 1.0 : 0.0};
    spec.decimals = 0;
    spec.toggle = true;
    return spec;
}

inline ParamSpec choiceParam(const QString& key, const QString& label, const QStringList& choices,
                             int def)
{
    ParamSpec spec{key, label, 0.0, double(choices.size() - 1), double(def)};
    spec.decimals = 0;
    spec.choices = choices;
    return spec;
}

} // namespace esa
