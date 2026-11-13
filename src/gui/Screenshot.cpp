#include "gui/Screenshot.h"

#include "app/Engine.h"
#include "app/Profiles.h"
#include "app/Settings.h"
#include "gui/MainWindow.h"
#include "gui/aether/Appearance.h"

#include <QApplication>
#include <QIcon>
#include <QPixmap>
#include <QTimer>

#include <utility>

namespace esa {

std::unique_ptr<MainWindow> openMainWindow(Engine* engine, Settings* settings, ProfileLibrary* profiles,
                                           const AppConfig& config)
{
    Appearance::install();
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/ExpertSDR3Audio-256.png")));
    return std::make_unique<MainWindow>(engine, engine->taps(), settings, profiles, config);
}

void showForScreenshot(MainWindow& window, const QString& page, const QSize& size,
                       std::function<void(const QImage&)> taken)
{
    // The page and size a screenshot shows are its own, not where the operator left things.
    window.setSavesWindowState(false);
    if (!page.isEmpty()) window.showPage(page);
    if (size.isValid()) {
        // Exactly this size: no saved geometry, and not maximised on a small screen.
        window.setWindowState(Qt::WindowNoState);
        window.resize(size);
    }
    window.show();
    // Tied to the window, so a window closed before then takes no picture.
    QTimer::singleShot(kScreenshotDelayMs, &window, [&window, taken = std::move(taken)] {
        taken(window.grab().toImage());
    });
}

} // namespace esa
