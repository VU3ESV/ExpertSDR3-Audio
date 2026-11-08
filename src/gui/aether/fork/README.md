# Forks of AetherSDR files

The files here are adapted copies of AetherSDR source files. Everything AetherSDR ships
unchanged lives in `vendor/aethersdr/`, which only `scripts/sync-aether.sh` writes. A
file lands here instead when it has to change to build on this app's platforms, when
only part of it is wanted and the rest cannot build here, or when a piece of it is
wanted in another form (the check box tick, as an image). Only that file is forked, the
change is kept as small as it can be, and each fork records where it came from and
exactly what differs. Like AetherSDR, these files are GPL-3.0. One adapted piece lives
elsewhere, beside the code that uses it, and is recorded here all the same: the macOS
title helper in `src/gui/shell/mac/WindowChromeMac.mm` (NativeWindowTitle, below).

A fork does not follow upstream by itself. `scripts/aether-fork-diff.sh` (run by every
sync) prints what upstream has changed in these files since the commit recorded below;
carry over what applies, then update the commit and the diff here. It reads each fork's
`- Upstream:` line, so keep that line's format: the upstream paths, `@`, then the full
commit hash, each in backticks.

## NrGainStrip

- Upstream: `src/gui/NrGainStrip.h` `src/gui/NrGainStrip.cpp` @ `141231e412ed554b9ab49213b2f8c68614fbf193`
- Forked on 2026-11-07, at the same commit as `vendor/aethersdr/UPSTREAM_COMMIT`.
- Why: `NrGainStrip::resizeBuffers()` calls `QList::assign`, which Qt added in 6.6.
  Debian Bookworm and Raspberry Pi OS ship Qt 6.4.2. `fill(value, size)` resizes and
  fills the same way and exists in every Qt 6.
- The header is unchanged apart from its notice. It is kept here, not vendored, so the
  class has one home.

The exact diff, upstream to ours:

```diff
--- a/src/gui/NrGainStrip.h
+++ b/src/gui/aether/fork/NrGainStrip.h
@@ -1,3 +1,6 @@
+// Forked from AetherSDR src/gui/NrGainStrip.h at 141231e4 (GPL-3.0) on 2026-11-07.
+// Unchanged apart from this notice; it sits beside its forked .cpp. README.md here
+// records the provenance and the exact diff.
 #pragma once
 
 #include "PanelTick.h"
--- a/src/gui/NrGainStrip.cpp
+++ b/src/gui/aether/fork/NrGainStrip.cpp
@@ -1,3 +1,7 @@
+// Forked from AetherSDR src/gui/NrGainStrip.cpp at 141231e4 (GPL-3.0) on 2026-11-07.
+// Changed: resizeBuffers() uses QList::fill instead of QList::assign, which is Qt 6.6
+// API, so this builds on the Qt 6.4.2 that Debian Bookworm and Raspberry Pi OS ship.
+// README.md here records the exact diff.
 #include "NrGainStrip.h"
 
 #include <QHideEvent>
@@ -55,8 +59,8 @@
     if (m_values.size() == n) {
         return;
     }
-    m_values.assign(n, 0.0f);
-    m_signal.assign(n, quint8(0));
+    m_values.fill(0.0f, n);
+    m_signal.fill(quint8(0), n);
     m_cursor = 0;
 }
 
```

## CanonGround

- Upstream: `src/gui/CanonWindow.h` `src/gui/CanonWindow.cpp` @ `141231e412ed554b9ab49213b2f8c68614fbf193`
- Forked on 2026-11-07, at the same commit as `vendor/aethersdr/UPSTREAM_COMMIT`.
- Why: the window paints AetherRX's ground and carries its header (the window's
  `HeaderBar` builds on `makeCanonHeader` and puts its own controls in the title's row),
  and, in the custom frame of Windows and Linux, its close button at the end of that row
  (the minimise chip beside it is the same button with a dash). The header's chips are
  the row's height, smaller than upstream's 26 px, so `HeaderBar::setWindowButtons`
  gives them `makeCanonCloseButton`'s sheet again with only the 13 px radius changed to
  half their side, or Qt would draw them square. These four free functions are what draw
  them:
  `paintCanonGround`, `makeCanonCloseButton`, `placeCanonCloseButton` and
  `makeCanonHeader`. The rest of
  CanonWindow cannot be vendored: its Spark widgets follow the system's reduced-motion
  setting through `QAccessibilityHints::motionPreference`, which is newer than Qt 6.10
  (let alone the 6.4.2 that Debian Bookworm ships), and the
  CanonWindow class itself is a frameless dialog this app does not use.
