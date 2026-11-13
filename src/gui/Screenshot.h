#pragma once

#include <QImage>
#include <QSize>
#include <QString>

#include <functional>
#include <memory>

namespace esa {

class Engine;
class MainWindow;
class ProfileLibrary;
class Settings;
struct AppConfig;

// How long a screenshot waits after showing the window: long enough for the engine's first
// status and device list to arrive and for every page to lay itself out and draw its idle
// meters.
inline constexpr int kScreenshotDelayMs = 2000;

// The main window as the application opens it: the look installed (once per process) and the
// window icon set, then the window built on `engine`. main() and test_look both open it here,
// so the goldens see whatever the application sets up for its look. A step that changes how
// the window draws, such as a font, style or palette, belongs in here rather than in main().
std::unique_ptr<MainWindow> openMainWindow(Engine* engine, Settings* settings, ProfileLibrary* profiles,
                                           const AppConfig& config);

// What --screenshot saves, and what test_look compares with its goldens, so the two cannot
// drift apart. Shows `window` on `page` (empty: the page saved) at exactly `size` (invalid:
// the size saved), saving none of that, so the next ordinary start opens where the operator
// left the window; then, kScreenshotDelayMs later, hands its picture to `taken`.
void showForScreenshot(MainWindow& window, const QString& page, const QSize& size,
                       std::function<void(const QImage&)> taken);

} // namespace esa
