#include "Guards.h"

#include "ClientCompKnob.h"
#include "ClientCompMeter.h"
#include "ClientCompThresholdFader.h"
#include "ClientEqOutputFader.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLocale>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>

#include <cstdlib>
#include <utility>

namespace esa {

namespace {

// What one notch of a mouse wheel reports, and what the vendored widgets divide by.
constexpr int kNotch = 120;

} // namespace

// ── WheelAccumulator ────────────────────────────────────────────────────────

WheelAccumulator::WheelAccumulator(QObject* parent)
    : QObject(parent)
{
}

WheelAccumulator* WheelAccumulator::install(QWidget* root)
{
    auto* guard = new WheelAccumulator(root);
    QList<QWidget*> targets;
    for (QWidget* w : root->findChildren<AetherSDR::ClientCompKnob*>()) targets.append(w);
    for (QWidget* w : root->findChildren<AetherSDR::ClientCompThresholdFader*>()) targets.append(w);
    for (QWidget* w : root->findChildren<AetherSDR::ClientCompMeter*>()) targets.append(w);
    for (QWidget* target : std::as_const(targets)) guard->watch(target);
    return guard;
}

void WheelAccumulator::watch(QWidget* target)
{
    // A value field inside the widget passes its wheel steps straight to the widget's own
    // handler, past anything installed on the widget, so it is watched as the widget.
    QList<QWidget*> watched{target};
    for (QWidget* field : target->findChildren<QLineEdit*>()) watched.append(field);
    for (QWidget* w : std::as_const(watched)) {
        m_targets.insert(w, target);
        // Installed last, so it runs before the widget's own filter on its field.
        w->installEventFilter(this);
        connect(w, &QObject::destroyed, this, [this, w] { m_targets.remove(w); });
    }
    connect(target, &QObject::destroyed, this, [this, target] { m_pending.remove(target); });
}

bool WheelAccumulator::eventFilter(QObject* watched, QEvent* event)
{
    // The whole notches this hands on come back through here: let them pass.
    if (event->type() != QEvent::Wheel || m_sending) return QObject::eventFilter(watched, event);
    QWidget* target = m_targets.value(watched);
    if (!target) return QObject::eventFilter(watched, event);

    auto* wheel = static_cast<QWheelEvent*>(event);
    const int delta = wheel->angleDelta().y();
    int& pending = m_pending[target];
    // Turning back starts afresh, rather than first paying off what was left the other way.
    if ((pending > 0 && delta < 0) || (pending < 0 && delta > 0)) pending = 0;
    pending += delta;
    const int notches = pending / kNotch;
    pending -= notches * kNotch;

    if (notches != 0) {
        const QPointF at = watched == target ? wheel->position()
                                             : static_cast<QWidget*>(watched)->mapTo(target, wheel->position());
        const QPoint step(0, notches > 0 ? kNotch : -kNotch);
        m_sending = true;
        for (int i = 0; i < std::abs(notches); ++i) {
            QWheelEvent notch(at, wheel->globalPosition(), QPoint(), step, wheel->buttons(), wheel->modifiers(),
                              Qt::NoScrollPhase, wheel->inverted(), Qt::MouseEventSynthesizedByApplication,
                              wheel->pointingDevice());
            QCoreApplication::sendEvent(target, &notch);
        }
        m_sending = false;
    }
    // Taken, so a page under the widget does not scroll with what the widget did not use.
    event->accept();
    return true;
}

// ── ValueFieldGuard ─────────────────────────────────────────────────────────

ValueFieldGuard::ValueFieldGuard(QObject* parent)
    : QObject(parent)
{
}

ValueFieldGuard* ValueFieldGuard::install(QWidget* root)
{
    auto* guard = new ValueFieldGuard(root);
    QList<QWidget*> owners;
    for (QWidget* w : root->findChildren<AetherSDR::ClientCompKnob*>()) owners.append(w);
    for (QWidget* w : root->findChildren<AetherSDR::ClientCompThresholdFader*>()) owners.append(w);
    for (QWidget* w : root->findChildren<AetherSDR::ClientEqOutputFader*>()) owners.append(w);
    for (QWidget* owner : std::as_const(owners)) {
        for (QLineEdit* field : owner->findChildren<QLineEdit*>()) guard->watch(field);
    }
    return guard;
}

void ValueFieldGuard::watch(QLineEdit* field)
{
    if (m_shown.contains(field)) return;
    m_shown.insert(field, field->text());
    // Installed after the widget's own filter, so it runs first.
    field->installEventFilter(this);
    connect(field, &QObject::destroyed, this, [this, field] { m_shown.remove(field); });
}

QList<QLineEdit*> ValueFieldGuard::fields() const
{
    QList<QLineEdit*> out;
    for (auto it = m_shown.cbegin(); it != m_shown.cend(); ++it) out.append(static_cast<QLineEdit*>(it.key()));
    return out;
}

QString ValueFieldGuard::readAsWritten(const QString& text)
{
    static const QRegularExpression plain(QStringLiteral("^\\s*[+-]?[0-9]*\\.?[0-9]+\\s*$"));
    if (!plain.match(text).hasMatch()) return text;
    bool written = false;
    const double value = QLocale::c().toDouble(text.trimmed(), &written);
    bool local = false;
    const double read = QLocale().toDouble(text.trimmed(), &local);
    // The widget tries the locale first and falls back to C, so only a locale that reads
    // the text as some other number needs it rewritten.
    if (!written || !local || read == value) return text;
    QLocale locale;
    locale.setNumberOptions(QLocale::OmitGroupSeparator);
    return locale.toString(value, 'f', QLocale::FloatingPointShortest);
}

void ValueFieldGuard::deliver(QLineEdit* field, QEvent* event, bool quiet)
{
    QObject* const outer = m_delivering;
    m_delivering = field;
    {
        const QSignalBlocker blocker(quiet ? field : nullptr);
        QCoreApplication::sendEvent(field, event);
    }
    m_delivering = outer;
}

bool ValueFieldGuard::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_delivering || !m_shown.contains(watched)) return QObject::eventFilter(watched, event);
    auto* field = static_cast<QLineEdit*>(watched);

    switch (event->type()) {
    case QEvent::FocusIn:
        // The widget puts the bare number in as the field takes the keyboard.
        deliver(field, event, false);
        m_shown[field] = field->text();
        return true;

    case QEvent::FocusOut: {
        const QString text = field->text();  // before the widget puts its reading back
        // The field's own commit would read that reading; it never runs.
        deliver(field, event, true);
        // A popup taking the keyboard for a moment, such as the field's own menu, is not
        // the end of the edit.
        if (static_cast<QFocusEvent*>(event)->reason() == Qt::PopupFocusReason) return true;
        if (text != m_shown.value(field)) {
            const QString read = readAsWritten(text);
            m_shown[field] = read;
            {
                const QSignalBlocker blocker(field);
                field->setText(read);
            }
            // The widget's own commit on leaving the field, once, on what was typed.
            emit field->editingFinished();
        }
        return true;
    }

    case QEvent::KeyPress: {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Escape) {
            // The widget throws the edit away and takes the keyboard from its field, and
            // what is left there is not to be committed.
            m_shown[field] = field->text();
            return QObject::eventFilter(watched, event);
        }
        if (key != Qt::Key_Return && key != Qt::Key_Enter) return QObject::eventFilter(watched, event);
        const QString text = field->text();
        if (text != m_shown.value(field)) {
            const QString read = readAsWritten(text);
            // Set first: a widget that takes the keyboard from its field on Return sees
            // nothing left to commit when it goes.
            m_shown[field] = read;
            if (read != text) {
                const QSignalBlocker blocker(field);
                field->setText(read);
            }
            // The widget's own commit on Return, once, on what was typed.
            emit field->returnPressed();
        }
        event->accept();
        return true;
    }

    default:
        return QObject::eventFilter(watched, event);
    }
}

