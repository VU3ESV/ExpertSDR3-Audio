#pragma once

#include "gui/pages/Pages.h"

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>

class QTimer;

namespace AetherSDR {
class AudioEngine;
class ClientCompKnob;
}

namespace esa {

class DetailsWindow;

// What one chain stage's AetherRX page is made of: upstream's panel for it, and what this
// app needs to know about that panel to bind it.
struct StageSpec {
    QString stage;  // RxChain's id: "gate"
    // Builds the vendored panel on the stand-in engine.
    std::function<QWidget*(AetherSDR::AudioEngine*)> make;
    // Binds it to the receive stage: upstream's showForRx(), or showForPath(Rx) for the EQ.
    std::function<void(QWidget* panel)> showForRx;
    // Redraws every control from the stage it is bound to: syncControlsFromEngine(), or
    // refreshFromEngine() for the EQ.
    std::function<void(QWidget* panel)> sync;
    // Whether the panel needs `sync` to show a value changed elsewhere, such as in the
    // exact-values window: its own tick redraws only some of its controls, or it has no
    // tick. The host then redraws it after each change to the stage.
    bool syncOnChange{false};
    // The keys the panel edits. Only these are read back from it; the rest of the stage's
    // keys go one way, from the settings to the page.
    QStringList owned;
    // The panel's knobs as its layout shows them, by the label each draws and the key it
    // edits. Upstream gives them no names; these are set as their accessible names, so
    // where two knobs draw the same label the name adds the group the page shows them in.
    // A knob that edits nothing on RX has an empty key.
    QList<QPair<QString, QString>> knobs;
    // For a key the page does not show exactly (DetailsWindow::keysNotOnPage), whether it
    // does show this value exactly after all. Empty: it never does.
    std::function<bool(const QString& key, double value)> showsExactly;
    // Corrections to upstream's behaviour, made from outside once the panel is bound.
    std::function<void(QWidget* panel)> correct;
};

// A chain stage's AetherRX page: upstream's own panel, built, bound and wrapped exactly as
// AetherRxDialog builds its pages (upstream:src/gui/AetherRxDialog.cpp), on the stand-in
// engine, whose stages are the display mirror. It declares the keys its panel edits, fills
// its stage's exact-values window with the stage's controls, and redraws the panel when
// the settings are replaced.
class StageHost : public Page {
    Q_OBJECT

public:
    StageHost(PageContext& context, StageSpec spec);

    QString stage() const { return m_spec.stage; }
    const StageSpec& spec() const { return m_spec; }
    QWidget* panel() const { return m_panel; }
    DetailsWindow* details() const { return m_details; }

    // The panel's knob for `key`, among those the spec names; null if there is none.
    AetherSDR::ClientCompKnob* knob(const QString& key) const;

    // Whether the page leaves out a value the operator has moved from its default: what
    // the header's exact-values button marks with a dot.
    bool hidesChangedValue() const;

    // Redraws the panel from the mirror.
    void syncPanel();

    void attach() override;
    void showChain() override;

private:
    void nameKnobs();

    StageSpec m_spec;
    QWidget* m_panel{nullptr};
    QTimer* m_redraw{nullptr};
    DetailsWindow* m_details{nullptr};
    QList<QPair<QString, AetherSDR::ClientCompKnob*>> m_knobs;  // key, knob
};

// The pages built on StageHost, one function each.
Page* makeGatePage(PageContext& context);
Page* makeEqPage(PageContext& context);
Page* makeCompPage(PageContext& context);
Page* makeTubePage(PageContext& context);
Page* makeExciterPage(PageContext& context);

} // namespace esa
