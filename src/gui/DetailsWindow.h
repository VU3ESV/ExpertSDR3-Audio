#pragma once

#include "dsp/ParamSpec.h"

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class QGridLayout;

namespace esa {

class ParamControl;

// A stage's "exact values" window: that stage's ParamControls in today's slider and spin box
// rows, for what its AetherRX page shows only approximately or not at all. A secondary window,
// so it is non-modal, sits on the canon ground, and Escape closes it.
//
// The window keeps one per stage for its whole life, hidden until asked for, so every control
// exists from the start. None is ever empty: every stage has at least its on/off, which its
// page leaves to the rail.
class DetailsWindow : public QDialog {
    Q_OBJECT

public:
    DetailsWindow(const QString& stage, const QString& title, QWidget* parent);

    QString stage() const { return m_stage; }

    // The keys of `stage` its AetherRX page never shows, or shows only roughly (the gate's
    // look-ahead, which the page rounds to the nearest of five stops).
    static QStringList keysNotOnPage(const QString& stage);

    // Makes a control for each of the stage's keys with `add`, in table order: first its
    // on/off, under "On the rail", then those the page shows, under "On the page", then
    // those it does not, under "Not on the page".
    using AddParam = std::function<ParamControl*(const ParamSpec&, QGridLayout*, int row)>;
    void populate(const AddParam& add);
    const QList<ParamControl*>& controls() const { return m_controls; }
    bool hasControls() const { return !m_controls.isEmpty(); }

    // Where the stage's ParamControls go, laid out as ParamControl lays a row.
    QGridLayout* grid() const { return m_grid; }
    // The first row nothing uses yet.
    int nextRow() const;
    // A subheading ("On the rail", "On the page", "Not on the page") on the next row.
    void addHeading(const QString& text);

    // Shows it, or brings it forward if it is open already.
    void present();

private:
    QString m_stage;
    QGridLayout* m_grid{nullptr};
    QList<ParamControl*> m_controls;
};

} // namespace esa
