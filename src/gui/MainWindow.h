#pragma once

#include "app/Engine.h"
#include "app/Settings.h"
#include "gui/ParamWidgets.h"

#include <QHash>
#include <QMainWindow>

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
    MainWindow(Engine* engine, Settings* settings, const AppConfig& config);

    void connectNow();

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

private:
    QWidget* buildConnectionBox();
    QWidget* buildOutputBox();
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
    AppConfig m_config;
    QHash<QString, ParamControl*> m_controls;

    QLineEdit* m_host{nullptr};
    QSpinBox* m_port{nullptr};
    QComboBox* m_receiver{nullptr};
    QPushButton* m_connect{nullptr};
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
