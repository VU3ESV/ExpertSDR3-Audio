#pragma once

#include "core/AudioEngine.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class QTimer;

namespace esa {

class ParamControl;
struct StageSet;

// Keeps the AetherRX pages' display mirror (LiveTaps::view) and the window's ParamControls
// in step, through the parameter table's apply and read pairs.
//
// The pages draw from the mirror and write into it, then call a save on the shim engine.
// A save pulls the stage: each key that page edits (its owned keys) is read back from the
// mirror, rounded to the table's decimals, and handed to that key's ParamControl when it
// differs, so the window's config, the engine and every other view of the value follow
// through the one path a ParamControl change always takes. Everything the page does not
// own flows one way, config to mirror, so a page that writes a value on its own (upstream
// forces a few when shown) can never change the sound.
//
// GUI thread only.
class StageBinding : public QObject {
    Q_OBJECT

public:
    struct Host {
        // What the window holds for a stage key: its config value, or the table's default.
        std::function<double(const QString& key)> value;
        // The one control that changes a key; null if there is none.
        std::function<ParamControl*(const QString& key)> control;
    };

    StageBinding(StageSet* view, Host host, QObject* parent = nullptr);

    // Upstream's engine surface over the mirror: the five RX stages are the mirror's, and
    // each stage's saves pull that stage.
    void fillHooks(AetherSDR::AudioEngine::Hooks* hooks);

    // The keys a page's panel edits, for its stage. Pulled in the table's order whatever
    // the order given, because the gate's mode must land before the ratio and floor it
    // presets. Keys that are not the stage's own are ignored.
    void own(const QString& stage, const QStringList& keys);
    QStringList ownedKeys(const QString& stage) const;

    // Saves before this are ignored: the pages save as they build themselves, before the
    // mirror is seeded with what is really set.
    void goLive();
    bool isLive() const { return m_live; }

    // Reads the stage's owned keys back from the mirror into their controls, in table
    // order, and puts any other key of the stage the page changed back as the settings
    // hold it. Called again from inside a pull (a control's change reaching the window,
    // which pulls to pick up side effects such as the gate mode's preset), it is not
    // nested: the walk under way goes round once more instead.
    void pull(const QString& stage);

    // A stage key's control changed: the mirror takes the control's value.
    void apply(const QString& key, double value);
    // Every stage key from the window's values into the mirror, in table order, so a saved
    // ratio lands after the gate mode that would otherwise preset it.
    void seed();

    // The stage a key belongs to ("gate"), or empty for a key that is not a stage's.
    static QString stageOf(const QString& key);

    // The first stage key whose mirror value differs from the window's by half a step or
    // more, described; empty when they agree. A mirror that drifts shows the operator
    // values the audio is not using.
    QString firstMismatch() const;
    // Compares every `ms` and logs the first mismatch found, once until it changes. Debug
    // builds do this from the start, and so does any build with ESA_CHECK_MIRROR set.
    void checkEvery(int ms);

private:
    void pullKey(const QString& key, bool owned);

    StageSet* m_view;
    Host m_host;
    QHash<QString, QStringList> m_owned;
    bool m_live{false};
    bool m_pulling{false};
    QSet<QString> m_pending;  // stages asked for while a pull is walking
    QTimer* m_check{nullptr};
    QString m_lastLogged;
};

} // namespace esa
