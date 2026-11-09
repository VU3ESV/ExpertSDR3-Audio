#include "app/LiveTaps.h"
#include "gui/DetailsWindow.h"
#include "gui/ParamWidgets.h"
#include "gui/aether/Guards.h"
#include "gui/binding/EqFeed.h"
#include "gui/binding/StageBinding.h"
#include "gui/pages/StageHost.h"

#include "ClientEqApplet.h"
#include "ClientEqEditorCanvas.h"
#include "ClientEqIconRow.h"
#include "ClientEqParamRow.h"
#include "StripEqPanel.h"
#include "core/AudioEngine.h"
#include "core/ClientEq.h"

#include <QComboBox>
#include <QEvent>
#include <QLineEdit>
#include <QList>
#include <QPointer>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>

#include <memory>
#include <utility>

namespace esa {

using AetherSDR::ClientEq;
using AetherSDR::ClientEqEditorCanvas;
using AetherSDR::StripEqPanel;

namespace {

const QString kStage = QStringLiteral("eq");

StripEqPanel* eqPanel(QWidget* panel)
{
    return static_cast<StripEqPanel*>(panel);
}

// What the page edits: the HP/LP family from its combo, and every band's on/off, frequency,
// gain, Q, type and slope from the curve, the type icons, the band fields and Reset, for each
// of the ten bands. The band count is not a setting here: it stays at ten, and the one thing
// on the page that sets it, Reset, sets ten. The EQ's on/off is the rail's, and its output
// gain is not on the receive page at all.
QStringList ownedKeys()
{
    QStringList keys{QStringLiteral("eq.family")};
    for (int band = 0; band < ClientEq::kDefaultBandCount; ++band) {
        for (const char* leaf : {"on", "freq", "gain", "q", "type", "slope"}) {
            keys << QStringLiteral("eq.b%1.%2").arg(band).arg(QLatin1String(leaf));
        }
    }
    return keys;
}

// A drag on the curve changes its band at every mouse move, but upstream saves only once the
// button is let go, so on its own the audio would wait for the release. This takes the band
// into the settings, and so to the engine, as the drag goes: at once for the first change,
// then at most once per step for the rest, so a fast mouse does not queue a change per pixel.
// The save on release then finds the last value already taken and sends nothing more.
class DragFollower : public QObject {
public:
    static constexpr int kStepMs = 33;

    DragFollower(ClientEqEditorCanvas* canvas, StageBinding* binding)
        : QObject(canvas), m_binding(binding)
    {
        m_step.setSingleShot(true);
        m_step.setInterval(kStepMs);
        connect(&m_step, &QTimer::timeout, this, [this] {
            if (std::exchange(m_changed, false)) take();
        });
        connect(canvas, &AetherSDR::ClientEqCurveWidget::bandsChanged, this, [this] {
            if (m_step.isActive()) {
                m_changed = true;
            } else {
                take();
            }
        });
    }

private:
    void take()
    {
        m_binding->pull(kStage);
        m_step.start();
    }

