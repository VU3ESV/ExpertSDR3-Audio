#include "gui/DetailsWindow.h"

#include "dsp/RxChain.h"
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
    m_note = hintLabel();
    m_note->hide();
    column->addWidget(m_note);
    column->addWidget(scrollable(content));

    Appearance::apply(this);
    // The controls in AetherSDR's canon chrome, as its own pages wear them.
    AetherSDR::applyCanonSheet(content, [] {
        namespace MC = AetherSDR::ModemChrome;
        return MC::styleSheet(MC::Scale::Dialog, MC::Look::Canon) + AetherSDR::canonSpinArrowRules();
    });
    resize(460, 520);
}

QStringList DetailsWindow::keysNotOnPage(const QString& stage)
{
    // Upstream hides these on RX, or never had a control for them. The gate's look-ahead is
    // on its page as a five-stop slider, so its exact value lives here.
    if (stage == QLatin1String("gate")) return {QStringLiteral("gate.attack"), QStringLiteral("gate.lookahead")};
    if (stage == QLatin1String("eq")) return {QStringLiteral("eq.master")};
    if (stage == QLatin1String("comp")) return {QStringLiteral("comp.drive"), QStringLiteral("comp.rotator")};
    if (stage == QLatin1String("tube")) return {QStringLiteral("tube.attack")};
    return {};
}

void DetailsWindow::populate(const AddParam& add)
{
    const QStringList hidden = keysNotOnPage(m_stage);
    // The stage's on/off is its box on the rail; its page never shows it.
    const QString enable = m_stage + QStringLiteral(".enabled");
    QList<const ParamSpec*> onRail;
    QList<const ParamSpec*> onPage;
    QList<const ParamSpec*> offPage;
    for (const ParamSpec& spec : RxChain::stageParams()) {
        if (spec.key.section(QLatin1Char('.'), 0, 0) != m_stage) continue;
        if (spec.key == enable) {
            onRail.append(&spec);
        } else {
            (hidden.contains(spec.key) ? offPage : onPage).append(&spec);
        }
    }
    const auto section = [&](const QString& heading, const QList<const ParamSpec*>& specs) {
        if (specs.isEmpty()) return;
        addHeading(heading);
        for (const ParamSpec* spec : specs) {
            if (ParamControl* control = add(*spec, m_grid, nextRow())) m_controls.append(control);
        }
    };
    section(QStringLiteral("On the rail"), onRail);
    section(QStringLiteral("On the page"), onPage);
    section(QStringLiteral("Not on the page"), offPage);
    m_grid->setRowStretch(nextRow(), 1);
}

void DetailsWindow::setNote(const QString& text)
{
    m_note->setText(text);
    m_note->setVisible(!text.isEmpty());
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
