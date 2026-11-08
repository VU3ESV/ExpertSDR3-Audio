#include "gui/pages/LegacyStagePage.h"

#include "dsp/RxChain.h"
#include "gui/ParamWidgets.h"
#include "gui/shell/StatusChip.h"

#include "ClientEq.h"
#include "core/ThemeManager.h"

#include <QGridLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <cstring>

namespace esa {

LegacyStagePage::LegacyStagePage(PageContext& ctx, const QString& stage)
    : Page(ctx), m_stage(stage)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scrollable(stage == QLatin1String("eq") ? buildEqTable() : buildGrid()));
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, interimPageSheet());
}

QWidget* LegacyStagePage::buildGrid()
{
    auto* panel = new QWidget;
    auto* grid = new QGridLayout(panel);
    int row = 0;
    for (const ParamSpec& spec : RxChain::stageParams()) {
        if (spec.key.section(QLatin1Char('.'), 0, 0) == m_stage) context().addParam(spec, grid, row++);
    }
    grid->setRowStretch(row, 1);
    grid->setColumnStretch(1, 1);
    return panel;
}

QWidget* LegacyStagePage::buildEqTable()
{
    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);

    auto* general = new QGridLayout;
    int row = 0;
    for (const char* key : {"eq.enabled", "eq.master", "eq.family"}) {
        context().addParam(*findParamSpec(QString::fromLatin1(key)), general, row++);
    }
    general->setColumnStretch(1, 1);
    layout->addLayout(general);

    // A compact table rather than the full slider row per value: ten bands of six fields
    // each would otherwise be a very long scroll. Each band takes two lines, on, frequency,
    // gain and Q, then its filter's type and slope beneath: on one line the six would not fit
    // the page column at the window's usual size.
    using AetherSDR::ClientEq;
    auto* table = new QGridLayout;
    const auto header = [table](int row, int column, const QString& text) {
        table->addWidget(new QLabel(colored(QStringLiteral("color.text.label"), text)), row, column);
    };
    header(0, 0, QStringLiteral("Band"));
    header(0, 1, QStringLiteral("Frequency"));
    header(0, 2, QStringLiteral("Gain"));
    header(0, 3, QStringLiteral("Q"));
    header(1, 1, QStringLiteral("Type"));
    header(1, 2, QStringLiteral("Slope"));
    struct Cell {
        const char* leaf;
        int line;
        int column;
    };
    constexpr Cell kCells[] = {{"on", 0, 0}, {"freq", 0, 1}, {"gain", 0, 2}, {"q", 0, 3}, {"type", 1, 1}, {"slope", 1, 2}};
    for (int band = 0; band < ClientEq::kDefaultBandCount; ++band) {
        const int top = 2 + band * 2;
        for (const Cell& cell : kCells) {
            const ParamSpec& spec = *findParamSpec(QStringLiteral("eq.b%1.%2").arg(band).arg(QLatin1String(cell.leaf)));
            ParamControl* control = context().addParamCell(spec, table, top + cell.line, cell.column);
            if (std::strcmp(cell.leaf, "gain") == 0) m_bandGains.append(control);
            // After the window has taken the new type: this reads it back from the config.
            if (std::strcmp(cell.leaf, "type") == 0) {
                connect(control, &ParamControl::valueChanged, this, [this] { showBandGains(); });
            }
        }
    }
    showBandGains();
    for (int column = 1; column <= 3; ++column) table->setColumnStretch(column, 1);
    layout->addLayout(table);
    layout->addStretch(1);
    return panel;
}

void LegacyStagePage::showChain()
{
    showBandGains();
}

void LegacyStagePage::showBandGains()
{
    // High- and low-pass have no gain to set. The choices run HighPass, LowShelf, Peak,
    // HighShelf, LowPass.
    for (int band = 0; band < m_bandGains.size(); ++band) {
        const ParamSpec& type = *findParamSpec(QStringLiteral("eq.b%1.type").arg(band));
        const int index = int(paramValue(config(), type));
        m_bandGains.at(band)->setEnabled(index != 0 && index != int(type.choices.size()) - 1);
    }
}

} // namespace esa
