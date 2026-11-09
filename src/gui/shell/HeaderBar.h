#pragma once

#include <QWidget>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QToolButton;

namespace esa {

class StatusChip;
struct EngineStatus;

// The window's one header row: AetherSDR's makeCanonHeader(), the title in canon ink over a
// hairline rule 8 px below it, with this app's controls in the title's row. Beside the title
// sit the profile combo and its Modified / Note chip, and at the far end REC's and PLAY's
// clocks, the status chip and the exact-values button. A window without a frame of its own
// adds minimise and close after them.
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

    // "⋯": the current stage's exact values. Usable only on a page that has them; the dot
    // says the page leaves out a value moved from its default.
    QPushButton* exactValuesButton() const { return m_exactValues; }
    void setExactValues(bool available, bool flagged);
    bool exactValuesFlagged() const;

    // "● REC 01:23" while a recording is on, amber once it has lost audio, and
    // "▶ 00:12 / 01:23" while PLAY plays; each hidden otherwise. The full texts, before any
    // shortening to fit: empty while hidden.
    void showActivity(const EngineStatus& s);
    QString recordingClock() const;
    QString playbackClock() const;
    // The texts themselves, from a status alone.
    static QString recordingText(const EngineStatus& s);
    static QString playbackText(const EngineStatus& s);
    // mm:ss, and h:mm:ss from an hour on; whole seconds, counted as a clock counts them.
    static QString clockText(double seconds);

    // Minimise and close chips at the end of the row, for a window with no frame of its
    // own to carry them; made the first time they are wanted. Neither takes the keyboard,
    // and close goes through the window's close(), as every way out does.
    void setWindowButtons(bool shown);

    // The row's minimum without REC's and PLAY's clocks, whether they show or not: they make
    // do with the room the rest leaves them, and never widen the window.
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void fitRow();

    QHBoxLayout* m_row{nullptr};
    QLabel* m_title{nullptr};
    QToolButton* m_minimise{nullptr};
    QToolButton* m_close{nullptr};
    QComboBox* m_profiles{nullptr};
    QLabel* m_profileState{nullptr};
    QLabel* m_recordClock{nullptr};
    QLabel* m_playClock{nullptr};
    StatusChip* m_status{nullptr};
    QPushButton* m_exactValues{nullptr};
};

} // namespace esa
