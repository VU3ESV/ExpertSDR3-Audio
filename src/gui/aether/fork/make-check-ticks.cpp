// Adapted from AetherSDR src/gui/CanonIndicators.h at 141231e4 (GPL-3.0) on 2026-11-07:
// its check box's tick, alone, with the same path, pen and colours, moved 2 px up and left
// onto the 14 px inside of the 16 px box MainWindow's style sheet draws. README.md here
// records the provenance and the exact diff.
//
// Paints the check-tick*.png beside it, which that sheet puts in a ticked check box. A
// sheet cannot draw a tick, and an image it names must be a file or a resource; upstream
// paints its check boxes at run time into the user's cache directory, while here the box
// stays the sheet's, in theme tokens, and only the tick is an image, compiled in. Each is
// written at 1x, 2x and 3x, as upstream does, so Qt picks the sharp one for the screen.
//
// Not part of the build: run it again only to change them. From this directory, in bash
// (Qt's headers go in as system headers, which Apple's clang needs for Homebrew's Qt):
//   c++ -std=c++20 make-check-ticks.cpp \
//       $(pkg-config --cflags --libs Qt6Gui | sed 's/-I/-isystem /g') -o /tmp/make-check-ticks
//   /tmp/make-check-ticks
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QString>

#include <cstdio>

namespace {

bool paintTick(const QString& name, const QColor& colour)
{
    constexpr int kSize = 14;
    for (int scale = 1; scale <= 3; ++scale) {
        QImage image(kSize * scale, kSize * scale, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(scale, scale);
        QPainterPath path;
        path.moveTo(3.0, 7.2);
        path.lineTo(5.9, 10.0);
        path.lineTo(11.0, 4.2);
        p.setPen(QPen(colour, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.end();
        const QString file = scale == 1 ? name + QStringLiteral(".png")
                                        : name + QStringLiteral("@%1x.png").arg(scale);
        if (!image.save(file, "PNG")) {
            std::fprintf(stderr, "cannot write %s\n", qPrintable(file));
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    // Upstream's check_on draws the tick in color.canon.onAccent on a cyan box, and its
    // check_on_dis in color.canon.control on a muted one (CanonIndicators.h:115-120).
    const bool ok = paintTick(QStringLiteral("check-tick"), QColor(0x04, 0x10, 0x19))
                    && paintTick(QStringLiteral("check-tick-disabled"), QColor(0x12, 0x23, 0x3a));
    return ok ? 0 : 1;
}
