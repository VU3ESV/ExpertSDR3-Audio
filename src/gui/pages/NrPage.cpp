#include "gui/pages/NrPage.h"

#include "app/LiveTaps.h"
#include "gui/ParamWidgets.h"
#include "gui/shell/StatusChip.h"

#include "CanonIndicators.h"
#include "ModemChrome.h"
#include "NrGainStrip.h"
#include "PanelTick.h"
#include "core/ThemeManager.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>

namespace esa {

namespace {

// AetherSDR's canon chrome, as its own AetherNR page wears it, and what this page adds to it:
// a combo arrow, which that sheet leaves to the caller, smaller combo boxes than its 14 px
// fields, and slider rows at the size AetherSDR's NR rows use.
QString pageSheet()
{
    namespace MC = AetherSDR::ModemChrome;
    return MC::styleSheet(MC::Scale::Dialog, MC::Look::Canon) + QStringLiteral(R"(
QComboBox { padding: 4px 10px; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox::down-arrow {
    width: 0;
    height: 0;
    border-top: 5px solid {{color.canon.inkSoft}};
    border-left: 4px solid {{color.canon.control}};
    border-right: 4px solid {{color.canon.control}};
    border-bottom: 0px solid {{color.canon.control}};
}
QComboBox::down-arrow:disabled { border-top-color: {{color.text.disabled}}; }
QLabel#ParamLabel { color: {{color.text.secondary}}; font-size: 11px; }
QLabel#ParamLabel:disabled { color: {{color.text.disabled}}; }
/* As tall as the handle, so the rows sit as close as AetherSDR's. */
QSlider:horizontal { max-height: 16px; }
/* A slider's reading looks like text, as AetherSDR's does, and is still a field: click it to
   type an exact value. */
QDoubleSpinBox {
    color: {{color.text.primary}};
    background: transparent;
    border: 1px solid transparent;
    border-radius: 4px;
    padding: 0px 2px;
    font-size: 11px;
    max-height: 14px;
}
QDoubleSpinBox:hover { border-color: {{color.canon.line}}; }
QDoubleSpinBox:focus {
    background: {{color.canon.control}};
    border-color: {{color.canon.cyan}};
}
QDoubleSpinBox:disabled { color: {{color.text.disabled}}; }
QLabel#NrAbout { color: {{color.text.label}}; font-size: 12px; }
QPushButton#nrReset { padding: 3px 12px; font-size: 12px; }
)");
}

QFrame* canonFrame(const QString& name)
{
    auto* frame = new QFrame;
    frame->setObjectName(name);
    // A styled QFrame paints its background from the palette unless told otherwise, and the
    // sheet's gradient would never show.
    frame->setAttribute(Qt::WA_StyledBackground, true);
    return frame;
}

QLabel* sectionLabel(const QString& text)
{
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("SectionLabel"));
    return label;
}

// The strip's reading of what runs. At the window's narrowest the whole name does not fit
// beside the trace, so it ends in "…" there rather than being cut off; wherever it fits it is
// drawn exactly as a QLabel draws it.
class ElidedLabel final : public QLabel {
public:
    using QLabel::QLabel;

