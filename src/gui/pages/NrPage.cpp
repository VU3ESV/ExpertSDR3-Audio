#include "gui/pages/NrPage.h"

#include "dsp/NrProcessor.h"
#include "gui/shell/StatusChip.h"

#include "core/ThemeManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace esa {

NrPage::NrPage(PageContext& ctx)
    : Page(ctx)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("Method")));
    m_method = new QComboBox;
    m_method->setObjectName(QStringLiteral("nrMethod"));
    m_method->addItem(QStringLiteral("Off"), QString());
    for (const NrMethod& method : nrMethods()) m_method->addItem(method.label, method.id);
    top->addWidget(m_method, 1);
    layout->addLayout(top);

    m_description = hintLabel();
    layout->addWidget(m_description);

    m_params = new QStackedWidget;
    m_params->addWidget(new QWidget);  // "Off" has no parameters
    for (const NrMethod& method : nrMethods()) {
        auto* panel = new QWidget;
        auto* grid = new QGridLayout(panel);
        int row = 0;
        for (const ParamSpec& spec : method.params) ctx.addParam(spec, grid, row++);
        grid->setRowStretch(row, 1);
        grid->setColumnStretch(1, 1);
        m_params->addWidget(scrollable(panel));
    }
    layout->addWidget(m_params, 1);

    m_autoBypass = new QCheckBox(QStringLiteral("Bypass NR automatically on CW and digital modes"));
    m_autoBypass->setChecked(config().nrAutoBypass);
    m_autoBypass->setToolTip(QStringLiteral(
        "These are not speech. A speech model treats a steady carrier as noise and can remove the signal itself."));
    connect(m_autoBypass, &QCheckBox::toggled, this, [this](bool on) {
        config().nrAutoBypass = on;
        context().post([on](Engine* engine) { engine->setNrAutoBypass(on); });
        context().chainChanged();
    });
    layout->addWidget(m_autoBypass);

    m_status = hintLabel();
    layout->addWidget(m_status);

    connect(m_method, &QComboBox::currentIndexChanged, this, [this](int index) {
        context().selectNrMethod(m_method->itemData(index).toString());
    });
    showMethod(config().nrMethod);
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, interimPageSheet());
}

void NrPage::showMethod(const QString& id)
{
    const int index = std::max(0, m_method->findData(id));
    {
        const QSignalBlocker block(m_method);
        m_method->setCurrentIndex(index);
    }
    m_params->setCurrentIndex(index);
    const NrMethod* method = findNrMethod(id);
    m_description->setText(colored(QStringLiteral("color.text.label"),
                                   method ? method->description : QStringLiteral("Audio passes through unchanged.")));
}

void NrPage::showChain()
{
    showMethod(config().nrMethod);
    const QSignalBlocker block(m_autoBypass);
    m_autoBypass->setChecked(config().nrAutoBypass);
}

void NrPage::showStatus(const EngineStatus& s)
{
    const QString nr = statusText::nr(s);
    if (nr != m_status->text()) m_status->setText(nr);
}

} // namespace esa
