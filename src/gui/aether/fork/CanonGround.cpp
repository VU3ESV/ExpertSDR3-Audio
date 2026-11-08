// Forked from AetherSDR src/gui/CanonWindow.cpp at 141231e4 (GPL-3.0) on 2026-11-07.
// Only paintCanonGround, makeCanonCloseButton, placeCanonCloseButton and
// makeCanonHeader, with the includes and constants they use, unchanged; the
// CanonWindow class and the Spark widgets stay upstream (their motion code needs a
// far newer Qt than 6.4). README.md here records the provenance and the exact diff.
#include "CanonGround.h"

#include "core/ThemeManager.h"

#include <QFrame>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QToolButton>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {
constexpr int kCloseSize = 26;
constexpr int kCloseMargin = 12;
constexpr int kGridStep = 64;
} // namespace

void paintCanonGround(QPainter& p, const QWidget* scope, const QSize& size, qreal radius)
{
    auto& tm = ThemeManager::instance();
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);

    // The hairline is stroked on the half-pixel line so it lands on whole
    // pixels. A rounded ground is clipped to that line too; a square one
    // fills to the edge, so an embedded surface has no half-covered rim for
    // its parent to show through.
    const QRectF bounds(QPointF(0, 0), QSizeF(size));
    const QRectF line = bounds.adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath shape;
    QPainterPath outline;
    if (radius > 0) {
        shape.addRoundedRect(line, radius, radius);
        outline = shape;
    } else {
        shape.addRect(bounds);
        outline.addRect(line);
    }
    const QRectF r = shape.boundingRect();
    p.setClipPath(shape);

    p.fillRect(r, tm.color(scope, QStringLiteral("color.canon.ground")));

    auto bloom = [&](const QPointF& centre, qreal reach, const QString& token) {
        QColor c = tm.color(scope, token);
        QColor clear = c;
        clear.setAlpha(0);
        QRadialGradient g(centre, reach);
        g.setColorAt(0.0, c);
        g.setColorAt(0.6, clear);
        p.fillRect(r, g);
    };
    bloom(QPointF(r.width() * 0.72, -r.height() * 0.08), r.width() * 1.1,
          QStringLiteral("color.canon.bloom.blue"));
    bloom(QPointF(r.width() * 0.12, r.height() * 0.04), r.width() * 0.9,
          QStringLiteral("color.canon.bloom.teal"));

    // Grid: full strength at the top, gone three quarters of the way down.
    const QColor grid = tm.color(scope, QStringLiteral("color.canon.grid"));
    const qreal fadeTo = r.height() * 0.75;
    for (int y = kGridStep; y < fadeTo; y += kGridStep) {
        QColor c = grid;
        c.setAlphaF(grid.alphaF() * (1.0 - y / fadeTo));
        p.setPen(QPen(c, 1));
        p.drawLine(QPointF(0, y + 0.5), QPointF(r.width(), y + 0.5));
    }
    QColor clearGrid = grid;
    clearGrid.setAlpha(0);
    QLinearGradient fade(0, 0, 0, fadeTo);
    fade.setColorAt(0.0, grid);
    fade.setColorAt(1.0, clearGrid);
    p.setPen(QPen(QBrush(fade), 1));
    for (int x = kGridStep; x < r.width(); x += kGridStep) {
        p.drawLine(QPointF(x + 0.5, 0), QPointF(x + 0.5, fadeTo));
    }

    p.setClipping(false);
    p.setPen(QPen(tm.color(scope, QStringLiteral("color.canon.lineHi")), 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(outline);
    p.restore();
}

QToolButton* makeCanonCloseButton(QWidget* parent)
{
    auto* close = new QToolButton(parent);
    close->setText(QStringLiteral("✕"));
    close->setAccessibleName(QStringLiteral("Close"));
    close->setToolTip(QStringLiteral("Close"));
    close->setFixedSize(kCloseSize, kCloseSize);
    close->setCursor(Qt::PointingHandCursor);
    ThemeManager::instance().applyStyleSheet(close,
        "QToolButton { background: transparent; color: {{color.canon.muted}}; border: 1px solid transparent; "
        "border-radius: 13px; font-size: 12px; }"
        "QToolButton:hover { background: {{color.canon.nested}}; color: {{color.canon.ink}}; border-color: {{color.canon.lineHi}}; }"
        "QToolButton:focus { border: 2px solid {{color.canon.cyan}}; }");
    return close;
}

void placeCanonCloseButton(QToolButton* close)
{
    const QWidget* host = close->parentWidget();
    close->move(host->width() - kCloseSize - kCloseMargin, kCloseMargin);
    close->raise();
}

QWidget* makeCanonHeader(const QString& title)
{
    auto* header = new QWidget;
    header->setObjectName(QStringLiteral("canonHeader"));
    auto* col = new QVBoxLayout(header);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(8);

    auto* label = new QLabel(title, header);
    label->setObjectName(QStringLiteral("canonHeaderTitle"));
    label->setAccessibleName(title);
    label->setContentsMargins(0, 0, 40, 0);
    col->addWidget(label);

    auto* rule = new QFrame(header);
    rule->setObjectName(QStringLiteral("canonHeaderRule"));
    col->addWidget(rule);

    AetherSDR::ThemeManager::instance().applyStyleSheet(header,
        "QWidget#canonHeader { background: transparent; }"
        "QLabel#canonHeaderTitle { background: transparent; color: {{color.canon.ink}};"
        " font-size: 17px; font-weight: 700; }"
        "QFrame#canonHeaderRule { background: {{color.canon.line}}; border: none;"
        " min-height: 1px; max-height: 1px; }");
    return header;
}

} // namespace AetherSDR
