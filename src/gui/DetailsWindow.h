#pragma once

#include <QDialog>
#include <QString>

class QGridLayout;

namespace esa {

// A stage's "exact values" window: that stage's ParamControls in today's slider and spin box
// rows, for what its AetherRX page shows only approximately or not at all. A secondary window,
// so it is non-modal, sits on the canon ground, and Escape closes it.
//
// A skeleton for now. The window, its look and its grid exist; it gets its controls when a
// stage's AetherRX page replaces that stage's interim page, which still shows them all.
class DetailsWindow : public QDialog {
    Q_OBJECT

public:
    DetailsWindow(const QString& stage, const QString& title, QWidget* parent);

    QString stage() const { return m_stage; }

    // Where the stage's ParamControls go, laid out as ParamControl lays a row.
    QGridLayout* grid() const { return m_grid; }
    // The first row nothing uses yet.
    int nextRow() const;
    // A subheading ("On the page", "Not on the page") on the next row.
    void addHeading(const QString& text);

    // Shows it, or brings it forward if it is open already.
    void present();

private:
    QString m_stage;
    QGridLayout* m_grid{nullptr};
};

} // namespace esa