    // Room for a few letters and the "…".
    QSize minimumSizeHint() const override
    {
        const QSize full = QLabel::minimumSizeHint();
        return {std::min(full.width(), fontMetrics().horizontalAdvance(QStringLiteral("No m…"))), full.height()};
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        const QRect room = contentsRect();
        if (fontMetrics().horizontalAdvance(text()) <= room.width()) {
            QLabel::paintEvent(event);
            return;
        }
        QPainter painter(this);
        style()->drawItemText(&painter, room, int(alignment()), palette(), isEnabled(),
                              fontMetrics().elidedText(text(), Qt::ElideRight, room.width()), foregroundRole());
    }
};

// A slider's spin box as a reading: no steppers, right-aligned, only as wide as its numbers.
void tidyReading(ParamControl* control)
{
    for (QWidget* widget : control->widgets()) {
        if (auto* spin = qobject_cast<QDoubleSpinBox*>(widget)) {
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            spin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            spin->setMinimumWidth(48);
        }
    }
}

// One heading's parameters, as the page lays them out.
struct Section {
    QString heading;
    QList<const ParamSpec*> specs;
    // AetherSDR's order on its NR pages: pickers first, side by side, then switches, then
    // sliders. 0, 1 and 2, in that order.
    int kind{2};
};

QList<Section> sectionsOf(const NrMethod& method)
{
    QList<Section> sections;
    for (const ParamSpec& spec : method.params) {
        auto it = std::find_if(sections.begin(), sections.end(),
                               [&](const Section& s) { return s.heading == spec.section; });
        if (it == sections.end()) {
            sections.append(Section{spec.section, {}, 2});
            it = sections.end() - 1;
        }
        it->specs.append(&spec);
    }
    for (Section& section : sections) {
        const auto all = [&](auto test) { return std::all_of(section.specs.begin(), section.specs.end(), test); };
        if (all([](const ParamSpec* s) { return !s->choices.isEmpty(); })) {
            section.kind = 0;
        } else if (all([](const ParamSpec* s) { return s->toggle; })) {
            section.kind = 1;
        }
    }
    std::stable_sort(sections.begin(), sections.end(),
                     [](const Section& a, const Section& b) { return a.kind < b.kind; });
    return sections;
}

} // namespace

QStringList NrPage::tabIds()
{
    QStringList ids{QStringLiteral("nr2"), QStringLiteral("nr4"), QStringLiteral("mnr"), QStringLiteral("dfnr"),
                    QStringLiteral("rn2"), QStringLiteral("bnr"), QStringLiteral("nnr")};
    // A method AetherSDR does not have would otherwise be out of reach.
    for (const NrMethod& method : nrMethods()) {
        if (!ids.contains(method.id)) ids.append(method.id);
    }
    return ids;
}

NrPage::NrPage(PageContext& ctx)
    : Page(ctx)
{
    // As AetherSDR's AetherNR page: tabs, the method's page, the status strip at the foot.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(6);
    layout->addWidget(buildTabs());

    m_methods = new QStackedWidget;
    for (const NrMethod& method : nrMethods()) {
        QWidget* page = buildMethod(method);
        m_methodPages.insert(method.id, page);
        m_methods->addWidget(page);
    }
    layout->addWidget(m_methods, 1);

    m_autoBypass = new QCheckBox(QStringLiteral("Bypass NR automatically on CW and digital modes"));
    m_autoBypass->setObjectName(QStringLiteral("nrAutoBypass"));
    m_autoBypass->setChecked(config().nrAutoBypass);
    m_autoBypass->setToolTip(QStringLiteral(
        "These are not speech. A speech model treats a steady carrier as noise and can remove the signal itself."));
    connect(m_autoBypass, &QCheckBox::toggled, this, [this](bool on) {
        config().nrAutoBypass = on;
        context().post([on](Engine* engine) { engine->setNrAutoBypass(on); });
        context().chainChanged();
    });
    m_status = hintLabel();
    m_status->setObjectName(QStringLiteral("nrStatus"));
    m_status->hide();  // until there is something to say
    auto* foot = new QVBoxLayout;
    foot->setContentsMargins(12, 0, 12, 0);
    foot->setSpacing(4);
    foot->addWidget(m_autoBypass);
    foot->addWidget(m_status);
    layout->addLayout(foot);

    layout->addWidget(buildStatusStrip());

    AetherSDR::applyCanonSheet(this, pageSheet);

    m_feed = new QTimer(this);
    m_feed->setObjectName(QStringLiteral("nrGainFeed"));
    m_feed->setInterval(AetherSDR::kPanelTickMs);
    connect(m_feed, &QTimer::timeout, this, &NrPage::feedGain);

    showMethod(config().nrMethod);
}

QWidget* NrPage::buildTabs()
{
    QFrame* frame = canonFrame(QStringLiteral("TabsFrame"));
    auto* row = new QHBoxLayout(frame);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    for (const QString& id : tabIds()) {
        const QString name = id.toUpper();
        auto* tab = new QPushButton(name);
        tab->setObjectName(QStringLiteral("nrTab_") + id);
        // ModemChrome's method tab, lit while checked.
        tab->setProperty("chrome", "tab");
        tab->setCheckable(true);
        tab->setFlat(true);
        // The tabs share the row evenly, whatever their labels.
        tab->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        tab->setMinimumWidth(0);
        tab->setAccessibleName(QStringLiteral("%1 noise-reduction method").arg(name));
        if (const NrMethod* method = findNrMethod(id)) {
            tab->setToolTip(QStringLiteral("%1\n%2").arg(method->label, method->description));
            // The lit tab is the method running: lighting another runs that one instead, and
            // putting the lit one out turns NR off.
            connect(tab, &QPushButton::clicked, this, [this, id](bool lit) {
                context().selectNrMethod(lit ? id : QString());
            });
        } else {
            const QString reason = nrMethodUnavailableReason(id);
            tab->setEnabled(false);
            tab->setToolTip(reason);
            tab->setAccessibleDescription(reason);
        }
        m_tabs.insert(id, tab);
        row->addWidget(tab, 1);
    }
    return frame;
}

QWidget* NrPage::buildMethod(const NrMethod& method)
{
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("nrMethod_") + method.id);
    auto* column = new QVBoxLayout(content);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(6);

