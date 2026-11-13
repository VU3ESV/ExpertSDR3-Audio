#include "app/AppInfo.h"
#include "gui/pages/StageHost.h"

#include "PooDooLogo.h"
#include "StripPuduPanel.h"

#include <QPushButton>

namespace esa {

using AetherSDR::PooDooLogo;
using AetherSDR::StripPuduPanel;

namespace {

StripPuduPanel* puduPanel(QWidget* panel)
{
    return static_cast<StripPuduPanel*>(panel);
}

// What the two mode buttons do, in place of upstream's tooltips, which describe each by
// the company whose hardware it follows.
const char* const kEvenTip = "Even: asymmetric soft clipping, mostly even harmonics. Warmer, with "
                             "a saturated low end that follows the voice.";
const char* const kOddTip = "Odd: symmetric soft clipping, odd harmonics only. Brighter and edgier, "
                            "with a compressor on the low end in place of saturation.";

} // namespace

Page* makeExciterPage(PageContext& context)
{
    StageSpec spec;
    spec.stage = QStringLiteral("pudu");
    spec.make = [](AetherSDR::AudioEngine* engine) -> QWidget* { return new StripPuduPanel(engine); };
    spec.showForRx = [](QWidget* panel) { puduPanel(panel)->showForRx(); };
    spec.sync = [](QWidget* panel) { puduPanel(panel)->syncControlsFromEngine(); };
    // It has no tick: it redraws only when synced.
    spec.syncOnChange = true;
    // Its Even and Odd buttons and six knobs: every key of the stage but the rail's enable.
    // A double-click on a knob lands on upstream's default, which for Body drive (6 dB)
    // and the two mixes (30 %) is not this app's (0 dB, 50 %). Ours stay: values left at
    // their default are not stored, so changing a default changes the sound of everyone
    // who never moved that knob.
    spec.owned = {QStringLiteral("pudu.mode"),   QStringLiteral("pudu.pooDrive"), QStringLiteral("pudu.pooTune"),
                  QStringLiteral("pudu.pooMix"), QStringLiteral("pudu.dooTune"),  QStringLiteral("pudu.dooHarmonics"),
                  QStringLiteral("pudu.dooMix")};
    // Under "Body" Drive, Tune and Mix, under "Clarity" Tune, Air and Mix.
    spec.knobs = {{QStringLiteral("Body drive"), QStringLiteral("pudu.pooDrive")},
                  {QStringLiteral("Body tune"), QStringLiteral("pudu.pooTune")},
                  {QStringLiteral("Body mix"), QStringLiteral("pudu.pooMix")},
                  {QStringLiteral("Clarity tune"), QStringLiteral("pudu.dooTune")},
                  {QStringLiteral("Air"), QStringLiteral("pudu.dooHarmonics")},
                  {QStringLiteral("Clarity mix"), QStringLiteral("pudu.dooMix")}};
    spec.correct = [](QWidget* panel) {
        if (auto* logo = panel->findChild<PooDooLogo*>()) {
            logo->setWordmark(QString::fromUtf8(kExciterWordmark));
            // Painted text, which a screen reader cannot read off the page.
            logo->setAccessibleName(QString::fromUtf8(kExciterWordmark));
        }
        const auto buttons = panel->findChildren<QPushButton*>();
        for (QPushButton* button : buttons) {
            if (button->text() == QLatin1String("Even")) button->setToolTip(QString::fromUtf8(kEvenTip));
            else if (button->text() == QLatin1String("Odd")) button->setToolTip(QString::fromUtf8(kOddTip));
        }
    };
    return new StageHost(context, spec);
}

} // namespace esa
