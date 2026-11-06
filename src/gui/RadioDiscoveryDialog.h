#pragma once

#include "tci/TciDiscovery.h"

#include <QDialog>

#include <optional>

class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;

namespace esa {

// Scans for TCI radios as soon as it opens, and lets the operator pick one.
class RadioDiscoveryDialog : public QDialog {
    Q_OBJECT

public:
    explicit RadioDiscoveryDialog(const TciDiscovery::Options& options, QWidget* parent = nullptr);

    std::optional<DiscoveredRadio> selectedRadio() const;
    bool isScanning() const { return m_discovery.isRunning(); }

public slots:
    void scan();

private slots:
    void onRadioFound(const esa::DiscoveredRadio& radio);
    void onFinished();
    void updateConnectButton();

private:
    TciDiscovery::Options m_options;
    TciDiscovery m_discovery;
    QList<DiscoveredRadio> m_radios;
    QLabel* m_status{nullptr};
    QListWidget* m_list{nullptr};
    QProgressBar* m_progress{nullptr};
    QPushButton* m_scanAgain{nullptr};
    QPushButton* m_connect{nullptr};
};

} // namespace esa
