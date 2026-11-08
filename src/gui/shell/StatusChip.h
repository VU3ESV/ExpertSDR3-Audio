#pragma once

#include "app/Engine.h"

#include <QPushButton>
#include <QString>

namespace esa {

// Rich text in one of the theme's colour tokens, as the canon/aetherRx scope resolves it.
QString colored(const QString& token, const QString& text);

// The window's status messages, worded as they have always been, for the pages that show
// them and for the chip's tooltip. Rich text; empty when there is nothing to say.
namespace statusText {
QString radio(const EngineStatus& s);   // the connection line, and a second line about audio
QString mute(const EngineStatus& s);    // Mute ExpertSDR3's five states
QString output(const EngineStatus& s);  // the output device: an error, or what is buffered
QString nr(const EngineStatus& s);
QString recording(const EngineStatus& s);  // why REC stopped, or that it is dropping audio, and its file
} // namespace statusText

// The single most serious state, always in the header: red for what is broken, amber for
// what is not there yet, green with the frequency when all is well. Clicking it opens the
// page concerned; its tooltip holds every message in full.
class StatusChip : public QPushButton {
    Q_OBJECT

public:
    struct Summary {
        QString text;
        QString tone;  // "danger", "warning", "success" or "idle"
        QString page;  // the page id a click opens; empty for none
    };
    // The first that applies, most serious first: the output failing, ExpertSDR3's mute
    // failing, the computer not keeping up, NR failing, a recording that stopped early (until
    // its warning is answered), the connection, no audio from the radio, a recording losing
    // audio, NR's state, and then the frequency.
    static Summary summarize(const EngineStatus& s, bool recordingFailureAnswered = false);

    explicit StatusChip(QWidget* parent = nullptr);

    void showStatus(const EngineStatus& s);
    const Summary& summary() const { return m_summary; }
    // Whether the operator has answered the warning about the recording failure in the status.
    // A failure is news: once answered, the chip shows the rest of the status again, and the
    // tooltip still has the failure. The window sets it back for each new failure.
    void setRecordingFailureAnswered(bool answered);

    // Sized for the whole summary, up to a limit; narrower than that, it is shortened with an
    // ellipsis and the tooltip has it all.
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void pageRequested(const QString& id);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    // What the sheet adds around the text: padding and border.
    int chrome() const;
    void refreshText();

    Summary m_summary;
    EngineStatus m_status;  // the last shown
    bool m_failureAnswered{false};
};

} // namespace esa