- What was taken: `CanonWindow.cpp` lines 6-7, 11-12, 15, 17-19, 24-25, 27-33, 42-43,
  283-371 and 542-569 (the ThemeManager include and the Qt includes the four functions
  use, the namespace, the three constants they read, then the functions), and
  `CanonWindow.h` lines 1-2, 6, 8-10, 101-120 and 197 (their declarations and comments).
  Every line of those is unchanged. When upstream changes CanonWindow, only changes to
  these lines matter; the Spark and window code can be ignored.
- To check the diff below against upstream, from an AetherSDR checkout at the commit:

  ```sh
  diff -u <(sed -n '1,2p;6p;8,10p;101,120p;197p' src/gui/CanonWindow.h) path/to/CanonGround.h
  diff -u <(sed -n '6,7p;11,12p;15p;17,19p;24,25p;27,33p;42,43p;283,371p;542,569p' src/gui/CanonWindow.cpp) path/to/CanonGround.cpp
  ```

The exact diff, upstream lines as listed above to ours:

```diff
--- a/src/gui/CanonWindow.h
+++ b/src/gui/aether/fork/CanonGround.h
@@ -1,7 +1,17 @@
+// Forked from AetherSDR src/gui/CanonWindow.h at 141231e4 (GPL-3.0) on 2026-11-07.
+// Only the declarations of the free functions that paint and dress a canon surface;
+// the CanonWindow class and the Spark widgets stay upstream (their motion code needs
+// a far newer Qt than 6.4). What CanonWindow.h's <QDialog> used to bring in is spelled
+// out. README.md here records the provenance and the exact diff.
 #pragma once
 
+#include <QtGlobal>
+
 class QPainter;
+class QSize;
+class QString;
 class QToolButton;
+class QWidget;
 
 namespace AetherSDR {
 
--- a/src/gui/CanonWindow.cpp
+++ b/src/gui/aether/fork/CanonGround.cpp
@@ -1,3 +1,10 @@
+// Forked from AetherSDR src/gui/CanonWindow.cpp at 141231e4 (GPL-3.0) on 2026-11-07.
+// Only paintCanonGround, makeCanonCloseButton, placeCanonCloseButton and
+// makeCanonHeader, with the includes and constants they use, unchanged; the
+// CanonWindow class and the Spark widgets stay upstream (their motion code needs a
+// far newer Qt than 6.4). README.md here records the provenance and the exact diff.
+#include "CanonGround.h"
+
 #include "core/ThemeManager.h"
 
 #include <QFrame>
```

## CheckTicks

- Upstream: `src/gui/CanonIndicators.h` @ `141231e412ed554b9ab49213b2f8c68614fbf193`
- Taken on 2026-11-07, at the same commit as `vendor/aethersdr/UPSTREAM_COMMIT`. The
  header itself is vendored unchanged; only its tick is adapted here.
- Why: the window draws its check boxes with a style sheet, which cannot draw a tick,
  and an image a sheet names must be a file or a resource. Upstream paints its canon
  check boxes at run time into the user's cache directory (`canonIndicatorRules()`,
  through `ThemedImageCache.h`). Here the box stays the sheet's, in theme tokens, and only
  the tick is an image, compiled into `aether_gui` as `:/canon/check-tick*.png`.
- What was taken: `CanonIndicators.h` lines 89-95, the tick's path and pen, into
  `tools/make-check-ticks.cpp`, moved 2 px up and left onto the 14 px inside of the sheet's
  16 px box (upstream draws on an 18 px canvas with the box inset by 1 px and a 1 px
  edge). Its colours are those of lines 115-120: `color.canon.onAccent` for a ticked box
  and `color.canon.control` for a ticked box that is disabled, written as fixed values.
- `tools/make-check-ticks.cpp` is not built, and sits in its own folder so no glob of
  this one picks up its `main()`. It painted the six `check-tick*.png` here (1x, 2x and
  3x); run it again from this folder, as its header says, if upstream changes the tick or
  those two tokens change. It reproduces the committed images byte for byte.