    QList<ParamControl*>& controls = m_controls[method.id];
    const auto addRows = [&](QGridLayout* grid, const Section& section) {
        int row = 0;
        for (const ParamSpec* spec : section.specs) {
            ParamControl* control = context().addParam(*spec, grid, row++);
            tidyReading(control);
            controls.append(control);
        }
        grid->setColumnStretch(1, 1);
    };
    const auto newGrid = [] {
        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(6);
        return grid;
    };

    // Reset goes at the end of the first frame's top row, where AetherSDR puts its own.
    QHBoxLayout* resetRow = nullptr;

    const QList<Section> sections = sectionsOf(method);
    for (qsizetype i = 0; i < sections.size();) {
        QFrame* frame = canonFrame(QStringLiteral("ControlsFrame"));
        if (sections.at(i).kind == 0) {
            // Pickers side by side in one frame, a column each, as GAIN METHOD and NOISE
            // ESTIMATION are.
            auto* row = new QHBoxLayout(frame);
            row->setContentsMargins(12, 10, 12, 10);
            row->setSpacing(16);
            qsizetype end = i;
            while (end < sections.size() && sections.at(end).kind == 0) ++end;
            for (qsizetype k = i; k < end; ++k) {
                const Section& section = sections.at(k);
                const bool last = k + 1 == end;
                QFrame* cell = canonFrame(last ? QStringLiteral("ControlCellLast") : QStringLiteral("ControlCell"));
                auto* box = new QVBoxLayout(cell);
                box->setContentsMargins(0, 0, last ? 0 : 16, 0);
                box->setSpacing(8);
                if (!section.heading.isEmpty()) box->addWidget(sectionLabel(section.heading));
                QGridLayout* grid = newGrid();
                if (section.specs.size() == 1) {
                    // The heading names it.
                    controls.append(context().addParamCell(*section.specs.first(), grid, 0, 0));
                } else {
                    addRows(grid, section);
                }
                box->addLayout(grid);
                box->addStretch(1);
                row->addWidget(cell);
            }
            row->addStretch(1);
            if (!resetRow) resetRow = row;
            i = end;
        } else {
            const Section& section = sections.at(i);
            auto* box = new QVBoxLayout(frame);
            box->setContentsMargins(12, 10, 12, 10);
            box->setSpacing(8);
            auto* header = new QHBoxLayout;
            header->setContentsMargins(0, 0, 0, 0);
            if (!section.heading.isEmpty()) header->addWidget(sectionLabel(section.heading));
            header->addStretch(1);
            box->addLayout(header);
            QGridLayout* grid = newGrid();
            if (section.kind == 1) {
                // Switches side by side on one row, as POST PROCESSING's are.
                grid->setHorizontalSpacing(20);
                int col = 0;
                for (const ParamSpec* spec : section.specs) {
                    controls.append(context().addParamCell(*spec, grid, 0, col++));
                }
                grid->setColumnStretch(col, 1);
            } else {
                addRows(grid, section);
            }
            box->addLayout(grid);
            if (!resetRow) resetRow = header;
            ++i;
        }
        column->addWidget(frame);
    }
    if (resetRow) {
        auto* reset = new QPushButton(QStringLiteral("Reset"));
        reset->setObjectName(QStringLiteral("nrReset"));
        reset->setToolTip(QStringLiteral("Put every %1 setting back to its default.").arg(method.id.toUpper()));
        reset->setAccessibleName(QStringLiteral("Reset %1 to defaults").arg(method.id.toUpper()));
        connect(reset, &QPushButton::clicked, this, &NrPage::resetShownMethod);
        resetRow->addWidget(reset, 0, Qt::AlignVCenter);
    }

    followEnabledBy(controls);

    auto* about = new QLabel(method.description);
    about->setObjectName(QStringLiteral("NrAbout"));
    about->setWordWrap(true);
    about->setContentsMargins(12, 2, 12, 0);
    column->addWidget(about);
    column->addStretch(1);
    return scrollable(content);
}

