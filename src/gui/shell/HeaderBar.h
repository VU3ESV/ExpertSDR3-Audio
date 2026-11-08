#pragma once

#include <QWidget>

class QComboBox;
class QLabel;

namespace esa {

class StatusChip;

// The window's one header row: AetherSDR's makeCanonHeader(), the title in canon ink over a
// hairline rule 8 px below it, with this app's controls in the title's row. Beside the title
// sit the profile combo and its Modified / Note chip, and at the far end the status chip.
//
// The row is exactly as tall as the title, as upstream's is, so the rail and the pages
// start where they do in AetherRX: every control in it takes the row's height rather
// than asking for its own.
class HeaderBar : public QWidget {
    Q_OBJECT

public:
    explicit HeaderBar(const QString& title, QWidget* parent = nullptr);

    QComboBox* profileList() const { return m_profiles; }
    StatusChip* statusChip() const { return m_status; }

    // "Modified" or "Note" beside the profile, with the reason as its tooltip; empty hides it.
    void setProfileState(const QString& text, const QString& toolTip);
    QString profileState() const;

private:
    QLabel* m_title{nullptr};
    QComboBox* m_profiles{nullptr};
    QLabel* m_profileState{nullptr};
    StatusChip* m_status{nullptr};
};

} // namespace esa
