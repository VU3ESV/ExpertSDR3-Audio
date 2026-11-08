#include "gui/pages/Pages.h"

#include "dsp/RxChain.h"
#include "gui/pages/LegacyStagePage.h"
#include "gui/pages/NrPage.h"
#include "gui/pages/OutputPage.h"
#include "gui/pages/RadioPage.h"
#include "gui/pages/StageHost.h"

#include <QFrame>
#include <QLabel>
#include <QScrollArea>

#include <algorithm>

namespace esa {

Page::Page(PageContext& context, QWidget* parent)
    : QWidget(parent), m_context(context)
{
}

void Page::showStatus(const EngineStatus&) {}

void Page::showChain() {}

void Page::attach() {}

AppConfig& Page::config() const
{
    return *m_context.config;
}

const QList<PageEntry>& pageTable()
{
    static const QList<PageEntry> table = [] {
        const auto legacy = [](const QString& stage) {
            return [stage](PageContext& c) -> Page* { return new LegacyStagePage(c, stage); };
        };
        const auto stageRow = [&](const QString& id, int railId, const QString& stage) {
            return PageEntry{id, railId, RxChain::stageLabel(stage), stage, true, legacy(stage)};
        };
        return QList<PageEntry>{
            {QStringLiteral("radio"), kRailRadio, QStringLiteral("Radio"), QString(), false,
             [](PageContext& c) -> Page* { return new RadioPage(c); }},
            {QStringLiteral("nr"), kRailNr, QStringLiteral("AetherNR"), QString(), true,
             [](PageContext& c) -> Page* { return new NrPage(c); }},
            {QStringLiteral("gate"), kRailGate, RxChain::stageLabel(QStringLiteral("gate")), QStringLiteral("gate"), true,
             makeGatePage},
            stageRow(QStringLiteral("eq"), kRailEq, QStringLiteral("eq")),
            stageRow(QStringLiteral("comp"), kRailComp, QStringLiteral("comp")),
            stageRow(QStringLiteral("tube"), kRailTube, QStringLiteral("tube")),
            stageRow(QStringLiteral("exciter"), kRailExciter, QStringLiteral("pudu")),
            {QStringLiteral("output"), kRailOutput, QStringLiteral("Final Output"), QString(), false,
             [](PageContext& c) -> Page* { return new OutputPage(c); }},
        };
    }();
    return table;
}

const PageEntry* findPage(const QString& id)
{
    const auto& table = pageTable();
    const auto it = std::find_if(table.begin(), table.end(), [&](const PageEntry& e) { return e.id == id; });
    return it == table.end() ? nullptr : &*it;
}

namespace {

// The rules today's controls share wherever they appear, with @scope@ ahead of every
// selector. Colours are tokens, resolved in the canon/aetherRx scope.
QString sharedControlRules()
{
    return QStringLiteral(R"(
@scope@QWidget:disabled { color: {{color.button.foreground.disabled}}; }

@scope@QLineEdit, @scope@QAbstractSpinBox, @scope@QComboBox {
    color: {{color.canon.ink}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 5px;
    padding: 2px 6px;
    selection-color: {{color.canon.onAccent}};
    selection-background-color: {{color.canon.cyan}};
}
/* The editor inside a spin box is a line edit too: it must not draw a second field. */
@scope@QAbstractSpinBox QLineEdit, @scope@QComboBox QLineEdit {
    border: none;
    background: transparent;
    padding: 0;
    margin: 0;
}
@scope@QLineEdit:focus, @scope@QAbstractSpinBox:focus, @scope@QComboBox:focus {
    border-color: {{color.canon.cyan}};
}
@scope@QLineEdit:disabled, @scope@QAbstractSpinBox:disabled, @scope@QComboBox:disabled {
    color: {{color.button.foreground.disabled}};
    background: {{color.canon.raised}};
    border-color: {{color.canon.line}};
}
@scope@QComboBox::drop-down { border: none; width: 18px; }
/* Once a sheet styles the drop-down, Qt draws no arrow. A border triangle draws one in
   the theme's tokens, with no image: side borders in the field's own colour mitre the
   top border into a point. */
@scope@QComboBox::down-arrow {
    width: 0;
    height: 0;
    border-top: 5px solid {{color.canon.inkSoft}};
    border-left: 4px solid {{color.canon.control}};
    border-right: 4px solid {{color.canon.control}};
    border-bottom: 0px solid {{color.canon.control}};
}
@scope@QComboBox::down-arrow:disabled {
    border-top-color: {{color.button.foreground.disabled}};
    border-left-color: {{color.canon.raised}};
    border-right-color: {{color.canon.raised}};
    border-bottom-color: {{color.canon.raised}};
}

@scope@QPushButton {
    color: {{color.canon.ink}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 5px;
    padding: 4px 12px;
}
@scope@QPushButton:hover { border-color: {{color.canon.aqua}}; }
@scope@QPushButton:pressed {
    background: {{color.canon.nested}};
    border-color: {{color.canon.cyan}};
}
/* Before :disabled, which ties with it and must win: a latched button that cannot be
   used reads as unusable first. */
@scope@QPushButton:checked {
    color: {{color.canon.cyan}};
    background: {{color.canon.nested}};
    border-color: {{color.canon.cyan}};
}
@scope@QPushButton:disabled {
    color: {{color.button.foreground.disabled}};
    background: transparent;
    border-color: {{color.canon.line}};
}

/* Drawn here rather than by Fusion: on the canon ground Fusion would take the box's fill
   and edge from a transparent palette and draw nothing for an unticked box. The colours
   are AetherSDR's canon check box (CanonIndicators.h); the tick is an image, because a
   sheet cannot draw one. */
@scope@QCheckBox { spacing: 6px; }
@scope@QCheckBox::indicator {
    width: 14px;
    height: 14px;
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 4px;
    background: {{color.canon.control}};
}
@scope@QCheckBox::indicator:hover { border-color: {{color.canon.aqua}}; }
@scope@QCheckBox::indicator:checked {
    border-color: {{color.canon.aqua}};
    background: {{color.canon.cyan}};
    image: url(:/canon/check-tick.png);
}
@scope@QCheckBox::indicator:disabled { border-color: {{color.canon.line}}; }
@scope@QCheckBox::indicator:checked:disabled {
    border-color: {{color.canon.muted}};
    background: {{color.canon.muted}};
    image: url(:/canon/check-tick-disabled.png);
}
)");
}

} // namespace

QString interimPageSheet()
{
    return QString(sharedControlRules()).replace(QLatin1String("@scope@"), QString()) + QStringLiteral(R"(
QLabel, QCheckBox { color: {{color.canon.inkSoft}}; }
QLabel:disabled, QCheckBox:disabled { color: {{color.button.foreground.disabled}}; }

QGroupBox {
    background: transparent;
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 8px;
    margin-top: 10px;
    padding-top: 6px;
    font-weight: 700;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
    color: {{color.canon.ink}};
}

QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:vertical {
    background: {{color.canon.lineHi}};
    border-radius: 3px;
    min-height: 30px;
}
QScrollBar::handle:horizontal {
    background: {{color.canon.lineHi}};
    border-radius: 3px;
    min-width: 30px;
}
QScrollBar::add-line, QScrollBar::sub-line {
    width: 0;
    height: 0;
    border: none;
    background: none;
}
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QProgressBar {
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.line}};
    border-radius: 3px;
    max-height: 12px;
}
QProgressBar::chunk {
    background: {{color.accent.success}};
    border-radius: 2px;
}
)");
}

QString dialogSheet()
{
    return QString(sharedControlRules()).replace(QLatin1String("@scope@"), QStringLiteral("QDialog "));
}

QLabel* hintLabel(const QString& text)
{
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    return label;
}

QWidget* scrollable(QWidget* content)
{
    auto* scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

} // namespace esa
