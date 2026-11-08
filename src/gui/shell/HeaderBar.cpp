#include "gui/shell/HeaderBar.h"

#include "gui/shell/StatusChip.h"

#include "CanonGround.h"
#include "core/ThemeManager.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace esa {

namespace {

// What this header adds to AetherSDR's: the title and the rule bring their own sheet from
// makeCanonHeader(). Colours are tokens, resolved in the window's canon scope.
QString headerSheet()
{
    return QStringLiteral(R"(
QComboBox#headerProfile {
    color: {{color.canon.ink}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 5px;
    padding: 0 6px;
    font-size: 12px;
}
QComboBox#headerProfile:hover { border-color: {{color.canon.aqua}}; }
QComboBox#headerProfile:focus { border-color: {{color.canon.cyan}}; }
QComboBox#headerProfile::drop-down { border: none; width: 16px; }
/* A sheet that styles the drop-down gets no arrow from Qt: a border triangle in the
   field's own colours draws one, with no image. */
QComboBox#headerProfile::down-arrow {
    width: 0;
    height: 0;
    border-top: 5px solid {{color.canon.inkSoft}};
    border-left: 4px solid {{color.canon.control}};
    border-right: 4px solid {{color.canon.control}};
    border-bottom: 0px solid {{color.canon.control}};
}

QLabel#profileState {
    color: {{color.accent.warning}};
    background: transparent;
    border: 1px solid {{color.accent.warning}};
    border-radius: 10px;
    padding: 0 8px;
    font-size: 12px;
    font-weight: 600;
}
)");
}

} // namespace

HeaderBar::HeaderBar(const QString& title, QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("headerBar"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    // AetherSDR's own canon header, the title over its rule, with the title moved into a row
    // that carries this header's controls after it.
    QWidget* canon = AetherSDR::makeCanonHeader(title);
    outer->addWidget(canon);
    auto* col = qobject_cast<QVBoxLayout*>(canon->layout());
    m_title = canon->findChild<QLabel*>(QStringLiteral("canonHeaderTitle"));
    Q_ASSERT(col && m_title);
    col->removeWidget(m_title);
    // Upstream keeps the title clear of a close button in the corner. This row has none: it
    // ends in the status chip.
    m_title->setContentsMargins(0, 0, 0, 0);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    col->insertLayout(0, row);
    row->addWidget(m_title);
    row->addSpacing(6);

    // Every control below ignores its own height and takes the row's, which is the title's.
    m_profiles = new QComboBox;
    m_profiles->setObjectName(QStringLiteral("headerProfile"));
    m_profiles->setAccessibleName(QStringLiteral("Profile"));
    m_profiles->setToolTip(QStringLiteral(
        "A saved set of everything on the stage pages: noise reduction, its settings, and the stages."));
    // As wide as a usual name whatever the longest one is, so a long name cannot widen the window.
    m_profiles->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_profiles->setMinimumContentsLength(16);
    m_profiles->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    row->addWidget(m_profiles);

    m_profileState = new QLabel;
    m_profileState->setObjectName(QStringLiteral("profileState"));
    m_profileState->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    m_profileState->hide();
    row->addWidget(m_profileState);

    row->addStretch(1);

    m_status = new StatusChip;
    row->addWidget(m_status);

    AetherSDR::ThemeManager::instance().applyStyleSheet(this, headerSheet());
}

void HeaderBar::setProfileState(const QString& text, const QString& toolTip)
{
    m_profileState->setText(text);
    m_profileState->setToolTip(toolTip);
    m_profileState->setVisible(!text.isEmpty());
}

QString HeaderBar::profileState() const
{
    return m_profileState->isHidden() ? QString() : m_profileState->text();
}

} // namespace esa
