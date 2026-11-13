#include "gui/aether/Guards.h"
#include "gui/pages/StageHost.h"

#include "StripGatePanel.h"

#include <QPushButton>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace esa {

using AetherSDR::StripGatePanel;

namespace {

// The look-ahead stops of the panel's Peek slider, as StripGatePanel.cpp lists them.
// test_aether_gui moves the slider through every stop and checks these still match.
constexpr double kPeekStopsMs[] = {0.0, 1.0, 1.5, 3.0, 5.0};

StripGatePanel* gatePanel(QWidget* panel)
{
    return static_cast<StripGatePanel*>(panel);
}

} // namespace

Page* makeGatePage(PageContext& context)
{
    StageSpec spec;
    spec.stage = QStringLiteral("gate");
    spec.make = [](AetherSDR::AudioEngine* engine) -> QWidget* { return new StripGatePanel(engine); };
    spec.showForRx = [](QWidget* panel) { gatePanel(panel)->showForRx(); };
    spec.sync = [](QWidget* panel) { gatePanel(panel)->syncControlsFromEngine(); };
    // Its two mode buttons, six knobs and Peek slider. The enable is the rail's, and it has
    // no Attack knob on RX.
    spec.owned = {QStringLiteral("gate.mode"),    QStringLiteral("gate.threshold"), QStringLiteral("gate.ratio"),
                  QStringLiteral("gate.release"), QStringLiteral("gate.hold"),      QStringLiteral("gate.floor"),
                  QStringLiteral("gate.return"),  QStringLiteral("gate.lookahead")};
    spec.knobs = {{QStringLiteral("Thresh"), QStringLiteral("gate.threshold")},
                  {QStringLiteral("Return"), QStringLiteral("gate.return")},
                  {QStringLiteral("Hold"), QStringLiteral("gate.hold")},
                  {QStringLiteral("Release"), QStringLiteral("gate.release")},
                  {QStringLiteral("Floor"), QStringLiteral("gate.floor")},
                  {QStringLiteral("Ratio"), QStringLiteral("gate.ratio")}};
    // The slider shows the stop nearest the look-ahead without writing it, so the page shows
    // a look-ahead exactly only when it is one of the stops.
    spec.showsExactly = [](const QString& key, double value) {
        if (key != QLatin1String("gate.lookahead")) return false;
        return std::any_of(std::begin(kPeekStopsMs), std::end(kPeekStopsMs),
                           [value](double stop) { return std::abs(stop - value) < 0.05; });
    };
    spec.correct = [](QWidget* panel) {
        LitModeGuard::install({panel->findChild<QPushButton*>(QStringLiteral("gateModeGate")),
                               panel->findChild<QPushButton*>(QStringLiteral("gateModeExpander"))});
    };
    return new StageHost(context, spec);
}

} // namespace esa
