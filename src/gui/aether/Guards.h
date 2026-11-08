#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

class QAbstractButton;
class QLineEdit;
class QWidget;

namespace esa {

// Corrections to AetherSDR's widgets made from outside, by event filter, so the vendored
// files stay exactly as upstream wrote them. test_aether_gui checks each one, so a sync
// that changes the behaviour underneath shows there.

// A trackpad sends wheel steps of a few units where a mouse sends 120 a notch. AetherSDR's
// knobs, threshold fader and makeup meter divide by 120 and drop the remainder, so on a
// trackpad they never move. This sums the small steps per widget and hands the widget one
// whole 120-unit step per notch reached, swallowing the originals.
class WheelAccumulator : public QObject {
    Q_OBJECT

public:
    explicit WheelAccumulator(QObject* parent = nullptr);

    // Watches every ClientCompKnob, ClientCompThresholdFader and ClientCompMeter under
    // `root`, with their value fields, which hand their wheel steps on to them. The
    // accumulator is a child of `root`.
    static WheelAccumulator* install(QWidget* root);

    // Watches `target`, and each of its text fields as part of it.
    void watch(QWidget* target);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QHash<QObject*, QWidget*> m_targets;  // a watched widget or field -> the widget it steps
    QHash<QWidget*, int> m_pending;       // the part of a notch not yet handed on, per widget
    bool m_sending{false};
};

// The value field on AetherSDR's knobs and faders shows the bare number while it has the
// keyboard and the formatted reading otherwise, and its widget commits whatever the field
// holds on Return and again each time it loses the keyboard. By then the widget has put the
// reading back, so leaving a field nobody typed in rounds the value to what the reading
// shows: a Release of 182.1 ms becomes 182, and a ratio of 3.7 reads "3.7:1" back as 3.71.
// Where '.' groups thousands, the bare "20.000" it shows is read as 20000.
// This lets a field commit only text that differs from what it showed, once, on Return or
// on losing the keyboard, and a plain number with a '.' point is read as the field wrote it.
class ValueFieldGuard : public QObject {
    Q_OBJECT

public:
    explicit ValueFieldGuard(QObject* parent = nullptr);

    // Guards the value field of every ClientCompKnob, ClientCompThresholdFader and
    // ClientEqOutputFader under `root`. The guard is a child of `root`.
    static ValueFieldGuard* install(QWidget* root);

    void watch(QLineEdit* field);
    QList<QLineEdit*> fields() const;

    // `text` as the field's widget should read it. A plain number with a '.' point is what
    // the field writes, so it means what it does in C, even where the locale would read
    // the '.' as grouping thousands; it is rewritten in the locale's own form if so.
    static QString readAsWritten(const QString& text);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // Hands `event` to `field` past this filter, with the field's signals blocked if
    // `quiet`, so its own commit does not run.
    void deliver(QLineEdit* field, QEvent* event, bool quiet);

    QHash<QObject*, QString> m_shown;  // per field: what it showed on taking the keyboard, or last committed
    QObject* m_delivering{nullptr};
};

// A Gate or Expander button that is already lit re-applies its mode when clicked again, and
// a mode is a preset that overwrites ratio and floor, losing whatever the operator tuned.
// This swallows clicks and key presses on whichever of the buttons is checked, as a combo
// box ignores choosing the entry it already shows, and silences the clicks that come from
// code, a shortcut or an accessibility action, which never pass as mouse or key events.
class LitModeGuard : public QObject {
    Q_OBJECT

public:
    explicit LitModeGuard(QObject* parent = nullptr);

    // Guards each of `buttons`; the guard is a child of the first.
    static LitModeGuard* install(const QList<QAbstractButton*>& buttons);

    void watch(QAbstractButton* button);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // Blocks a lit button's signals from its pressed() until the click is over.
    void hold(QAbstractButton* button);
    void releaseWhenDone(QAbstractButton* button);

    QSet<QAbstractButton*> m_held;
};

} // namespace esa
