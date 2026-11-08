#pragma once

#include <QColor>
#include <QPixmap>
#include <QString>
#include <QWidget>

namespace esa {

// The AetherRX look, taken from AetherSDR's own theme (default-dark.json, through its
// ThemeManager) rather than drawn by hand here, so every colour is a named token and the
// vendored pages and our widgets agree.
class Appearance {
public:
    // The theme scope AetherRX's window declares. The canon tokens under it win over the
    // root theme's, which is what turns AetherSDR's blue-grey into the canon navy and cyan.
    static QString container();

    // Once per process, after the QApplication exists and before any window: registers
    // the bundled Inter, sets Fusion, and gives the application a palette in the canon
    // colours for whatever is drawn outside a themed window. Calling it again does nothing.
    static void install();

    // Gives a top-level window the look: the canon/aetherRx scope, AetherSDR's application
    // sheet resolved in it, the canon body's sheet, then `extraSheet`. Templates take
    // {{token}} placeholders; the later sheet wins where two rules tie.
    static void apply(QWidget* window, const QString& extraSheet = QString());

    // A colour token as the canon/aetherRx scope sees it, for code that paints or writes
    // rich text rather than going through a style sheet.
    static QColor color(const QString& token);
};

// A window's content area on the canon ground: the fill, the two blooms, the fading grid
// and the hairline border of AetherSDR's paintCanonGround(), square-cornered for a window
// with the system frame. It is named canonBody, because canonBodyStyleSheet() makes the
// widgets inside it transparent by that name so the ground shows through them.
class CanonBody : public QWidget {
    Q_OBJECT

public:
    explicit CanonBody(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    // Painted once and kept until the size, the device pixel ratio or the theme changes:
    // the blooms are full-window gradients, too costly to redraw under every repaint.
    QPixmap m_ground;
};

} // namespace esa
