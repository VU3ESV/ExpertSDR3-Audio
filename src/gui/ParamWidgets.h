#pragma once

#include "dsp/ParamSpec.h"

#include <QList>
#include <QObject>
#include <QString>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QSlider;
class QWidget;

namespace esa {

// One parameter's control, built entirely from its ParamSpec: a checkbox for a
// toggle, a combo box for choices, otherwise a slider paired with a spin box.
// Wide positive ranges (frequencies) get a logarithmic slider. A spec marked percent
// shows its 0..1 value as 0..100 % in the spin box; the value itself stays 0..1.
class ParamControl : public QObject {
    Q_OBJECT

public:
    // Adds its widgets to `grid` at `row`.
    ParamControl(const ParamSpec& spec, double value, QGridLayout* grid, int row, QObject* parent);
    // The compact form, for one cell of a table: the checkbox (with its label), the combo
    // box or the spin box alone, at `row`, `column`. No slider and no separate label.
    ParamControl(const ParamSpec& spec, double value, QGridLayout* grid, int row, int column, QObject* parent);

    const ParamSpec& spec() const { return m_spec; }
    double value() const { return m_value; }
    // Updates the display and notifies, as if the operator had moved it.
    void setValue(double value);
    // Updates the display only, for when the value is already applied elsewhere.
    void setValueSilently(double value);
    // Enables or greys out every widget it is drawn with.
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }
    // Every widget it is drawn with: its label (the row form's), then the checkbox, combo
    // box, slider or spin box.
    QList<QWidget*> widgets() const;

signals:
    void valueChanged(const QString& key, double value);
    // The display changed to `value`, whether the operator moved it or it was set from
    // elsewhere (a profile load) without notifying.
    void valueShown(double value);

private:
    void apply(double value, bool notify);
    // To the spec's decimals, so float noise from a drag never reaches the settings.
    double rounded(double value) const;
    int toSlider(double value) const;
    double fromSlider(int position) const;
    void makeSpin(double value);
    // The spin box's number for a value: the value, or for a percent spec, 100 times it.
    double spinScale() const { return m_spec.percent ? 100.0 : 1.0; }

    ParamSpec m_spec;
    double m_value{0.0};
    bool m_logarithmic{false};
    bool m_enabled{true};
    QLabel* m_label{nullptr};
    QCheckBox* m_check{nullptr};
    QComboBox* m_combo{nullptr};
    QSlider* m_slider{nullptr};
    QDoubleSpinBox* m_spin{nullptr};
};

// Greys each control in `controls` whose spec names an enabledByKey, for as long as the
// control for that key, also in `controls`, holds anything other than enabledByValue: the
// noise fill's sliders do nothing while the fill is off.
void followEnabledBy(const QList<ParamControl*>& controls);

} // namespace esa