- To check the diff below against upstream, from an AetherSDR checkout at the commit:

  ```sh
  diff -u <(sed -n '89,95p' src/gui/CanonIndicators.h) <(sed -n '36,42p' path/to/fork/tools/make-check-ticks.cpp)
  ```

The exact diff, upstream lines as listed above to ours:

```diff
--- a/src/gui/CanonIndicators.h
+++ b/src/gui/aether/fork/tools/make-check-ticks.cpp
@@ -1,7 +1,7 @@
-                QPainterPath path;
-                path.moveTo(5.0, 9.2);
-                path.lineTo(7.9, 12.0);
-                path.lineTo(13.0, 6.2);
-                p.setPen(QPen(*tick, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
-                p.setBrush(Qt::NoBrush);
-                p.drawPath(path);
+        QPainterPath path;
+        path.moveTo(3.0, 7.2);
+        path.lineTo(5.9, 10.0);
+        path.lineTo(11.0, 4.2);
+        p.setPen(QPen(colour, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
+        p.setBrush(Qt::NoBrush);
+        p.drawPath(path);
```

## NativeWindowTitle

- Upstream: `src/gui/mac/NativeWindowTitle.mm` @ `141231e412ed554b9ab49213b2f8c68614fbf193`
- Taken on 2026-11-08, at the same commit as `vendor/aethersdr/UPSTREAM_COMMIT`. Ours is
  `setTitleTextHidden()` in `src/gui/shell/mac/WindowChromeMac.mm`, with the window's
  other frame code rather than in this folder.
- Why: in the custom frame on macOS the title bar is drawn over the window's ground, and
  AppKit would still write the window's name in it, over the header that already shows
  it. Qt has no hint for that; upstream's helper sets `NSWindow.titleVisibility`.
- What was taken: `NativeWindowTitle.mm` lines 16-32, `nativeView()` and the first half of
  `updateNativeTitleVisibility()`: the check for the cocoa plugin and a native window, the
  NSView to NSWindow step, and the title visibility, folded into one function that takes
  the visibility rather than reading it from the window's flags. Upstream's unified
  toolbar (lines 7-12 and 33-57) is left out: this window keeps the standard title bar.
  When upstream changes the file, only changes to those lines matter.
- To check the diff below against upstream, from an AetherSDR checkout at the commit:

  ```sh
  diff -u <(sed -n '16,32p' src/gui/mac/NativeWindowTitle.mm) <(sed -n '10,22p' path/to/WindowChromeMac.mm)
  ```

The exact diff, upstream lines as listed above to ours:

```diff
--- a/src/gui/mac/NativeWindowTitle.mm
+++ b/src/gui/shell/mac/WindowChromeMac.mm
@@ -1,17 +1,13 @@
-static NSView* nativeView(const QWidget* window)
+// Adapted from AetherSDR's src/gui/mac/NativeWindowTitle.mm (nativeView() and
+// updateNativeTitleVisibility()) at 141231e4, GPL-3.0, without its toolbar.
+// src/gui/aether/fork/README.md records the exact diff.
+void setTitleTextHidden(QWidget* window, bool hidden)
 {
-    if (!window || QGuiApplication::platformName() != QStringLiteral("cocoa")
-        || !window->internalWinId()) {
-        return nil;
-    }
-    return reinterpret_cast<NSView*>(window->internalWinId());
+    // Only the cocoa plugin's ids are NSViews; the offscreen plugin's, in a test or a
+    // screenshot run, are plain numbers.
+    if (!window || QGuiApplication::platformName() != QLatin1String("cocoa") || !window->internalWinId()) return;
+    NSWindow* native = reinterpret_cast<NSView*>(window->internalWinId()).window;
+    if (!native) return;
+    const NSWindowTitleVisibility visibility = hidden ? NSWindowTitleHidden : NSWindowTitleVisible;
+    if (native.titleVisibility != visibility) native.titleVisibility = visibility;
 }
-
-void updateNativeTitleVisibility(QWidget* window)
-{
-    NSWindow* native = nativeView(window).window;
-    if (!native) {
-        return;
-    }
-    const bool expanded = window->windowFlags().testFlag(Qt::ExpandedClientAreaHint);
-    native.titleVisibility = expanded ? NSWindowTitleHidden : NSWindowTitleVisible;
```
