#include "gui/DetailsWindow.h"

#include "gui/aether/Appearance.h"
#include "gui/pages/Pages.h"

#include "CanonIndicators.h"
#include "ModemChrome.h"

#include <QGridLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace esa {

DetailsWindow::DetailsWindow(const QString& stage, const QString& title, QWidget* parent)
    : QDialog(parent, Qt::Tool), m_stage(stage)
{
    setObjectName(QStringLiteral("details_") + stage);
    setWindowTitle(title);
    setModal(false);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* body = new CanonBody;
    outer->addWidget(body);

    auto* content = new QWidget;
    m_grid = new QGridLayout(content);
    m_grid->setColumnStretch(1, 1);

    auto* column = new QVBoxLayout(body);
    column->setContentsMargins(14, 12, 14, 14);
    column->addWidget(scrollable(content));

    Appearance::apply(this);
    // The controls in AetherSDR's canon chrome, as its own pages wear them.
    AetherSDR::applyCanonSheet(content, [] {
        namespace MC = AetherSDR::ModemChrome;
        return MC::styleSheet(MC::Scale::Dialog, MC::Look::Canon) + AetherSDR::canonSpinArrowRules();
    });
    resize(460, 520);
}

int DetailsWindow::nextRow() const
{
    // An empty grid still reports one row.
    return m_grid->count() == 0 ? 0 : m_grid->rowCount();
}

void DetailsWindow::addHeading(const QString& text)
{
    auto* label = new QLabel(text.toUpper());
    label->setObjectName(QStringLiteral("SectionLabel"));
    m_grid->addWidget(label, nextRow(), 0, 1, -1);
}

void DetailsWindow::present()
{
    show();
    raise();
    activateWindow();
}

} // namespace esa
