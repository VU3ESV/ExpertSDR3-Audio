#pragma once

#include "dsp/NrProcessor.h"
#include "gui/pages/Pages.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

class QCheckBox;
class QHideEvent;
class QLabel;
class QPushButton;
class QShowEvent;
class QStackedWidget;
class QTimer;

namespace AetherSDR {
class NrGainStrip;
}

namespace esa {

class ParamControl;

// AetherNR, built from the NR method table: AetherSDR's strip of seven method tabs, the
// parameters of one method in canon frames, automatic bypass, the NR status line, and the
// status strip with the live NR GAIN trace.
//
// The tabs work as AetherSDR's do. A click on an unlit tab runs that method; a click on the
// lit one turns NR off. With NR off no tab is lit and the page keeps showing the method the
// AetherNR box would bring back. A method this build or this computer cannot run keeps its
// tab, greyed, saying why.
class NrPage : public Page {
    Q_OBJECT

public:
    explicit NrPage(PageContext& context);

    void showStatus(const EngineStatus& status) override;
    void showChain() override;
    // NR now runs `id`, or is off for an empty id: the tabs show it, and the page shows that
    // method's parameters, or while NR is off those of the method the AetherNR box brings back.
    void showMethod(const QString& id);

    // The method whose parameters are on show.
    QString shownMethod() const { return m_shown; }

    // AetherSDR's methods in the order of its tabs, as this app's ids. Some are never in this
    // build (BNR) or not on every computer; they still have a tab.
    static QStringList tabIds();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QWidget* buildTabs();
    QWidget* buildMethod(const NrMethod& method);
    QWidget* buildStatusStrip();
    void resetShownMethod();
    void showStrip();
    void feedGain();

    QHash<QString, QPushButton*> m_tabs;  // by method id
    QStackedWidget* m_methods{nullptr};
    QHash<QString, QWidget*> m_methodPages;
    QHash<QString, QList<ParamControl*>> m_controls;  // each method's, in table order
    QCheckBox* m_autoBypass{nullptr};
    QLabel* m_status{nullptr};
    QLabel* m_dot{nullptr};
    QLabel* m_running{nullptr};
    AetherSDR::NrGainStrip* m_gain{nullptr};
    // Moves the engine's latest NR gain into the trace, on the panels' cadence, only while the
    // page is on screen.
    QTimer* m_feed{nullptr};
    QString m_method;  // the method NR runs; empty when off
    QString m_shown;
    QString m_dotTone;
    // What the strip's dot says about a running method, from the latest status.
    bool m_failed{false};
    bool m_held{false};  // preparing, bypassed for the mode, or BYPASS
};

} // namespace esa
