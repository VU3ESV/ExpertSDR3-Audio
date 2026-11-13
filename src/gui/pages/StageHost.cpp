#include "gui/pages/StageHost.h"

#include "app/Settings.h"
#include "gui/DetailsWindow.h"
#include "gui/ParamWidgets.h"
#include "gui/aether/Guards.h"
#include "gui/binding/StageBinding.h"

#include "ClientCompKnob.h"
#include "StagePage.h"

#include <QLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <QtGlobal>

#include <cmath>
#include <functional>
#include <utility>

namespace esa {

namespace {

using AetherSDR::ClientCompKnob;

// The panel's knobs in the order its layouts show them: left to right along a row, rows
// top to bottom, into the widgets it lays out.
QList<ClientCompKnob*> knobsInLayoutOrder(QWidget* panel)
{
    QList<ClientCompKnob*> found;
    const std::function<void(QLayout*)> walk = [&](QLayout* layout) {
        for (int i = 0; i < layout->count(); ++i) {
            QLayoutItem* item = layout->itemAt(i);
            if (auto* knob = qobject_cast<ClientCompKnob*>(item->widget())) {
                found.append(knob);
            } else if (item->layout()) {
                walk(item->layout());
            } else if (item->widget() && item->widget()->layout()) {
                walk(item->widget()->layout());
            }
        }
    };
    if (panel->layout()) walk(panel->layout());
    return found;
}

} // namespace

StageHost::StageHost(PageContext& ctx, StageSpec spec)
    : Page(ctx), m_spec(std::move(spec))
{
    // As AetherRxDialog: the panel on the engine, then the page that fits it to the room.
    m_panel = m_spec.make(ctx.aether);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(AetherSDR::makeStagePage(m_panel));

    ctx.binding->own(m_spec.stage, m_spec.owned);

    // Every control of the stage, for the values the page shows only roughly or not at all.
    m_details = ctx.details(m_spec.stage);
    Q_ASSERT(m_details);
    if (m_details) m_details->populate(ctx.addParam);

    if (m_spec.syncOnChange && m_details) {
        // Once the change has settled, outside whatever panel handler made it: a value the
        // panel set itself is redrawn as the settings rounded it, which is harmless.
        m_redraw = new QTimer(this);
        m_redraw->setSingleShot(true);
        m_redraw->setInterval(0);
        connect(m_redraw, &QTimer::timeout, this, &StageHost::syncPanel);
        for (ParamControl* control : m_details->controls()) {
            connect(control, &ParamControl::valueChanged, m_redraw, qOverload<>(&QTimer::start));
        }
    }
}

void StageHost::attach()
{
    // As AetherRxDialog, once every page is in its stack: bound to the receive stage, then
    // the floating editor's chrome taken off.
    m_spec.showForRx(m_panel);
    AetherSDR::tidyEmbeddedPanel(m_panel);

    nameKnobs();
    WheelAccumulator::install(m_panel);
    ValueFieldGuard::install(m_panel);
    if (m_spec.correct) m_spec.correct(m_panel);
}

void StageHost::nameKnobs()
{
    const QList<ClientCompKnob*> knobs = knobsInLayoutOrder(m_panel);
    if (knobs.size() != m_spec.knobs.size()) {
        // test_aether_gui fails on this too: the panel changed in a sync.
        qWarning("The %s page has %d knobs where %d were expected; they are left unnamed.",
                 qPrintable(m_spec.stage), int(knobs.size()), int(m_spec.knobs.size()));
        return;
    }
    for (int i = 0; i < knobs.size(); ++i) {
        knobs.at(i)->setAccessibleName(m_spec.knobs.at(i).first);
        if (!m_spec.knobs.at(i).second.isEmpty()) m_knobs.append({m_spec.knobs.at(i).second, knobs.at(i)});
    }
}

ClientCompKnob* StageHost::knob(const QString& key) const
{
    for (const auto& entry : m_knobs) {
        if (entry.first == key) return entry.second;
    }
    return nullptr;
}

bool StageHost::hidesChangedValue() const
{
    const QStringList hidden = DetailsWindow::keysNotOnPage(m_spec.stage);
    for (const QString& key : hidden) {
        const ParamSpec* spec = findParamSpec(key);
        if (!spec) continue;
        const double value = paramValue(config(), *spec);
        if (std::abs(value - spec->def) < 0.5 * std::pow(10.0, -spec->decimals)) continue;
        if (m_spec.showsExactly && m_spec.showsExactly(key, value)) continue;
        return true;
    }
    return false;
}

void StageHost::syncPanel()
{
    m_spec.sync(m_panel);
}

void StageHost::showChain()
{
    syncPanel();
}

} // namespace esa
