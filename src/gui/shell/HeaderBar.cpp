#include "gui/shell/HeaderBar.h"

#include "gui/shell/StatusChip.h"

#include "CanonGround.h"
#include "core/ThemeManager.h"

#include <QComboBox>
#include <QDir>
#include <QHBoxLayout>
#include <QLayoutItem>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

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

QPushButton#exactValues {
    background: transparent;
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 10px;
}
QPushButton#exactValues:hover { background: {{color.canon.raised}}; }
QPushButton#exactValues:disabled { border-color: {{color.canon.line}}; }

QLabel#profileState {
    color: {{color.accent.warning}};
    background: transparent;
    border: 1px solid {{color.accent.warning}};
    border-radius: 10px;
    padding: 0 8px;
    font-size: 12px;
    font-weight: 600;
}

/* REC's and PLAY's clocks: the colours of their buttons on the rail. No padding, so the
   width a clock asks for is its text's. */
QLabel#recordClock, QLabel#playClock {
    background: transparent;
    font-size: 12px;
    font-weight: 600;
}
QLabel#recordClock { color: {{color.accent.danger}}; }
QLabel#recordClock[tone="warning"] { color: {{color.accent.warning}}; }
QLabel#playClock { color: {{color.accent.success}}; }
)");
}

// A clock in the header's row. As tall as the row, like every control there. Short of room it
// shows a shorter form, the time alone, and then shortens that with an ellipsis, rather than
// widening the window; the tooltip says what it is about either way.
class HeaderClock final : public QLabel {
public:
    explicit HeaderClock(const QString& name)
    {
        setObjectName(name);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
        hide();
    }

    QString fullText() const { return m_full; }
    // The width its shorter form takes whole, and what showing that saves; nothing while hidden.
    int shortWidth() const
    {
        if (isHidden()) return 0;
        return fontMetrics().horizontalAdvance(m_compact.isEmpty() ? m_full : m_compact) + 2;
    }
    int shorterBy() const { return isHidden() ? 0 : std::max(0, sizeHint().width() - shortWidth()); }
    // An empty `full` hides it.
    void setTexts(const QString& full, const QString& compact)
    {
        if (full == m_full && compact == m_compact) return;
        m_full = full;
        m_compact = compact;
        setVisible(!full.isEmpty());
        refresh();
        updateGeometry();
    }

    void setTone(const QString& tone)
    {
        if (property("tone").toString() == tone) return;
        // A property the sheet selects on, so it takes a re-polish to show.
        setProperty("tone", tone);
        style()->unpolish(this);
        style()->polish(this);
    }

    // From the whole text, so shortening it never changes what it asks for.
    QSize sizeHint() const override
    {
        // 2 px spare: the advance is rounded, and eliding goes by the unrounded width.
        return {fontMetrics().horizontalAdvance(m_full) + 2, QLabel::sizeHint().height()};
    }
    // Nothing at all, if that is what it takes: in a window at its narrowest, with the profile
    // marked Modified and a long status, the row has no room left for the clocks, and the
    // rail's REC and PLAY still show the state.
    QSize minimumSizeHint() const override { return {0, QLabel::minimumSizeHint().height()}; }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        refresh();
    }

private:
    void refresh()
    {
        const int room = contentsRect().width();
        QString shown = m_full;
        if (fontMetrics().horizontalAdvance(m_full) > room) {
            shown = fontMetrics().elidedText(m_compact.isEmpty() ? m_full : m_compact, Qt::ElideRight, room);
        }
        if (shown != text()) setText(shown);
    }

    QString m_full;
    QString m_compact;
};