QWidget* NrPage::buildStatusStrip()
{
    // AetherSDR's status strip, sized as its own: what runs on the left, and on the right the
    // trace of how much it is taking out.
    QFrame* frame = canonFrame(QStringLiteral("StatusFrame"));
    auto* row = new QHBoxLayout(frame);
    row->setContentsMargins(10, 5, 10, 5);
    row->setSpacing(8);

    m_dot = new QLabel;
    m_dot->setObjectName(QStringLiteral("StatusDot"));
    row->addWidget(m_dot);
    row->addWidget(sectionLabel(QStringLiteral("METHOD")));
    m_running = new ElidedLabel;
    m_running->setObjectName(QStringLiteral("StatusValue"));
    // The trace says nothing to a screen reader; the reading has to.
    m_running->setAccessibleName(QStringLiteral("Noise reduction status"));
    row->addWidget(m_running);
    row->addStretch(1);

    row->addWidget(sectionLabel(QStringLiteral("NR GAIN")));
    m_gain = new AetherSDR::NrGainStrip(frame);
    m_gain->setObjectName(QStringLiteral("nrGainStrip"));
    m_gain->setMinimumHeight(18);
    m_gain->setMaximumHeight(20);
    m_gain->setMinimumWidth(120);
    row->addWidget(m_gain, 2);
    return frame;
}

void NrPage::showMethod(const QString& id)
{
    const QString running = findNrMethod(id) ? id : QString();
    if (running != m_method) {
        // The old method's trace says nothing about the new one.
        m_gain->reset();
        m_method = running;
    }
    for (auto it = m_tabs.cbegin(); it != m_tabs.cend(); ++it) {
        const QSignalBlocker block(it.value());
        it.value()->setChecked(it.key() == m_method);
    }

    QString shown = m_method;
    if (shown.isEmpty() && context().lastNrMethod) shown = context().lastNrMethod();
    if (!m_methodPages.contains(shown)) shown = nrMethods().constFirst().id;
    m_shown = shown;
    m_methods->setCurrentWidget(m_methodPages.value(shown));
    showStrip();
}

void NrPage::showChain()
{
    showMethod(config().nrMethod);
    const QSignalBlocker block(m_autoBypass);
    m_autoBypass->setChecked(config().nrAutoBypass);
}

void NrPage::showStatus(const EngineStatus& s)
{
    const QString nr = statusText::nr(s);
    if (nr != m_status->text()) {
        m_status->setText(nr);
        m_status->setVisible(!nr.isEmpty());
    }
    const bool failed = s.nrFailed;
    const bool held = s.nrPreparing || s.nrBypassedByMode || s.rxBypassed;
    if (failed != m_failed || held != m_held) {
        m_failed = failed;
        m_held = held;
        showStrip();
    }
}

void NrPage::resetShownMethod()
{
    // Each through its control, so the engine, the settings and Modified all follow.
    for (ParamControl* control : m_controls.value(m_shown)) {
        if (control->value() != control->spec().def) control->setValue(control->spec().def);
    }
}

void NrPage::showStrip()
{
    // Grey is nothing running, distinct from the green of a method that runs; amber while it
    // waits or is passed by, red when it failed.
    const NrMethod* method = findNrMethod(m_method);
    QString tone = QStringLiteral("color.accent.success");
    if (!method) {
        tone = QStringLiteral("color.text.label");
    } else if (m_failed) {
        tone = QStringLiteral("color.accent.danger");
    } else if (m_held) {
        tone = QStringLiteral("color.accent.warning");
    }
    const QString text = method ? method->label : QStringLiteral("No method running");
    if (text != m_running->text()) {
        m_running->setText(text);
        m_running->setAccessibleDescription(text);
    }
    if (tone != m_dotTone) {
        m_dotTone = tone;
        AetherSDR::ThemeManager::instance().applyStyleSheet(
            m_dot, QStringLiteral("QLabel#StatusDot { background: {{%1}}; border-radius: 6px; min-width: 12px;"
                                  " max-width: 12px; min-height: 12px; max-height: 12px; }")
                       .arg(tone));
    }
}

void NrPage::feedGain()
{
    if (!context().taps) return;
    const LiveTaps& taps = *context().taps;
    m_gain->setGain(taps.nrGain.load(std::memory_order_relaxed), taps.nrActive.load(std::memory_order_relaxed));
}

void NrPage::showEvent(QShowEvent* event)
{
    Page::showEvent(event);
    // The dot's colour is resolved in the theme scope of the window the page is in by now,
    // which it was not in when built.
    m_dotTone.clear();
    showStrip();
    feedGain();
    m_feed->start();
}

void NrPage::hideEvent(QHideEvent* event)
{
    m_feed->stop();
    Page::hideEvent(event);
}

} // namespace esa
