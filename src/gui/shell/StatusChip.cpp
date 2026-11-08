#include "gui/shell/StatusChip.h"

#include "gui/aether/Appearance.h"

#include "core/ThemeManager.h"

#include <QResizeEvent>
#include <QStringList>
#include <QStyle>

#include <algorithm>

namespace esa {

namespace {

// Status colours are AetherSDR's theme tokens, as the window's canon scope resolves them.
const QString kSuccess = QStringLiteral("color.accent.success");
const QString kWarning = QStringLiteral("color.accent.warning");
const QString kDanger = QStringLiteral("color.accent.danger");
const QString kLabel = QStringLiteral("color.text.label");

QString formatFrequency(qint64 hz)
{
    return QStringLiteral("%1 MHz").arg(double(hz) / 1e6, 0, 'f', 3);
}

// Never wider than this, so a long error cannot push the header's other controls away,
// and never narrower than enough to read the colour and the first word.
constexpr int kMaxWidth = 300;
constexpr int kMinWidth = 80;

} // namespace

QString colored(const QString& token, const QString& text)
{
    const QString color = Appearance::color(token).name();
    return QStringLiteral("<span style='color:%1'>%2</span>").arg(color, text.toHtmlEscaped());
}

namespace statusText {

QString radio(const EngineStatus& s)
{
    QString radio;
    switch (s.connection) {
    case TciClient::State::Disconnected:
        radio = colored(kLabel, QStringLiteral("Not connected"));
        break;
    case TciClient::State::Connecting: {
        radio = colored(kWarning, QStringLiteral("Looking for ExpertSDR3 on port %1...").arg(s.connectedPort));
        if (!s.connectionError.isEmpty()) {
            radio += QStringLiteral(" ") + colored(kLabel, QStringLiteral("(is TCI enabled in its options?)"));
        }
        break;
    }
    case TciClient::State::Initializing:
        radio = colored(kWarning, QStringLiteral("Connected, reading radio state..."));
        break;
    case TciClient::State::Ready: {
        QString line = QStringLiteral("%1 %2  ·  %3").arg(s.serverName, s.serverVersion, s.device);
        if (s.frequencyHz > 0) {
            line += QStringLiteral("  ·  %1 %2").arg(formatFrequency(s.frequencyHz), s.modulation);
        }
        if (s.transmitting) line += QStringLiteral("  ·  TX");
        radio = colored(kSuccess, line);
        if (!s.receiverEnabled) {
            radio += QStringLiteral("<br>") + colored(kWarning, QStringLiteral("This receiver is switched off in ExpertSDR3, so it has no audio."));
        } else if (!s.audioFlowing) {
            radio += QStringLiteral("<br>") + colored(kWarning, QStringLiteral("Waiting for audio. Is ExpertSDR3 started?"));
        }
        break;
    }
    }
    return radio;
}

QString mute(const EngineStatus& s)
{
    // Say what is true now, and whether anything needs doing.
    switch (s.mutePhase) {
    case MuteController::Phase::Holding:
        return colored(kSuccess, QStringLiteral("ExpertSDR3 muted"));
    case MuteController::Phase::Engaging:
        return colored(kWarning, QStringLiteral("Muting ExpertSDR3..."));
    case MuteController::Phase::Restoring:
        return colored(kWarning, QStringLiteral("Restoring ExpertSDR3..."));
    case MuteController::Phase::OperatorOverride:
        return colored(kWarning, QStringLiteral("You unmuted ExpertSDR3; leaving it alone"));
    case MuteController::Phase::Failed:
        return colored(kDanger, QStringLiteral("ExpertSDR3 did not respond to mute"));
    case MuteController::Phase::Idle:
        break;
    }
    return QString();
}

QString output(const EngineStatus& s)
{
    if (!s.outputError.isEmpty() && !s.outputOpen) return colored(kDanger, s.outputError);
    if (s.outputOpen) {
        return colored(kLabel, QStringLiteral("%1 ms buffered, %2 Hz")
                                   .arg(s.sink.bufferedMs, 0, 'f', 0)
                                   .arg(s.sink.deviceRate));
    }
    return QString();
}

QString nr(const EngineStatus& s)
{
    if (s.dspLoad > 0.85) {
        // Past this the stream falls behind and audio breaks up. A Raspberry Pi can get
        // here on the neural methods, so say what to do about it.
        return colored(kDanger, QStringLiteral("This computer cannot keep up with this method. Try NR2, which uses far less CPU."));
    }
    if (s.nrFailed) {
        return colored(kDanger, QStringLiteral("Could not start this method. Audio is passing through unprocessed."));
    }
    if (s.nrPreparing) return colored(kWarning, QStringLiteral("Preparing..."));
    if (s.nrBypassedByMode) {
        return colored(kWarning, QStringLiteral("Bypassed: %1 is not a speech mode").arg(s.modulation));
    }
    if (!s.nrMethod.isEmpty() && s.audioFlowing) {
        return colored(kSuccess, QStringLiteral("Active, removing %1 dB").arg(-s.nrGainDb, 0, 'f', 1));
    }
    return QString();
}

QString recording(const EngineStatus& s)
{
    const EngineStatus::Recording& r = s.recording;
    // A press that was turned down recorded nothing, so there is no file to point to.
    if (r.refused && !r.error.isEmpty()) return colored(kWarning, QStringLiteral("Not recording: %1").arg(r.error));
    QString line;
    if (!r.error.isEmpty()) {
        line = colored(kDanger, QStringLiteral("Recording stopped: %1").arg(r.error));
    } else if (r.active && r.droppedFrames > 0) {
        line = colored(kWarning, QStringLiteral("Recording dropped audio: the disk could not keep up"));
    }
    // Where to find what was kept.
    if (!line.isEmpty() && !r.path.isEmpty()) line += QStringLiteral("<br>") + colored(kLabel, r.path);
    return line;
}

} // namespace statusText

StatusChip::Summary StatusChip::summarize(const EngineStatus& s, bool recordingFailureAnswered)
{
    const QString radioPage = QStringLiteral("radio");
    const QString nrPage = QStringLiteral("nr");
    if (!s.outputError.isEmpty()) {
        return {QStringLiteral("Output: %1").arg(s.outputError), QStringLiteral("danger"), QStringLiteral("output")};
    }
    if (s.mutePhase == MuteController::Phase::Failed) {
        return {QStringLiteral("ExpertSDR3 mute failed"), QStringLiteral("danger"), radioPage};
    }
    if (s.dspLoad > 0.85) return {QStringLiteral("Can't keep up: try NR2"), QStringLiteral("danger"), nrPage};
    if (s.nrFailed) return {QStringLiteral("NR failed: running dry"), QStringLiteral("danger"), nrPage};
    // No page has more to say about a recording; the tooltip names its file. A REC that was
    // turned down is not here: the button flips back and the tooltip says why. Nor is one
    // whose warning has been answered: what went wrong is no longer news, and the chip has the
    // connection to show.
    if (!s.recording.error.isEmpty() && !s.recording.refused && !recordingFailureAnswered) {
        return {QStringLiteral("Recording stopped: %1").arg(s.recording.error), QStringLiteral("danger"), QString()};
    }
    switch (s.connection) {
    case TciClient::State::Disconnected:
        return {QStringLiteral("Not connected"), QStringLiteral("idle"), radioPage};
    case TciClient::State::Connecting:
    case TciClient::State::Initializing:
        return {QStringLiteral("Connecting…"), QStringLiteral("warning"), radioPage};
    case TciClient::State::Ready:
        break;
    }
    if (!s.audioFlowing) return {QStringLiteral("No audio from radio"), QStringLiteral("warning"), radioPage};
    // While it lasts: the count stays with the finished recording, which is no longer losing any.
    if (s.recording.active && s.recording.droppedFrames > 0) {
        return {QStringLiteral("Recording dropped audio"), QStringLiteral("warning"), QString()};
    }
    if (s.nrPreparing) return {QStringLiteral("NR preparing…"), QStringLiteral("warning"), nrPage};
    if (s.nrBypassedByMode) {
        return {QStringLiteral("NR bypassed: %1").arg(s.modulation), QStringLiteral("warning"), nrPage};
    }

    QStringList parts;
    if (s.frequencyHz > 0) {
        parts << QStringLiteral("%1 %2").arg(formatFrequency(s.frequencyHz), s.modulation).trimmed();
    } else if (!s.modulation.isEmpty()) {
        parts << s.modulation;
    }
    parts << QStringLiteral("RX%1").arg(s.receiver + 1);
    if (s.transmitting) parts << QStringLiteral("TX");
    return {QStringLiteral("● ") + parts.join(QStringLiteral(" · ")), QStringLiteral("success"), radioPage};
}

StatusChip::StatusChip(QWidget* parent)
    : QPushButton(parent)
{
    setObjectName(QStringLiteral("statusChip"));
    setFlat(true);
    setCursor(Qt::PointingHandCursor);
    // A chip, not a stop on the way through the window: the rail is where the keyboard goes.
    setFocusPolicy(Qt::NoFocus);
    // Its height is the header row's, never its own (HeaderBar).
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, QStringLiteral(R"(
QPushButton#statusChip {
    color: {{color.text.label}};
    background: transparent;
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 10px;
    padding: 0 10px;
    font-size: 12px;
    font-weight: 600;
    text-align: center;
}
QPushButton#statusChip:hover { background: {{color.canon.raised}}; }
QPushButton#statusChip[tone="danger"] { color: {{color.accent.danger}}; border-color: {{color.accent.danger}}; }
QPushButton#statusChip[tone="warning"] { color: {{color.accent.warning}}; border-color: {{color.accent.warning}}; }
QPushButton#statusChip[tone="success"] { color: {{color.accent.success}}; border-color: {{color.accent.success}}; }
)"));
    connect(this, &QPushButton::clicked, this, [this] {
        if (!m_summary.page.isEmpty()) emit pageRequested(m_summary.page);
    });
    showStatus(EngineStatus{});
}