// The profile list. Made narrower than its name by a full row, it shows the name shortened
// with an ellipsis, where a combo box would cut it off mid-letter.
class ProfileCombo final : public QComboBox {
public:
    // How wide it must be to show the current name whole: the name and what the style puts
    // round it, the arrow included. Never more than it asks for.
    int wholeNameWidth() const
    {
        const int asked = sizeHint().width();
        QStyleOptionComboBox option;
        initStyleOption(&option);
        option.rect.setWidth(asked);
        const QRect field = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
        // 2 px spare: the advance is rounded, and eliding goes by the unrounded width.
        return std::min(asked, fontMetrics().horizontalAdvance(currentText()) + asked - field.width() + 2);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        // QComboBox's own painting, with the text elided to the field it is drawn in.
        QStylePainter painter(this);
        painter.setPen(palette().color(QPalette::Text));
        QStyleOptionComboBox option;
        initStyleOption(&option);
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        const QRect field = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
        option.currentText = option.fontMetrics.elidedText(option.currentText, Qt::ElideRight, field.width());
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
    }
};

// "⋯", drawn rather than typed, because the typeface may not have the character, and with
// a dot in the corner when the page leaves out a changed value.
class ExactValuesButton final : public QPushButton {
public:
    ExactValuesButton()
    {
        setObjectName(QStringLiteral("exactValues"));
        setAccessibleName(QStringLiteral("Exact values"));
        setCursor(Qt::PointingHandCursor);
        // A chip, like the status chip beside it: the rail is where the keyboard goes.
        setFocusPolicy(Qt::NoFocus);
        setFixedWidth(30);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    }

    bool flagged() const { return m_flagged; }
    void setFlagged(bool on)
    {
        if (on == m_flagged) return;
        m_flagged = on;
        setAccessibleDescription(on ? QStringLiteral("Some values this page does not show are changed") : QString());
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPushButton::paintEvent(event);
        auto& theme = AetherSDR::ThemeManager::instance();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(theme.color(this, isEnabled() ? QStringLiteral("color.canon.inkSoft")
                                                 : QStringLiteral("color.button.foreground.disabled")));
        const QPointF centre = QRectF(rect()).center();
        for (int i = -1; i <= 1; ++i) p.drawEllipse(centre + QPointF(i * 5.0, 0.0), 1.4, 1.4);
        if (m_flagged && isEnabled()) {
            p.setBrush(theme.color(this, QStringLiteral("color.accent.warning")));
            p.drawEllipse(QPointF(width() - 6.0, 5.0), 3.0, 3.0);
        }
    }

private:
    bool m_flagged{false};
};

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
    // Upstream keeps the title clear of a close button in the corner. Here whatever ends the
    // row, a close chip included, is in the row itself.
    m_title->setContentsMargins(0, 0, 0, 0);
    // AetherRX's row height, the title's line on a Mac and on Linux. Windows draws the same
    // font a pixel shorter, which would lift every page a pixel off AetherRX's.
    m_title->setMinimumHeight(kRowHeight);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    col->insertLayout(0, row);
    m_row = row;
    row->addWidget(m_title);
    row->addSpacing(6);

    // Every control below ignores its own height and takes the row's, which is the title's.
    m_profiles = new ProfileCombo;
    m_profiles->setObjectName(QStringLiteral("headerProfile"));
    m_profiles->setAccessibleName(QStringLiteral("Profile"));
    m_profiles->setToolTip(QStringLiteral(
        "A saved set of everything on the stage pages: noise reduction, its settings, and the stages."));
    // As wide as a usual name whatever the longest one is, so a long name cannot widen the window.
    m_profiles->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_profiles->setMinimumContentsLength(16);
    m_profiles->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    row->addWidget(m_profiles);
    // Another name may need more or less of it (fitRow).
    connect(m_profiles, &QComboBox::currentTextChanged, this, [this] { fitRow(); });

    m_profileState = new QLabel;
    m_profileState->setObjectName(QStringLiteral("profileState"));
    m_profileState->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    m_profileState->hide();
    row->addWidget(m_profileState);

    row->addStretch(1);

    m_recordClock = new HeaderClock(QStringLiteral("recordClock"));
    m_recordClock->setAccessibleName(QStringLiteral("Recording time"));
    row->addWidget(m_recordClock);
    m_playClock = new HeaderClock(QStringLiteral("playClock"));
    m_playClock->setAccessibleName(QStringLiteral("Playback time"));
    row->addWidget(m_playClock);

    m_status = new StatusChip;
    row->addWidget(m_status);

    m_exactValues = new ExactValuesButton;
    row->addWidget(m_exactValues);
    setExactValues(false, false);

    AetherSDR::ThemeManager::instance().applyStyleSheet(this, headerSheet());
}

