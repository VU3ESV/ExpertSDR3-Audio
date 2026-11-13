// Forked from AetherSDR src/gui/CanonWindow.h at 141231e4 (GPL-3.0) on 2026-11-07.
// Only the declarations of the free functions that paint and dress a canon surface;
// the CanonWindow class and the Spark widgets stay upstream (their motion code needs
// a far newer Qt than 6.4). What CanonWindow.h's <QDialog> used to bring in is spelled
// out. README.md here records the provenance and the exact diff.
#pragma once

#include <QtGlobal>

class QPainter;
class QSize;
class QString;
class QToolButton;
class QWidget;

namespace AetherSDR {

// The guide's ambient ground, shared by CanonWindow and the embedded canon
// surfaces (CanonSurface): color.canon.ground with a blue and a teal bloom, a
// 64 px grid that fades out three quarters of the way down, and a hairline
// color.canon.lineHi border, all clipped to a `radius` rounded rectangle (0
// for square corners). Colors resolve through ThemeManager in `scope`'s theme
// container.
void paintCanonGround(QPainter& p, const QWidget* scope, const QSize& size, qreal radius);

// The round close button in a canon surface's top-right corner, styled from
// color.canon.* tokens. The caller connects clicked() and keeps it placed with
// placeCanonCloseButton() from its resizeEvent().
QToolButton* makeCanonCloseButton(QWidget* parent);
void placeCanonCloseButton(QToolButton* close);

// The canon header a CanonWindow carries in place of a title bar: the title in
// canon ink over a hairline, with a right margin that keeps it clear of the
// corner close button. The empty part of it is a handle for moving the window.
QWidget* makeCanonHeader(const QString& title);

} // namespace AetherSDR