    StageBinding* m_binding;
    QTimer m_step;
    bool m_changed{false};
};

// The band row under the curve throws its columns away and builds new ones whenever a band
// changes, and its fields with them, so they cannot be guarded once and for all. This guards
// each column's fields as the column is built, before anyone can click one: a column cannot
// take the keyboard until it is shown, and it is polished as it is shown.
class BandFieldGuard : public QObject {
public:
    explicit BandFieldGuard(QWidget* row)
        : QObject(row), m_row(row), m_guard(new ValueFieldGuard(row))
    {
        row->installEventFilter(this);
        watchFields();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == m_row) {
            if (event->type() == QEvent::ChildPolished) {
                watchFields();
            } else if (event->type() == QEvent::ChildAdded && !m_queued) {
                // A column is added before its constructor has made its fields: they are
                // there once the event loop runs again.
                m_queued = true;
                QTimer::singleShot(0, this, [this] {
                    m_queued = false;
                    watchFields();
                });
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void watchFields()
    {
        // watch() takes each field once, however often it is asked.
        for (QLineEdit* field : m_row->findChildren<QLineEdit*>()) m_guard->watch(field);
    }

    QWidget* m_row;
    ValueFieldGuard* m_guard;
    bool m_queued{false};
};

// A value set anywhere but on the page (the exact-values window) reaches the mirror the
// page draws from, but upstream's EQ panel has no timer that rereads it, as its other panels
// do: it redraws only after its own gestures. This redraws it once the change is in: the
// band readings, the type icons, the curve and the family combo. The rows are never rebuilt,
// so a field being typed in keeps the keyboard. A change the page made itself comes back here
// too, as one redraw per event-loop pass however many keys it moved; refreshing the readings
// restyles each band's column, a few style sheets apiece.
class PageRedraw : public QObject {
public:
    PageRedraw(QWidget* panel, const ClientEq* eq, const QList<ParamControl*>& controls)
        : QObject(panel), m_panel(panel), m_eq(eq)
    {
        for (QComboBox* combo : panel->findChildren<QComboBox*>()) {
            if (combo->findText(QStringLiteral("Butterworth")) >= 0) m_family = combo;
        }
        for (ParamControl* control : controls) {
            connect(control, &ParamControl::valueChanged, this, [this] {
                if (m_queued) return;
                m_queued = true;
                QTimer::singleShot(0, this, [this] {
                    m_queued = false;
                    redraw();
                });
            });
        }
    }

private:
    void redraw()
    {
        if (auto* row = m_panel->findChild<AetherSDR::ClientEqParamRow*>()) row->refreshValues();
        if (auto* icons = m_panel->findChild<AetherSDR::ClientEqIconRow*>()) icons->update();
        if (auto* canvas = m_panel->findChild<ClientEqEditorCanvas*>()) canvas->update();
        if (m_family) {
            const QSignalBlocker blocker(m_family);
            m_family->setCurrentIndex(m_family->findData(int(m_eq->filterFamily())));
        }
    }

    QWidget* m_panel;
    const ClientEq* m_eq;
    QComboBox* m_family{nullptr};
    bool m_queued{false};
};

} // namespace

Page* makeEqPage(PageContext& context)
{
    StageSpec spec;
    spec.stage = kStage;
    spec.make = [](AetherSDR::AudioEngine* engine) -> QWidget* { return new StripEqPanel(engine); };
    // Receive only. Upstream forces the master gain to unity here and saves it; the master is
    // not a key this page owns, so the save puts the settings' value back. Nothing feeds it
    // the radio's filter, so it keeps its row of filter widths hidden and draws no edges.
    spec.showForRx = [](QWidget* panel) { eqPanel(panel)->showForPath(AetherSDR::ClientEqApplet::Path::Rx); };
    spec.sync = [](QWidget* panel) { eqPanel(panel)->refreshFromEngine(); };
    spec.owned = ownedKeys();
    spec.correct = [taps = context.taps, aether = context.aether, binding = context.binding,
                    details = context.details](QWidget* panel) {
        if (auto* canvas = panel->findChild<ClientEqEditorCanvas*>(QStringLiteral("stripRxEqCanvas"))) {
            new DragFollower(canvas, binding);
        }
        if (auto* row = panel->findChild<AetherSDR::ClientEqParamRow*>()) new BandFieldGuard(row);
        if (DetailsWindow* exact = details(kStage)) new PageRedraw(panel, &taps->view.eq, exact->controls());

        // The analyser and the OUT meter: post-EQ samples from the engine while the page shows.
        auto* feed = new EqFeed(taps, panel);
        feed->follow(panel);
        AetherSDR::AudioEngine::Hooks hooks = aether->hooks();
        hooks.copyEqSamples = [feed = QPointer<EqFeed>(feed)](float* out, int count) {
            return feed && feed->copyRecent(out, count);
        };
        aether->setHooks(std::move(hooks));
    };
    return new StageHost(context, spec);
}

} // namespace esa