void HeaderBar::setProfileState(const QString& text, const QString& toolTip)
{
    m_profileState->setText(text);
    m_profileState->setToolTip(toolTip);
    m_profileState->setVisible(!text.isEmpty());
    fitRow();
}

QString HeaderBar::profileState() const
{
    return m_profileState->isHidden() ? QString() : m_profileState->text();
}

void HeaderBar::setExactValues(bool available, bool flagged)
{
    auto* button = static_cast<ExactValuesButton*>(m_exactValues);
    button->setEnabled(available);
    button->setFlagged(available && flagged);
    QString tip;
    if (available) {
        tip = QStringLiteral("Exact values: every setting of this stage, including those its page does not show.");
        if (flagged) tip += QStringLiteral("\nSome of those the page does not show are changed from their defaults.");
    } else {
        tip = QStringLiteral("Exact values are for the stage pages.");
    }
    button->setToolTip(tip);
}

bool HeaderBar::exactValuesFlagged() const
{
    return static_cast<const ExactValuesButton*>(m_exactValues)->flagged();
}

void HeaderBar::showActivity(const EngineStatus& s)
{
    auto* record = static_cast<HeaderClock*>(m_recordClock);
    record->setTexts(recordingText(s), s.recording.active ? QStringLiteral("● ") + clockText(s.recording.seconds) : QString());
    if (s.recording.active) {
        // Amber once the disk has fallen behind, as the status chip says it.
        const bool lost = s.recording.droppedFrames > 0;
        record->setTone(lost ? QStringLiteral("warning") : QString());
        QString tip = s.recording.path.isEmpty()
                          ? QStringLiteral("Recording: waiting for audio from the radio")
                          : QStringLiteral("Recording to %1").arg(QDir::toNativeSeparators(s.recording.path));
        if (lost) tip += QStringLiteral("\nThe disk could not keep up, so some audio was lost.");
        if (tip != record->toolTip()) record->setToolTip(tip);
    }

    auto* play = static_cast<HeaderClock*>(m_playClock);
    play->setTexts(playbackText(s), s.playback.active ? QStringLiteral("▶ ") + clockText(s.playback.seconds) : QString());
    if (s.playback.active) {
        // The file playing, not the last one recorded: a recording that finishes meanwhile is
        // the next PLAY's.
        const QString tip = s.playback.waiting
                                ? QStringLiteral("Waiting for the recording to finish, then playing it")
                                : QStringLiteral("Playing %1").arg(QDir::toNativeSeparators(s.playback.path));
        if (tip != play->toolTip()) play->setToolTip(tip);
    }
    fitRow();
}

void HeaderBar::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    fitRow();
}

QSize HeaderBar::minimumSizeHint() const
{
    // What fitRow holds the clocks and the combo to fits the row into the width it has. The
    // header's minimum, and so the window's, is the row's without the clocks and with the
    // combo as it asks, so that showing a clock never widens the window or stops a drag short.
    QSize size = QWidget::minimumSizeHint();
    const auto least = [this](QWidget* w) {
        const int index = m_row->indexOf(w);
        return index < 0 ? 0 : m_row->itemAt(index)->minimumSize().width();
    };
    int held = least(m_profiles) - m_profiles->minimumSizeHint().width();
    for (QWidget* clock : {static_cast<QWidget*>(m_recordClock), static_cast<QWidget*>(m_playClock)}) {
        if (!clock->isHidden()) held += least(clock) + m_row->spacing();
    }
    size.setWidth(size.width() - held);
    return size;
}

