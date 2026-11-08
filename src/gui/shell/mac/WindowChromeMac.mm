#include "gui/shell/mac/WindowChromeMac.h"

#include <QGuiApplication>
#include <QWidget>

#import <AppKit/AppKit.h>

namespace esa::mac {

// Adapted from AetherSDR's src/gui/mac/NativeWindowTitle.mm (nativeView() and
// updateNativeTitleVisibility()) at 141231e4, GPL-3.0, without its toolbar.
// src/gui/aether/fork/README.md records the exact diff.
void setTitleTextHidden(QWidget* window, bool hidden)
{
    // Only the cocoa plugin's ids are NSViews; the offscreen plugin's, in a test or a
    // screenshot run, are plain numbers.
    if (!window || QGuiApplication::platformName() != QLatin1String("cocoa") || !window->internalWinId()) return;
    NSWindow* native = reinterpret_cast<NSView*>(window->internalWinId()).window;
    if (!native) return;
    const NSWindowTitleVisibility visibility = hidden ? NSWindowTitleHidden : NSWindowTitleVisible;
    if (native.titleVisibility != visibility) native.titleVisibility = visibility;
}

} // namespace esa::mac
