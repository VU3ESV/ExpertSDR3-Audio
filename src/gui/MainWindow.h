#pragma once

#include "app/Engine.h"
#include "app/Profiles.h"
#include "app/Settings.h"
#include "gui/ParamWidgets.h"
#include "gui/pages/Pages.h"
#include "tci/TciDiscovery.h"

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QRect>
#include <QStringList>

#include <functional>
#include <memory>
#include <utility>

class QAction;
class QMenu;
class QStackedWidget;

namespace AetherSDR {
class StageTabBar;
}

namespace esa {

class HeaderBar;
class NrPage;
class OutputPage;
class RadioPage;

// The AetherRX window, as a main window: a header row, the rail of pages down the left
// (AetherSDR's own StageTabBar) and the page it selects beside it.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // `taps` is the engine's own (Engine::taps()), kept here so the pages can still read it
    // after the engine has gone.
    MainWindow(Engine* engine, std::shared_ptr<LiveTaps> taps, Settings* settings, ProfileLibrary* profiles,
               const AppConfig& config);
    ~MainWindow() override;

    void connectNow();

    // Leaves any current radio first, restoring its mute.
    void connectToRadio(const esa::DiscoveredRadio& radio);

    // Profile actions behind the buttons, with no dialogs in the way.
    void loadProfile(const QString& name);  // empty: the factory settings
    bool saveProfileAs(const QString& name, QString* error = nullptr);
    bool deleteProfile(const QString& name, QString* error = nullptr);
    ImportResult importProfiles(const QString& path);
    QString currentProfile() const { return m_config.profileName; }
    bool chainModified() const;

    // The pages, by the ids --page and ui/page use, in the rail's order.
    QStringList pageIds() const;
    QString currentPage() const;
    void showPage(const QString& id);  // an unknown id changes nothing

    // The chain's stage order, as the rail shows it, and the one way it changes: the rail's
    // grips, its tab menus and Alt+Up/Down, and Reset stage order all come here.
    QStringList stageOrder() const;
    void setStageOrder(const QStringList& order);

    // Runs once closeEvent has saved everything. By default it quits the application, so
    // main() stops the engine and restores ExpertSDR3's mute; tests replace it.
    void setQuitHandler(std::function<void()> quit) { m_quit = std::move(quit); }

    // Whether saving (on close and every 5 s) writes the window's own state, ui/*, as well
    // as the settings. A screenshot run turns it off: its page and size are its own, and
    // the next ordinary start should open where the operator left the window.
    void setSavesWindowState(bool save) { m_savesWindowState = save; }

    // Whether a screen's available area is too small for the window's usual size, so it
    // opens maximised there (a Raspberry Pi touchscreen, say).
    static bool isSmallScreen(const QSize& available);
    // The size it opens at when nothing is saved: the AetherRX references' size.
    static QSize defaultSize() { return {724, 752}; }

    // Where the window opens, from ui/geometry and the screens' available areas, the main
    // screen first. Apart from the real screens, so every case can be tested anywhere.
    struct Placement {
        QRect geometry;           // when not `positioned`, only its size counts
        bool positioned{false};   // false: the window system picks the place
        bool maximised{false};
    };
    static Placement placement(const QString& savedGeometry, const QList<QRect>& screens);

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onStatus(const esa::EngineStatus& status);
    void onParamChanged(const QString& key, double value);
    void findRadios();
    void onProfileSelected(int index);
    void onSaveProfile();
    void onSaveProfileAs();
    void onDeleteProfile();
    void onImportProfiles();
    void onExportProfile();
    void onExportAllProfiles();
    void showAbout();

private:
    void buildPageContext();
    QWidget* buildRail();
    void buildPages();
    void buildActions();
    void buildMenus();
    void wireGatePreset();
    void restoreWindowGeometry();
    void popUpGearMenu();
    void popUpStageMenu(const QString& stage, const QPoint& globalPos);
    void moveStage(const QString& stage, int direction);
    ParamControl* registerControl(ParamControl* control);

    // The rail's host: what its boxes show and do.
    bool stageEnabled(int railId) const;
    void setStageEnabled(int railId, bool on);
    void selectNrMethod(const QString& id);

    void showChain();
    void refreshProfileList();
    void updateProfileState();
    bool exportWithDialog(const QStringList& names, const QString& suggestedName);
    void applyTheme();
    void saveSettings();

    template <class Fn>
    void onEngine(Fn&& fn)
    {
        QMetaObject::invokeMethod(m_engine, std::forward<Fn>(fn), Qt::QueuedConnection);
    }

    Engine* m_engine;
    std::shared_ptr<LiveTaps> m_taps;
    Settings* m_settings;
    ProfileLibrary* m_profiles;
    AppConfig m_config;
    Settings::WindowState m_window;
    QHash<QString, ParamControl*> m_controls;
    QString m_profileNote;
    std::function<void()> m_quit;
    bool m_shownOnce{false};
    bool m_savesWindowState{true};

    PageContext m_context;
    HeaderBar* m_header{nullptr};
    AetherSDR::StageTabBar* m_rail{nullptr};
    QStackedWidget* m_stack{nullptr};
    QList<Page*> m_pages;          // in the stack's order
    QHash<QObject*, QString> m_chainTabs;  // a chain stage's rail tab -> its stage id
    RadioPage* m_radio{nullptr};
    OutputPage* m_output{nullptr};
    NrPage* m_nr{nullptr};

    QMenu* m_gearMenu{nullptr};
    QAction* m_saveAction{nullptr};
    QAction* m_saveAsAction{nullptr};
    QAction* m_deleteAction{nullptr};
    QAction* m_importAction{nullptr};
    QAction* m_exportAction{nullptr};
    QAction* m_exportAllAction{nullptr};
    QAction* m_findRadiosAction{nullptr};
    QAction* m_resetOrderAction{nullptr};
    QAction* m_aboutAction{nullptr};
    QAction* m_websiteAction{nullptr};
    QAction* m_quitAction{nullptr};
};

} // namespace esa