// With a clock showing and too little room, the profile combo first gives up what its name
// does not need. Then the status chip is shortened, down to its own minimum, then the combo,
// down to half its width, then the clocks, PLAY's before REC's, to their time alone, and then
// the combo again, down to a quarter, before a clock is cut short: the chip's tooltip has its
// whole text, the combo's list has every name whole, and a clock cut short says nothing. A
// layout takes the same from every widget that can give, so each is held to what it is to
// have once the rest have what they ask for.
void HeaderBar::fitRow()
{
    auto* record = static_cast<HeaderClock*>(m_recordClock);
    auto* play = static_cast<HeaderClock*>(m_playClock);
    int statusCap = QWIDGETSIZE_MAX;
    int profilesCap = QWIDGETSIZE_MAX;
    int recordLeast = 0;  // 0: as the layout gives it
    int playLeast = 0;
    const int width = m_row->geometry().width();
    if (width > 0 && (!record->isHidden() || !play->isHidden())) {
        // What the row asks for with the chip, the combo and the clocks at their whole widths,
        // whatever they are held to now, as the layout adds it up: hidden widgets count for
        // nothing, and spacing goes between what is shown.
        int wanted = 0;
        int shown = 0;
        for (int i = 0; i < m_row->count(); ++i) {
            QLayoutItem* item = m_row->itemAt(i);
            if (item->widget() && item->isEmpty()) continue;
            QWidget* w = item->widget();
            const bool held = w == m_status || w == m_profiles || w == record || w == play;
            wanted += held ? w->sizeHint().width() : item->sizeHint().width();
            if (!item->isEmpty()) ++shown;
        }
        wanted += m_row->spacing() * std::max(0, shown - 1);
        int excess = wanted - width;
        const int whole = m_profiles->sizeHint().width();
        int profiles = whole;
        // Takes from the combo what the row still lacks, down to `floor` at most.
        const auto narrowProfiles = [&](int floor) {
            const int gives = std::max(0, std::min(excess, profiles - floor));
            profiles -= gives;
            excess -= gives;
        };
        narrowProfiles(static_cast<ProfileCombo*>(m_profiles)->wholeNameWidth());
        if (excess > 0) {
            const int statusGives = std::max(0, std::min(excess, m_status->sizeHint().width()
                                                                     - m_status->minimumSizeHint().width()));
            statusCap = m_status->sizeHint().width() - statusGives;
            excess -= statusGives;
        }
        if (excess > 0) {
            narrowProfiles(whole - whole / 2);
            if (excess > 0) {
                // Each clock whole if it can be, REC first, else its time alone.
                const int playGives = std::min(excess, play->shorterBy());
                excess -= playGives;
                const int recordGives = std::min(excess, record->shorterBy());
                excess -= recordGives;
                narrowProfiles(whole - whole / 2 - (whole / 2 - whole / 4));
                if (excess <= 0) {
                    recordLeast = recordGives > 0 ? record->shortWidth() : record->sizeHint().width();
                    playLeast = playGives > 0 ? play->shortWidth() : play->sizeHint().width();
                }
            }
        }
        if (profiles < whole) profilesCap = profiles;
    }
    bool changed = false;
    const auto hold = [&changed](QWidget* w, int least, int most) {
        if (least == w->minimumWidth() && most == w->maximumWidth()) return;
        w->setMinimumWidth(least);
        w->setMaximumWidth(most);
        changed = true;
    };
    hold(record, record->isHidden() ? 0 : recordLeast, QWIDGETSIZE_MAX);
    hold(play, play->isHidden() ? 0 : playLeast, QWIDGETSIZE_MAX);
    hold(m_status, m_status->minimumWidth(), statusCap);
    // Below its own minimum only while capped; uncapped, the combo asks for its usual width.
    hold(m_profiles, profilesCap == QWIDGETSIZE_MAX ? 0 : profilesCap, profilesCap);
    // Laid out again now rather than at the next turn of the event loop, so the row is never
    // seen with a widget resized and its neighbours not yet moved.
    QLayout* outer = m_row->parentWidget() ? m_row->parentWidget()->layout() : nullptr;
    if (changed && outer) outer->activate();
}

