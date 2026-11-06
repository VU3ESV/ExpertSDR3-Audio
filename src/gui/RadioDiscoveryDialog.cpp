#include "gui/RadioDiscoveryDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace esa {

RadioDiscoveryDialog::RadioDiscoveryDialog(const TciDiscovery::Options& options, QWidget* parent)
    : QDialog(parent), m_options(options)
{
    setWindowTitle(QStringLiteral("Find Radios"));
    resize(480, 340);

    m_status = new QLabel;
    m_status->setWordWrap(true);
    m_list = new QListWidget;
    m_progress = new QProgressBar;
    m_progress->setTextVisible(false);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_connect = buttons->addButton(QStringLiteral("Connect"), QDialogButtonBox::AcceptRole);
    m_scanAgain = buttons->addButton(QStringLiteral("Scan Again"), QDialogButtonBox::ActionRole);
    m_connect->setDefault(true);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_status);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_progress);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_scanAgain, &QPushButton::clicked, this, &RadioDiscoveryDialog::scan);
    connect(m_list, &QListWidget::currentRowChanged, this, &RadioDiscoveryDialog::updateConnectButton);
    connect(m_list, &QListWidget::itemActivated, this, &QDialog::accept);
    connect(&m_discovery, &TciDiscovery::radioFound, this, &RadioDiscoveryDialog::onRadioFound);
    connect(&m_discovery, &TciDiscovery::finished, this, &RadioDiscoveryDialog::onFinished);
    connect(&m_discovery, &TciDiscovery::progressChanged, this, [this](int done, int total) {
        m_progress->setRange(0, total);
        m_progress->setValue(done);
    });

    // Once the dialog is up, so that a scan finishing at once still shows.
    QTimer::singleShot(0, this, &RadioDiscoveryDialog::scan);
}

void RadioDiscoveryDialog::scan()
{
    m_radios.clear();
    m_list->clear();
    m_scanAgain->setEnabled(false);
    m_progress->show();
    m_status->setText(QStringLiteral("Looking for TCI radios on %1 addresses. Radios in use are not disturbed.")
                          .arg(m_options.hosts.size()));
    updateConnectButton();
    m_discovery.start(m_options);
}

void RadioDiscoveryDialog::onRadioFound(const DiscoveredRadio& radio)
{
    m_radios.append(radio);
    auto* item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(radio.title(), radio.location()));
    item->setData(Qt::UserRole, int(m_radios.size() - 1));
    m_list->addItem(item);
    if (m_list->currentRow() < 0) {
        m_list->setCurrentRow(0);
        m_list->setFocus();  // so Enter connects, rather than whatever button had focus
    }
}

void RadioDiscoveryDialog::onFinished()
{
    m_scanAgain->setEnabled(true);
    m_progress->hide();
    if (!m_radios.isEmpty()) {
        m_status->setText(m_radios.size() == 1 ? QStringLiteral("Found 1 radio.")
                                               : QStringLiteral("Found %1 radios.").arg(m_radios.size()));
        return;
    }
    QString hint = QStringLiteral(
        "No radios found. Check that ExpertSDR3 is running with TCI enabled in its options, and that "
        "its computer's firewall allows the TCI port. You can still type a host and port in the main window.");
#ifdef Q_OS_MACOS
    hint += QStringLiteral(" On a Mac, also allow this app under System Settings, Privacy & Security, Local Network.");
#endif
    m_status->setText(hint);
}

void RadioDiscoveryDialog::updateConnectButton()
{
    m_connect->setEnabled(m_list->currentRow() >= 0);
}

std::optional<DiscoveredRadio> RadioDiscoveryDialog::selectedRadio() const
{
    const QListWidgetItem* item = m_list->currentItem();
    if (!item) return std::nullopt;
    return m_radios.value(item->data(Qt::UserRole).toInt());
}

} // namespace esa