// ── LitModeGuard ────────────────────────────────────────────────────────────

LitModeGuard::LitModeGuard(QObject* parent)
    : QObject(parent)
{
}

LitModeGuard* LitModeGuard::install(const QList<QAbstractButton*>& buttons)
{
    if (buttons.isEmpty() || !buttons.constFirst()) return nullptr;
    auto* guard = new LitModeGuard(buttons.constFirst());
    for (QAbstractButton* button : buttons) {
        if (button) guard->watch(button);
    }
    return guard;
}

void LitModeGuard::watch(QAbstractButton* button)
{
    button->installEventFilter(this);
    // click() and animateClick(), which a shortcut and an accessibility press use, pass no
    // mouse or key event, but each starts with pressed() and ends with clicked().
    connect(button, &QAbstractButton::pressed, this, [this, button] { hold(button); });
    connect(button, &QObject::destroyed, this, [this, button] { m_held.remove(button); });
}

void LitModeGuard::hold(QAbstractButton* button)
{
    if (!button->isChecked() || m_held.contains(button)) return;
    // Its clicked() is the one that re-applies the mode: the button says nothing until the
    // click is over. Blocked from inside pressed(), so the rest of pressed() still goes out.
    m_held.insert(button);
    button->blockSignals(true);
    releaseWhenDone(button);
}

void LitModeGuard::releaseWhenDone(QAbstractButton* button)
{
    // click() is over by the time the event loop runs again. animateClick() keeps the
    // button down until its timer fires, and the filter calls this again then.
    QTimer::singleShot(0, button, [this, button] {
        if (!m_held.contains(button) || button->isDown()) return;
        m_held.remove(button);
        button->blockSignals(false);
    });
}

bool LitModeGuard::eventFilter(QObject* watched, QEvent* event)
{
    auto* button = qobject_cast<QAbstractButton*>(watched);
    // The timer that ends an animated click, and with it the click.
    if (button && event->type() == QEvent::Timer && m_held.contains(button)) releaseWhenDone(button);
    if (!button || !button->isChecked()) return QObject::eventFilter(watched, event);

    bool swallow = false;
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
        swallow = true;
        break;
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        // The keys that press a button where it has the focus.
        const int key = static_cast<QKeyEvent*>(event)->key();
        swallow = key == Qt::Key_Space || key == Qt::Key_Select;
        break;
    }
    default:
        break;
    }
    if (!swallow) return QObject::eventFilter(watched, event);
    event->accept();
    return true;
}

} // namespace esa
