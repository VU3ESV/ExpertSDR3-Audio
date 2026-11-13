#pragma once

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <Qt>

#include <functional>
#include <optional>
#include <utility>

class QAction;
class QMainWindow;
class QMenu;
class QWindow;

namespace esa {

class HeaderBar;

// The window's frame, saved as ui/frame: the platform's own (System, the default
// everywhere) or AetherRX's (Custom). What Custom is depends on the platform:
//  - macOS, with Qt 6.9 or later: the system's frame kept, with its title bar drawn over
//    the window's own ground, so the traffic lights sit on it and the content starts below
//    them. Moving, resizing, full screen and the main-window status all stay the system's.
//    An older Qt cannot ask for that, so there Custom is the system frame.
//  - Windows, Linux and the Raspberry Pi: no system frame at all. The header's empty part
//    moves the window and double-clicking it maximises; a 6 px band round the edges resizes
//    it; the header carries minimise and close chips. The window stays opaque, so nothing
//    needs a compositor, and Windows 11 rounds its corners.
class WindowChrome : public QObject {
    Q_OBJECT

public:
    enum class Frame { System, Custom };
    Q_ENUM(Frame)
    // How a frame is made on a platform.
    enum class Mode { System, ExpandedClientArea, Frameless };
    Q_ENUM(Mode)

    static Frame defaultFrame() { return Frame::System; }
    // ui/frame's value; empty or unknown gives the default.
    static Frame parse(const QString& stored);
    static QString name(Frame frame);  // "System" or "Custom"

    // What Custom is on this platform with this Qt.
    static Mode customMode();
    static Mode modeFor(Frame frame) { return frame == Frame::Custom ? customMode() : Mode::System; }

    // `flags` with `mode`'s frame hints in place of whatever frame hints they had. Apart
    // from the platform, so every platform's flags can be checked on any machine.
    static Qt::WindowFlags flagsFor(Qt::WindowFlags flags, Mode mode);

    // Tests only: what Custom is, as if on another platform, so the Windows and Linux path
    // runs on a Mac too. Set before the window is made; nullopt goes back to the platform's.
    static void overrideCustomMode(std::optional<Mode> mode);

    // Applies `frame` to `window` before it is first shown. The header is the one HeaderBar
    // `window` carries, as its central widget's first row.
    WindowChrome(QMainWindow* window, HeaderBar* header, Frame frame);

    Frame frame() const { return m_frame; }
    Mode mode() const { return m_mode; }

    // Gear > Window frame: System and Custom, the frame in use ticked. The window's child,
    // so it is drawn in the window's look.
    QMenu* menu() const { return m_menu; }

    // Applies `frame` at once. Changing a window's frame hides it, so a visible window is
    // shown again where it was (or where the placer puts it), in the same state.
    void setFrame(Frame frame);

    // Where a visible window goes back to after its frame changed, from where its content was
    // (its normal geometry), with mode() already the new frame's. A new frame can add a title
    // bar above the content, which must stay on the screen. Unset, exactly where it was.
    using Placer = std::function<QRect(const QRect& normal)>;
    void setPlacer(Placer placer) { m_placer = std::move(placer); }

signals:
    // The operator picked `frame` from the menu. It is already applied; the window saves it.
    void frameChosen(esa::WindowChrome::Frame frame);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // What can only be done once the window has a native window, and again whenever it
    // is shown: following its safe area, and the platform's own touches.
    void updateNative();
    void updateInsets();
    void updateMenu();

    QMainWindow* m_window;
    HeaderBar* m_header;
    Frame m_frame;
    Mode m_mode{Mode::System};
    QMenu* m_menu{nullptr};
    QAction* m_systemAction{nullptr};
    QAction* m_customAction{nullptr};
    QPointer<QWindow> m_followed;  // the native window whose safe area the content follows
    bool m_resizer{false};         // the edge resizer, once installed, stays for good
    Placer m_placer;
};

} // namespace esa