QString HeaderBar::recordingClock() const
{
    return static_cast<const HeaderClock*>(m_recordClock)->fullText();
}

QString HeaderBar::playbackClock() const
{
    return static_cast<const HeaderClock*>(m_playClock)->fullText();
}

QString HeaderBar::recordingText(const EngineStatus& s)
{
    if (!s.recording.active) return QString();
    return QStringLiteral("● REC ") + clockText(s.recording.seconds);
}

QString HeaderBar::playbackText(const EngineStatus& s)
{
    if (!s.playback.active) return QString();
    QString text = QStringLiteral("▶ ") + clockText(s.playback.seconds);
    // The length is known once the file has been read.
    if (s.playback.totalSeconds > 0.0) text += QStringLiteral(" / ") + clockText(s.playback.totalSeconds);
    return text;
}

QString HeaderBar::clockText(double seconds)
{
    // Seconds summed from frames can land a hair under a whole second.
    const qint64 whole = std::isfinite(seconds) && seconds > 0.0 ? qint64(std::floor(seconds + 1e-6)) : 0;
    const qint64 h = whole / 3600;
    const qint64 m = whole / 60 % 60;
    const qint64 sec = whole % 60;
    const QString mmss = QStringLiteral("%1:%2").arg(m, 2, 10, QLatin1Char('0')).arg(sec, 2, 10, QLatin1Char('0'));
    return h > 0 ? QStringLiteral("%1:%2").arg(h).arg(mmss) : mmss;
}

void HeaderBar::setWindowButtons(bool shown)
{
    if (!m_close) {
        if (!shown) return;
        // AetherSDR's round close button, twice, as tall as the row: the title's height,
        // so the row, and the pages under it, stay where upstream's are.
        const int side = m_title->sizeHint().height();
        m_minimise = AetherSDR::makeCanonCloseButton(this);
        m_minimise->setObjectName(QStringLiteral("headerMinimise"));
        m_minimise->setText(QStringLiteral("–"));
        m_minimise->setAccessibleName(QStringLiteral("Minimise"));
        m_minimise->setToolTip(QStringLiteral("Minimise"));
        m_close = AetherSDR::makeCanonCloseButton(this);
        m_close->setObjectName(QStringLiteral("headerClose"));
        // Upstream's sheet, with the radius half this side rather than half of its 26 px: Qt
        // squares a sheet's corners once two radii add up to more than the side.
        const QString sheet = QStringLiteral(
            "QToolButton { background: transparent; color: {{color.canon.muted}}; border: 1px solid transparent; "
            "border-radius: %1px; font-size: 12px; }"
            "QToolButton:hover { background: {{color.canon.nested}}; color: {{color.canon.ink}}; "
            "border-color: {{color.canon.lineHi}}; }").arg(side / 2);
        for (QToolButton* chip : {m_minimise, m_close}) {
            chip->setFixedSize(side, side);
            chip->setFocusPolicy(Qt::NoFocus);
            AetherSDR::ThemeManager::instance().applyStyleSheet(chip, sheet);
            m_row->addWidget(chip);
        }
        connect(m_minimise, &QToolButton::clicked, this, [this] { window()->showMinimized(); });
        connect(m_close, &QToolButton::clicked, this, [this] { window()->close(); });
    }
    m_minimise->setVisible(shown);
    m_close->setVisible(shown);
}

} // namespace esa
