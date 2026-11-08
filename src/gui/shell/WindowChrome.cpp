#include "gui/shell/WindowChrome.h"

#include "gui/shell/HeaderBar.h"

#include "FramelessMoveHelper.h"
#include "FramelessResizer.h"

#include <QAction>
#include <QActionGroup>
#include <QEvent>
#include <QGuiApplication>
#include <QMainWindow>
#include <QMargins>
#include <QMenu>
#include <QMouseEvent>
#include <QRect>
#include <QTimer>
#include <QWindow>

#ifdef Q_OS_MACOS
#include "gui/shell/mac/WindowChromeMac.h"
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

namespace esa {

namespace {

std::optional<WindowChrome::Mode>& customModeOverride()
{
    static std::optional<WindowChrome::Mode> mode;  // the GUI thread only
    return mode;
}

// The hints a frame is made of. Nothing else in a window's flags is the frame's business.
Qt::WindowFlags frameHints()
{
    Qt::WindowFlags hints = Qt::FramelessWindowHint;
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    hints |= Qt::ExpandedClientAreaHint | Qt::NoTitleBarBackgroundHint;
#endif
    return hints;
}

// The resize band round a frameless window's edges. The content's own margins are wider,
// so the band never covers a control.
constexpr int kResizeBand = 6;

#ifdef Q_OS_WIN
// Windows 11 rounds a frameless window's corners only when asked. The attribute and its
// values are spelled out, so a Windows SDK older than 11's still builds; Windows 10 turns
// the request down and keeps its square corners.
void roundCorners(const QWidget* window, bool round)
{
    // Only the windows platform's ids are window handles; a test run's offscreen ones are not.
    if (QGuiApplication::platformName() != QLatin1String("windows") || !window->internalWinId()) return;
    constexpr DWORD kCornerPreference = 33;  // DWMWA_WINDOW_CORNER_PREFERENCE
    const DWORD preference = round ? 2 : 0;  // DWMWCP_ROUND, DWMWCP_DEFAULT
    DwmSetWindowAttribute(reinterpret_cast<HWND>(window->internalWinId()), kCornerPreference, &preference,
                          sizeof(preference));
}
#endif

} // namespace

WindowChrome::Frame WindowChrome::parse(const QString& stored)
{
    if (stored == QLatin1String("Custom")) return Frame::Custom;
    if (stored == QLatin1String("System")) return Frame::System;
    return defaultFrame();
}

QString WindowChrome::name(Frame frame)
{
    return frame == Frame::Custom ? QStringLiteral("Custom") : QStringLiteral("System");
}

WindowChrome::Mode WindowChrome::customMode()
{
    if (customModeOverride()) return *customModeOverride();
#if defined(Q_OS_MACOS) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    return Mode::ExpandedClientArea;
#elif defined(Q_OS_MACOS)
    return Mode::System;  // no way to put a title bar over the content before Qt 6.9
#else
    return Mode::Frameless;
#endif
}

void WindowChrome::overrideCustomMode(std::optional<Mode> mode)
{
    customModeOverride() = mode;
}

Qt::WindowFlags WindowChrome::flagsFor(Qt::WindowFlags flags, Mode mode)
{
    flags &= ~frameHints();
    switch (mode) {
    case Mode::System:
        break;
    case Mode::ExpandedClientArea:
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        flags |= Qt::ExpandedClientAreaHint | Qt::NoTitleBarBackgroundHint;
#endif
        break;
    case Mode::Frameless:
        // Everything else stays, the minimise and maximise hints included: on Windows they
        // are what lets the taskbar button, Win+Down and Win+Up minimise and maximise it.
        flags |= Qt::FramelessWindowHint;
        break;
    }
    return flags;
}

WindowChrome::WindowChrome(QMainWindow* window, HeaderBar* header, Frame frame)
    : QObject(window), m_window(window), m_header(header), m_frame(frame)
{
    m_menu = new QMenu(QStringLiteral("Window frame"), window);
    m_menu->setObjectName(QStringLiteral("frameMenu"));
    m_menu->setToolTipsVisible(true);
    auto* choices = new QActionGroup(m_menu);  // exclusive: one frame at a time
    m_systemAction = m_menu->addAction(QStringLiteral("System"));
    m_systemAction->setToolTip(QStringLiteral("The frame and title bar this computer gives every window."));
    m_customAction = m_menu->addAction(QStringLiteral("Custom"));
    switch (customMode()) {
    case Mode::ExpandedClientArea:
        m_customAction->setToolTip(QStringLiteral(
            "AetherRX's ground right to the top of the window, under the system's own buttons."));
        break;
    case Mode::Frameless:
        m_customAction->setToolTip(QStringLiteral(
            "AetherRX's own edge, with minimise and close in the header. Drag the header to move the "
            "window, double-click it to maximise, and drag an edge to resize."));
        break;
    case Mode::System:
        m_customAction->setEnabled(false);
        m_customAction->setToolTip(QStringLiteral("Needs Qt 6.9 or later, which this build does not have."));
        break;
    }
    for (QAction* action : {m_systemAction, m_customAction}) {
        action->setCheckable(true);
        choices->addAction(action);
    }
    connect(m_systemAction, &QAction::triggered, this, [this] {
        setFrame(Frame::System);
        emit frameChosen(Frame::System);
    });
    connect(m_customAction, &QAction::triggered, this, [this] {
        setFrame(Frame::Custom);
        emit frameChosen(Frame::Custom);
    });

    window->installEventFilter(this);
    header->installEventFilter(this);
    setFrame(frame);
}

void WindowChrome::setFrame(Frame frame)
{
    m_frame = frame;
    m_mode = modeFor(frame);
    updateMenu();

    // With the title bar drawn over it, the content spreads under it and is inset by
    // hand instead (updateInsets), so the ground reaches the window's top edge.
    const bool expanded = m_mode == Mode::ExpandedClientArea;
    m_window->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, !expanded);
    if (QWidget* body = m_window->centralWidget()) body->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, !expanded);
    // Never see-through: an opaque window needs no compositor for its corners, and on
    // Windows a translucent one loses the system's move, and with it snapping to the edges.
    m_window->setAttribute(Qt::WA_TranslucentBackground, false);

    if (m_mode == Mode::Frameless && !m_resizer) {
        // It does nothing while the window has a frame, so it can stay once installed.
        AetherSDR::FramelessResizer::install(m_window, kResizeBand);
        m_resizer = true;
    }
    m_header->setWindowButtons(m_mode == Mode::Frameless);

    const Qt::WindowFlags flags = flagsFor(m_window->windowFlags(), m_mode);
    if (flags != m_window->windowFlags()) {
        if (!m_window->isVisible()) {
            m_window->setWindowFlags(flags);  // before the first show: nothing else to keep
        } else {
            // New flags hide the window. It comes back with its content where it was, the
            // same size, maximised or not as it was, and the keyboard where it was.
            const Qt::WindowStates state = m_window->windowState() & ~Qt::WindowActive;
            const QRect normal = m_window->normalGeometry();
            const bool active = m_window->isActiveWindow();
            const QPointer<QWidget> focus = m_window->focusWidget();
            m_window->setWindowFlags(flags);
            m_window->setWindowState(Qt::WindowNoState);
            m_window->setGeometry(normal);
            m_window->setWindowState(state);
            m_window->show();
            if (active) m_window->activateWindow();
            if (focus) focus->setFocus(Qt::OtherFocusReason);
        }
    }
    updateNative();
}

