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
#include <QPointer>
#include <QRect>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

class QAction;
class QMenu;
class QMessageBox;
class QPushButton;
class QStackedWidget;

namespace AetherSDR {
class AudioEngine;
class StageTabBar;
}

namespace esa {

class DetailsWindow;
class HeaderBar;
class NrPage;
class OutputPage;
class RadioPage;
class StageBinding;
class StageHost;
class WindowChrome;

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
    // What the window holds and saves: the settings, with the chain that is playing.
    const AppConfig& config() const { return m_config; }

    // The pages, by the ids --page and ui/page use, in the rail's order.
    QStringList pageIds() const;
    QString currentPage() const;
    void showPage(const QString& id);  // an unknown id changes nothing

    // Final Output's Volume knob comes here, and nothing else changes the volume: rounded to
    // the knob's tenths of a dB, kept with this computer's settings and handed to the engine.
    void setVolumeDb(double db);

    // The chain's stage order, as the rail shows it, and the one way it changes: the rail's
    // grips, its tab menus and Alt+Up/Down, and Reset stage order all come here.
    QStringList stageOrder() const;
    void setStageOrder(const QStringList& order);

    // Runs once closeEvent has saved everything. By default it quits the application, so
    // main() stops the engine and restores ExpertSDR3's mute; tests replace it.
    void setQuitHandler(std::function<void()> quit) { m_quit = std::move(quit); }

    // Shows a file, or a folder, in the system's file manager: Finder, Explorer or the
    // desktop's own. Everything the window reveals goes through this, so tests replace it
    // and never open one.
    void setRevealHandler(std::function<void(const QString& path)> reveal) { m_reveal = std::move(reveal); }
    // Asks where recordings go, starting from the folder they go to now, and returns the
    // choice, or nothing to leave it. A folder dialog unless replaced, as tests do.
    void setFolderChooser(std::function<QString(const QString& current)> choose) { m_chooseFolder = std::move(choose); }
    // What gear › Recordings › Show in… is called here: Finder, Explorer or Files.
    static QString revealCommandText();

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
    // screen first. Apart from the real screens, so every case can be tested anywhere. The
    // geometry is the content's; `titleHeight` is room left on the screen for a title bar the
    // system draws above it, so the whole window fits. A Mac and GNOME move a window down
    // themselves to keep its title bar on the screen; Windows leaves it where it is put, so
    // there `placeTitle` keeps that room above the content too.
    struct Placement {
        QRect geometry;           // when not `positioned`, only its size counts
        bool positioned{false};   // false: the window system picks the place
        bool maximised{false};
    };
    static Placement placement(const QString& savedGeometry, const QList<QRect>& screens, int titleHeight = 0,
                               bool placeTitle = false);
    // Where the window goes back to when its frame changes while it is shown: where its content
    // `normal` was, unless a title bar the new frame adds, `titleHeight` tall, would be left
    // above the top of the screen, out of reach, by a system that leaves it where it is put
    // (`placeTitle`). Then it is placed as when it opens, by placement().
    static QRect placementAfterFrameChange(const QRect& normal, const QList<QRect>& screens, int titleHeight,
                                           bool placeTitle);
    // The room the window leaves for the system frame's title bar: its `titleHeight` above
    // while the frame is the system's.
    static int systemTitleAllowance();
    // Whether this system leaves a window's title bar where the window is put, as Windows does.
    // The window passes the answer to the two functions above as `placeTitle`.
    static bool placesTitleBar();
    // Tests only: answer placesTitleBar() as another system would, so Windows' path runs on any
    // machine. Set before the window is made; nullopt goes back to this system's answer.
    static void overridePlacesTitleBar(std::optional<bool> places);

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
    void buildBinding();
    void buildPageContext();
    QWidget* buildRail();
    void buildDetails();
    void buildPages();
    void buildActions();
    void buildMenus();
    void restoreWindowGeometry();
    void popUpGearMenu();
    void popUpStageMenu(const QString& stage, const QPoint& globalPos);
    void moveStage(const QString& stage, int direction);
    ParamControl* registerControl(ParamControl* control);

