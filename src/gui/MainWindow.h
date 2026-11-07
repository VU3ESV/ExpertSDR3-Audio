#pragma once

#include "app/Engine.h"
#include "app/Profiles.h"
#include "app/Settings.h"
#include "gui/ParamWidgets.h"
#include "tci/TciDiscovery.h"

#include <QHash>
#include <QMainWindow>

#include <functional>
#include <utility>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTabWidget;

namespace esa {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(Engine* engine, Settings* settings, ProfileLibrary* profiles, const AppConfig& config);

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

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onStatus(const esa::EngineStatus& status);
    void onDevices(const QList<esa::AudioDeviceInfo>& devices);
    void onConnectToggled(bool on);
    void onDeviceSelected(int index);
    void onNrSelected(int index);
    void onParamChanged(const QString& key, double value);
    void moveStage(int direction);
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
    QWidget* buildConnectionBox();
    QWidget* buildOutputBox();
    QWidget* buildProfileBar();
    void buildMenus();
    void showNrMethod(const QString& id);
    void showChain();
    void setOrderList(const QStringList& order);
    void refreshProfileList();
    void updateProfileState();
    bool exportWithDialog(const QStringList& names, const QString& suggestedName);
    QWidget* buildMeters();
    QTabWidget* buildTabs();
    QWidget* buildNrTab();
    QWidget* buildStageTab(const QString& group);
    QWidget* buildEqTab();
    QWidget* buildOrderTab();
    ParamControl* addParam(const ParamSpec& spec, class QGridLayout* grid, int row);
    void applyTheme();
    void pushStageOrder();
    void saveSettings();

    template <class Fn>
    void onEngine(Fn&& fn)
    {
        QMetaObject::invokeMethod(m_engine, std::forward<Fn>(fn), Qt::QueuedConnection);
    }

    Engine* m_engine;
    Settings* m_settings;
    ProfileLibrary* m_profiles;
    AppConfig m_config;
    QHash<QString, ParamControl*> m_controls;
    // Shows a value without re-applying it, for every parameter including the EQ's table.
    QHash<QString, std::function<void(double)>> m_editors;
    QString m_profileNote;

    QLineEdit* m_host{nullptr};
    QSpinBox* m_port{nullptr};
    QComboBox* m_receiver{nullptr};
    QPushButton* m_connect{nullptr};
    QPushButton* m_findRadios{nullptr};
    QLabel* m_radioLabel{nullptr};

    QComboBox* m_device{nullptr};
    QSpinBox* m_buffer{nullptr};
    QSlider* m_gain{nullptr};
    QLabel* m_gainLabel{nullptr};
    QCheckBox* m_boost{nullptr};
    QPushButton* m_muteOutput{nullptr};
    QCheckBox* m_muteExpert{nullptr};
    QLabel* m_muteStatus{nullptr};
    QLabel* m_outputStatus{nullptr};

    QComboBox* m_profileList{nullptr};
    QLabel* m_profileStatus{nullptr};
    QAction* m_saveAction{nullptr};
    QAction* m_saveAsAction{nullptr};
    QAction* m_deleteAction{nullptr};
    QAction* m_importAction{nullptr};
    QAction* m_exportAction{nullptr};
    QAction* m_exportAllAction{nullptr};

    QCheckBox* m_autoBypass{nullptr};
    QComboBox* m_nrMethod{nullptr};
    QLabel* m_nrDescription{nullptr};
    QStackedWidget* m_nrParams{nullptr};
    QLabel* m_nrStatus{nullptr};

    QListWidget* m_order{nullptr};

    QProgressBar* m_inputMeter{nullptr};
    QProgressBar* m_outputMeter{nullptr};
    QLabel* m_inputValue{nullptr};
    QLabel* m_outputValue{nullptr};
    QLabel* m_stats{nullptr};
};

} // namespace esa
