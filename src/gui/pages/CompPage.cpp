#include "gui/aether/Guards.h"
#include "gui/pages/StageHost.h"

#include "ClientCompMeter.h"
#include "StripCompPanel.h"

namespace esa {

using AetherSDR::ClientCompMeter;
using AetherSDR::StripCompPanel;

namespace {

StripCompPanel* compPanel(QWidget* panel)
{
    return static_cast<StripCompPanel*>(panel);
}

// The Out meter, which carries the makeup handle: the panel's GR meter has none.
ClientCompMeter* makeupMeter(QWidget* panel)
{
    const auto meters = panel->findChildren<ClientCompMeter*>();
    for (ClientCompMeter* meter : meters) {
        if (meter->mode() == ClientCompMeter::Mode::Level && meter->makeupControlEnabled()) return meter;
    }
    return nullptr;
}

} // namespace

Page* makeCompPage(PageContext& context)
{
    StageSpec spec;
    spec.stage = QStringLiteral("comp");
    spec.make = [](AetherSDR::AudioEngine* engine) -> QWidget* { return new StripCompPanel(engine); };
    // Upstream's showForRx() also zeroes Drive and Phase and saves. Neither key is the
    // page's, so the save puts back what the settings hold and the sound never changes.
    spec.showForRx = [](QWidget* panel) { compPanel(panel)->showForRx(); };
    spec.sync = [](QWidget* panel) { compPanel(panel)->syncControlsFromEngine(); };
    // Its meter tick redraws the knobs, the threshold, makeup and the ceiling, not LIMIT.
    spec.syncOnChange = true;
    // Its threshold fader and curve, four knobs, the makeup handle on the Out meter, the
    // LIMIT button and the ceiling slider. The enable is the rail's; Drive and Phase are
    // transmit controls that upstream hides on RX.
    spec.owned = {QStringLiteral("comp.threshold"), QStringLiteral("comp.ratio"),   QStringLiteral("comp.attack"),
                  QStringLiteral("comp.release"),   QStringLiteral("comp.knee"),    QStringLiteral("comp.makeup"),
                  QStringLiteral("comp.limiter"),   QStringLiteral("comp.ceiling")};
    // Drive and Phase are built on RX too, hidden, and edit nothing there: the panel drops
    // their changes on that side. Their values are in the exact-values window.
    spec.knobs = {{QStringLiteral("Ratio"), QStringLiteral("comp.ratio")},
                  {QStringLiteral("Attack"), QStringLiteral("comp.attack")},
                  {QStringLiteral("Release"), QStringLiteral("comp.release")},
                  {QStringLiteral("Knee"), QStringLiteral("comp.knee")},
                  {QStringLiteral("Drive"), QString()},
                  {QStringLiteral("Phase"), QString()}};
    spec.correct = [](QWidget* panel) { MakeupHitGuard::install(makeupMeter(panel)); };
    return new StageHost(context, spec);
}

} // namespace esa
