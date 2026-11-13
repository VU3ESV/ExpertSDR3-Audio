#pragma once

class QWidget;

namespace esa::mac {

// Hides or shows the title text AppKit draws in a window's title bar. Qt has no hint for
// it: a title bar drawn over the content still says the window's name, which the header
// already shows. The title itself is kept, for the Dock, the Window menu and VoiceOver.
// Does nothing until the window has a Cocoa window, or on any other platform plugin.
void setTitleTextHidden(QWidget* window, bool hidden);

} // namespace esa::mac
