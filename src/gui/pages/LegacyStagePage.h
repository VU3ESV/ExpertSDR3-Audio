#pragma once

#include "gui/pages/Pages.h"

namespace esa {

// Interim: today's controls for one chain stage, until its AetherRX page replaces it, and
// then deleted. Every parameter of the stage gets its ParamControl here, in the stage's
// table order: a slider and spin box row each, except the EQ's bands, which keep a
// compact table, two lines a band, narrow enough for the page column.
class LegacyStagePage : public Page {
    Q_OBJECT

public:
    LegacyStagePage(PageContext& context, const QString& stage);

    QString stage() const { return m_stage; }

    void showChain() override;

private:
    QWidget* buildGrid();
    QWidget* buildEqTable();
    // Greys out the gain of a band that is a high- or low-pass.
    void showBandGains();

    QString m_stage;
    QList<ParamControl*> m_bandGains;  // the EQ's, by band
};

} // namespace esa