void StatusChip::setRecordingFailureAnswered(bool answered)
{
    if (answered == m_failureAnswered) return;
    m_failureAnswered = answered;
    const EngineStatus last = m_status;
    showStatus(last);
}

void StatusChip::showStatus(const EngineStatus& s)
{
    m_status = s;
    const Summary next = summarize(s, m_failureAnswered);
    const bool toneChanged = next.tone != m_summary.tone;
    const bool textChanged = next.text != m_summary.text;
    m_summary = next;
    if (toneChanged) {
        // The tone is a property the sheet selects on, so it takes a re-polish to show.
        setProperty("tone", m_summary.tone);
        style()->unpolish(this);
        style()->polish(this);
    }
    if (textChanged) {
        refreshText();
        updateGeometry();
    }

    // Every message in full, as the pages show them.
    QStringList lines;
    for (const QString& line : {statusText::radio(s), statusText::mute(s), statusText::output(s), statusText::nr(s),
                                statusText::recording(s)}) {
        if (!line.isEmpty()) lines << line;
    }
    const QString tip = lines.join(QStringLiteral("<br>"));
    if (tip != toolTip()) setToolTip(tip);
}

int StatusChip::chrome() const
{
    return QPushButton::sizeHint().width() - fontMetrics().horizontalAdvance(text());
}

QSize StatusChip::sizeHint() const
{
    // 2 px spare: the advance is rounded, and eliding goes by the unrounded width.
    const int whole = fontMetrics().horizontalAdvance(m_summary.text) + chrome() + 2;
    return {std::clamp(whole, kMinWidth, kMaxWidth), QPushButton::sizeHint().height()};
}

QSize StatusChip::minimumSizeHint() const
{
    return {std::min(kMinWidth, sizeHint().width()), QPushButton::minimumSizeHint().height()};
}

void StatusChip::resizeEvent(QResizeEvent* event)
{
    QPushButton::resizeEvent(event);
    refreshText();
}

void StatusChip::refreshText()
{
    const QString shown = fontMetrics().elidedText(m_summary.text, Qt::ElideRight, width() - chrome());
    if (shown != text()) setText(shown);
}

} // namespace esa
