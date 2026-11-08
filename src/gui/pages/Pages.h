#pragma once

#include "app/Engine.h"
#include "app/Settings.h"

#include <QList>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>

class QGridLayout;
class QLabel;

namespace esa {

class ParamControl;
struct PageContext;

// One page of the window's stack. The window tells every page what the engine reports and
// when the chain changes under it; a page shows what concerns it and ignores the rest.
class Page : public QWidget {
    Q_OBJECT

public:
    explicit Page(PageContext& context, QWidget* parent = nullptr);

    // Ten times a second, and after each change the engine makes.
    virtual void showStatus(const EngineStatus& status);
    // The chain settings were replaced (a profile load): show them again.
    virtual void showChain();

protected:
    PageContext& context() const { return m_context; }
    AppConfig& config() const;

private:
    PageContext& m_context;
};

// What a page may ask of the window that hosts it. The window fills it in before building
// the first page and keeps it until the last one is gone.
struct PageContext {
    AppConfig* config{nullptr};
    // What the engine shares without locks: levels and samples for the page on screen, which
    // reads them from its own timer while it is visible. Held, not borrowed, because the
    // engine is deleted before the window.
    std::shared_ptr<LiveTaps> taps;
    // The control for one parameter, registered with the window so the engine, profiles and
    // Modified all follow it: a row of `grid`, as ParamControl lays one out.
    std::function<ParamControl*(const ParamSpec&, QGridLayout*, int row)> addParam;
    // The same in its compact form, in one cell of a table.
    std::function<ParamControl*(const ParamSpec&, QGridLayout*, int row, int column)> addParamCell;
    // Queues a call on the engine's thread.
    std::function<void(std::function<void(Engine*)>)> post;
    // The chain changed other than through a ParamControl; Modified and the rail follow.
    std::function<void()> chainChanged;
    // Turns a noise-reduction method on, or NR off with an empty id.
    std::function<void(const QString& id)> selectNrMethod;
    // Opens Find Radios.
    std::function<void()> findRadios;
};

// The rail's ids for its rows, as StageTabBar hands them back. AetherNR to Final Output
// number as upstream's AetherRxDialog does; Radio is ours, above them.
enum RailId : int {
    kRailNr = 0,
    kRailGate = 1,
    kRailEq = 2,
    kRailComp = 3,
    kRailTube = 4,
    kRailExciter = 5,
    kRailOutput = 6,
    kRailRadio = 100,
};

// One row of the rail and the page it opens. A page that changes kind (the vendored stage
// pages replacing today's grids) changes only its `make` here.
struct PageEntry {
    QString id;     // for --page and ui/page
    int railId;
    QString label;  // the rail's label
    QString stage;  // the chain stage it shows ("gate"), empty for Radio, AetherNR and Final Output
    bool checkbox;  // an on/off box on the rail
    std::function<Page*(PageContext&)> make;
};

// Every page, in the rail's order top to bottom. The five chain stages are then laid out in
// the chain's own order by the rail itself.
const QList<PageEntry>& pageTable();
const PageEntry* findPage(const QString& id);

// Today's controls in the canon look, for the interim pages that still show them. Applied to
// the page itself, so it outranks the canon body's "everything is transparent" rule there
// and reaches nothing outside it: not the rail, the header, or a vendored page.
QString interimPageSheet();

// The same look for the dialogs the window opens (profile names, Find Radios, About, message
// boxes): scoped to QDialog, so the window can carry it without it reaching the rail.
QString dialogSheet();

// A wrapping rich-text label, for status and description lines.
QLabel* hintLabel(const QString& text = QString());

// Wraps a page's content so a short window scrolls instead of squashing rows.
QWidget* scrollable(QWidget* content);

} // namespace esa