void WindowChrome::updateMenu()
{
    // The frame the window has: a Custom that this build cannot make is the system's.
    const bool custom = m_mode != Mode::System;
    m_customAction->setChecked(custom);
    m_systemAction->setChecked(!custom);
}

void WindowChrome::updateNative()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    QWindow* handle = m_window->windowHandle();
    if (handle != m_followed) {
        if (m_followed) disconnect(m_followed, nullptr, this, nullptr);
        m_followed = handle;
        // It changes on entering and leaving full screen, where the title bar goes.
        if (handle) connect(handle, &QWindow::safeAreaMarginsChanged, this, &WindowChrome::updateInsets);
    }
#endif
    updateInsets();
#ifdef Q_OS_MACOS
    // The header shows the name; the system's title over the ground would say it twice.
    mac::setTitleTextHidden(m_window, m_mode == Mode::ExpandedClientArea);
#endif
#ifdef Q_OS_WIN
    roundCorners(m_window, m_mode == Mode::Frameless);
#endif
}

void WindowChrome::updateInsets()
{
    // The safe area is what the title bar and its buttons cover (and, full screen, a notch):
    // the content stays clear of it while the ground goes on underneath.
    QMargins safe;
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    if (m_mode == Mode::ExpandedClientArea && m_window->windowHandle()) safe = m_window->windowHandle()->safeAreaMargins();
#endif
    if (QWidget* body = m_window->centralWidget()) body->setContentsMargins(safe);
}

bool WindowChrome::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_window) {
        if (event->type() == QEvent::Show) {
            updateNative();
            // A platform may finish its native window only after the show event.
            QTimer::singleShot(0, this, &WindowChrome::updateNative);
        }
        return false;
    }
    if (watched != m_header || m_mode != Mode::Frameless) return false;

    // Only presses no control took reach the header itself: on its empty part, the title
    // and the rule. FramelessMoveHelper uses the platform's own move where that works,
    // which on Windows keeps snapping to the screen's edges, and moves by hand elsewhere.
    using namespace AetherSDR;
    switch (event->type()) {
    case QEvent::MouseButtonPress:
        return FramelessMoveHelper::start(m_header, static_cast<QMouseEvent*>(event));
    case QEvent::MouseMove:
        return FramelessMoveHelper::move(m_header, static_cast<QMouseEvent*>(event));
    case QEvent::MouseButtonRelease:
        return FramelessMoveHelper::finish(m_header, static_cast<QMouseEvent*>(event));
    case QEvent::MouseButtonDblClick:
        if (static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton) return false;
        FramelessMoveHelper::toggleMaximized(m_header);
        return true;
    default:
        return false;
    }
}

} // namespace esa
