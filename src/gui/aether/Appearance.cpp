#include "Appearance.h"

#include "CanonGround.h"
#include "StagePage.h"
#include "Theme.h"
#include "core/ThemeManager.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QPalette>
#include <QStyleFactory>

namespace esa {

using AetherSDR::ThemeManager;

namespace {

// Declared before the first lookup, so colorAt() finds the scope instead of quietly
// answering from the root theme. ThemeManager keeps a declared scope across theme loads.
void declareContainer()
{
    static const bool declared = [] {
        ThemeManager::instance().registerDeclaredContainer(Appearance::container());
        return true;
    }();
    Q_UNUSED(declared);
}

// `top` laid over `under`. Palette roles want opaque colours, and the canon lines are
// translucent so that they read the same on every canon surface.
QColor over(const QColor& top, const QColor& under)
{
    const float a = top.alphaF();
    return QColor::fromRgbF(top.redF() * a + under.redF() * (1.0f - a),
                            top.greenF() * a + under.greenF() * (1.0f - a),
                            top.blueF() * a + under.blueF() * (1.0f - a));
}

// For what Fusion draws from the palette rather than from a sheet: tooltips, parentless
// dialogs, a platform file dialog's widgets, focus and selection.
QPalette canonPalette()
{
    const auto c = [](const char* token) { return Appearance::color(QString::fromLatin1(token)); };
    const QColor window = c("color.background.0");
    const QColor control = c("color.canon.control");
    const QColor text = c("color.text.primary");
    // AetherSDR's colour for a button that cannot be pressed: dim, but still readable,
    // about as far from the window as today's disabled grey was from its field.
    const QColor disabled = c("color.button.foreground.disabled");

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, control);
    p.setColor(QPalette::AlternateBase, c("color.background.1"));
    p.setColor(QPalette::ToolTipBase, c("color.canon.raised"));
    p.setColor(QPalette::ToolTipText, c("color.canon.ink"));
    p.setColor(QPalette::PlaceholderText, c("color.text.label"));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, control);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, c("color.canon.ink"));
    p.setColor(QPalette::Light, over(c("color.canon.lineHi"), control));
    p.setColor(QPalette::Midlight, over(c("color.canon.lineHi"), window));
    p.setColor(QPalette::Mid, over(c("color.canon.line"), window));
    p.setColor(QPalette::Dark, c("color.canon.ground"));
    p.setColor(QPalette::Shadow, c("color.canon.ground"));
    p.setColor(QPalette::Link, c("color.canon.cyan"));
    p.setColor(QPalette::LinkVisited, c("color.canon.aqua"));
    p.setColor(QPalette::Highlight, c("color.canon.cyan"));
    p.setColor(QPalette::HighlightedText, c("color.canon.onAccent"));
    for (const QPalette::ColorRole role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText}) {
        p.setColor(QPalette::Disabled, role, disabled);
    }
    p.setColor(QPalette::Disabled, QPalette::Base, window);
    p.setColor(QPalette::Disabled, QPalette::Button, window);
    return p;
}

} // namespace

QString Appearance::container()
{
    return QStringLiteral("canon/aetherRx");
}

void Appearance::install()
{
    static bool installed = false;  // the GUI thread only, like everything here
    if (installed) return;
    installed = true;

    // Bundled, so the px-sized sheets look the same whatever fonts the machine has.
    // Static Regular and Bold, as AetherSDR ships them: a variable font needs Qt 6.7.
    for (const char* path : {":/fonts/Inter-Regular.ttf", ":/fonts/Inter-Bold.ttf"}) {
        if (QFontDatabase::addApplicationFont(QString::fromLatin1(path)) < 0) {
            qWarning("Could not load the bundled font %s", path);
        }
    }
    // One look on every platform. Fusion draws whatever the sheets leave to the style.
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    declareContainer();
    QApplication::setPalette(canonPalette());
}

void Appearance::apply(QWidget* window, const QString& extraSheet)
{
    AetherSDR::theme::setContainer(window, container());
    ThemeManager::instance().applyStyleSheet(
        window, AetherSDR::appStylesheetTemplate() + AetherSDR::canonBodyStyleSheet() + extraSheet);
}

QColor Appearance::color(const QString& token)
{
    declareContainer();
    return ThemeManager::instance().colorAt(container(), token);
}

CanonBody::CanonBody(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("canonBody"));
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        m_ground = QPixmap();
        update();
    });
}

void CanonBody::paintEvent(QPaintEvent*)
{
    // As upstream:src/gui/CanonWindow.cpp:270-281 does for its own window.
    const qreal dpr = devicePixelRatioF();
    if (m_ground.isNull() || m_ground.size() != size() * dpr
        || !qFuzzyCompare(m_ground.devicePixelRatio(), dpr)) {
        m_ground = QPixmap(size() * dpr);
        m_ground.setDevicePixelRatio(dpr);
        m_ground.fill(Qt::transparent);
        QPainter p(&m_ground);
        AetherSDR::paintCanonGround(p, this, size(), 0);  // square: the system frame
    }
    QPainter(this).drawPixmap(0, 0, m_ground);
}

} // namespace esa
