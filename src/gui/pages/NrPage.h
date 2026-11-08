#pragma once

#include "gui/pages/Pages.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QStackedWidget;

namespace esa {

// AetherNR. Interim: today's method list, description, parameters, auto-bypass and status
// line, until the page built from the NR method table replaces it.
class NrPage : public Page {
    Q_OBJECT

public:
    explicit NrPage(PageContext& context);

    void showStatus(const EngineStatus& s) override;
    void showChain() override;
    // Shows `id`'s description and parameters, without selecting anything.
    void showMethod(const QString& id);

private:
    QComboBox* m_method{nullptr};
    QLabel* m_description{nullptr};
    QStackedWidget* m_params{nullptr};
    QCheckBox* m_autoBypass{nullptr};
    QLabel* m_status{nullptr};
};

} // namespace esa