    // The rail's host: what its boxes show and do.
    bool stageEnabled(int railId) const;
    void setStageEnabled(int railId, bool on);
    void selectNrMethod(const QString& id);
    // The method the AetherNR box turns back on: the last that ran, else NR2.
    QString lastNrMethod() const;

    // The chain stage the current page shows ("gate"); empty on Radio, AetherNR and Final Output.
    QString currentStage() const;
    // Opens a stage's exact-values window, if its page has one.
    void showDetails(const QString& stage);
    bool hasDetails(const QString& stage) const;
    // The header's "⋯" and the gear's Exact values for the page showing.
    void updateExactValues();
    // BYPASS as the engine reports it: the button, and the rail's boxes greyed while it holds.
    void showBypass(bool bypassed);
    // REC and PLAY as the engine reports them: the buttons, the header's clocks, and a
    // warning for each failure reported since the last status.
    void showRecording(const EngineStatus& s);
    void onRecordingFailed(const QString& reason);
    void onPlaybackFailed(const QString& reason);
    // One warning at a time. The same cause again brings the open one forward rather than
    // adding another saying the same; a different one takes its place. `offersFolder` adds
    // a button that asks for another recordings folder.
    void showNotice(const QString& cause, const QString& title, const QString& text, bool offersFolder = false);
    void updateRecordingsMenu();
    void chooseRecordingFolder();
    // What Show in… shows: the last recording if it is still there, else the folder if it
    // exists yet; empty when neither does.
    QString revealTarget() const;

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

    // The AetherRX panels draw from the display mirror (m_taps->view) through the stand-in
    // engine, and the binding keeps the mirror and the controls in step.
    StageBinding* m_binding{nullptr};
    AetherSDR::AudioEngine* m_aether{nullptr};
    QHash<QString, DetailsWindow*> m_details;  // by stage, one each, for good
    QHash<QString, StageHost*> m_hosts;        // the stages whose page is AetherRX's own

    PageContext m_context;
    HeaderBar* m_header{nullptr};
    WindowChrome* m_chrome{nullptr};
    AetherSDR::StageTabBar* m_rail{nullptr};
    QStackedWidget* m_stack{nullptr};
    QList<Page*> m_pages;          // in the stack's order
    QHash<QObject*, QString> m_chainTabs;  // a chain stage's rail tab -> its stage id
    RadioPage* m_radio{nullptr};
    OutputPage* m_output{nullptr};
    NrPage* m_nr{nullptr};
    QPushButton* m_bypass{nullptr};
    bool m_bypassShown{false};
    bool m_outputMuted{false};  // Final Output's MUTE: this app's output, this session only

    // REC and PLAY, which show what the engine reports and never just the click.
    QPushButton* m_record{nullptr};
    QPushButton* m_play{nullptr};
    bool m_playing{false};
    QString m_lastRecordingFile;  // the file REC wrote last, whether or not it is finished
    // recordingFailed() also reports a REC turned down, which is no failure. The engine sends
    // the status saying so before the signal, so the last status seen tells the two apart.
    EngineStatus::Recording m_lastRecording;
    // A failure waits here for the status sent after it, which names what was kept.
    QStringList m_recordingFailures;
    QPointer<QMessageBox> m_notice;
    QString m_noticeCause;
    std::function<void(const QString&)> m_reveal;
    std::function<QString(const QString&)> m_chooseFolder;

    QMenu* m_gearMenu{nullptr};
    QMenu* m_recordingsMenu{nullptr};
    QAction* m_recordingsFolderAction{nullptr};
    QAction* m_revealAction{nullptr};
    QAction* m_folderShown{nullptr};
    QAction* m_lastFileShown{nullptr};
    QAction* m_saveAction{nullptr};
    QAction* m_saveAsAction{nullptr};
    QAction* m_deleteAction{nullptr};
    QAction* m_importAction{nullptr};
    QAction* m_exportAction{nullptr};
    QAction* m_exportAllAction{nullptr};
    QAction* m_findRadiosAction{nullptr};
    QAction* m_resetOrderAction{nullptr};
    QAction* m_exactValuesAction{nullptr};
    QAction* m_aboutAction{nullptr};
    QAction* m_websiteAction{nullptr};
    QAction* m_quitAction{nullptr};
};

} // namespace esa
