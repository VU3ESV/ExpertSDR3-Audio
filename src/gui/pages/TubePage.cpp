#include "gui/pages/StageHost.h"

#include "StripTubePanel.h"

namespace esa {

using AetherSDR::StripTubePanel;

namespace {

StripTubePanel* tubePanel(QWidget* panel)
{
    return static_cast<StripTubePanel*>(panel);
}

} // namespace

Page* makeTubePage(PageContext& context)
{
    StageSpec spec;
    spec.stage = QStringLiteral("tube");
    spec.make = [](AetherSDR::AudioEngine* engine) -> QWidget* { return new StripTubePanel(engine); };
    spec.showForRx = [](QWidget* panel) { tubePanel(panel)->showForRx(); };
    spec.sync = [](QWidget* panel) { tubePanel(panel)->syncControlsFromEngine(); };
    // Its A, B and C buttons, the Dry/Wet slider and six knobs. The enable is the rail's,
    // and the panel has no Attack control.
    spec.owned = {QStringLiteral("tube.model"),  QStringLiteral("tube.drive"),    QStringLiteral("tube.bias"),
                  QStringLiteral("tube.tone"),   QStringLiteral("tube.output"),   QStringLiteral("tube.mix"),
                  QStringLiteral("tube.envelope"), QStringLiteral("tube.release")};
    spec.knobs = {{QStringLiteral("Drive"), QStringLiteral("tube.drive")},
                  {QStringLiteral("Tone"), QStringLiteral("tube.tone")},
                  {QStringLiteral("Bias"), QStringLiteral("tube.bias")},
                  {QStringLiteral("Envelope"), QStringLiteral("tube.envelope")},
                  {QStringLiteral("Release"), QStringLiteral("tube.release")},
                  {QStringLiteral("Output"), QStringLiteral("tube.output")}};
    return new StageHost(context, spec);
}

} // namespace esa
