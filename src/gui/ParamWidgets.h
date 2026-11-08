#pragma once

#include "dsp/ParamSpec.h"

#include <QObject>
#include <QString>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QSlider;

namespace esa {

// One parameter's control, built entirely from its ParamSpec: a checkbox for a
// toggle, a combo box for choices, otherwise a slider paired with a spin box.
// Wide positive ranges (frequencies) get a logarithmic slider.
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

signals:
    void valueChanged(const QString& key, double value);

private:
    void apply(double value, bool notify);
    // To the spec's decimals, so float noise from a drag never reaches the settings.
    double rounded(double value) const;
    int toSlider(double value) const;
    double fromSlider(int position) const;
    void makeSpin(double value);

    ParamSpec m_spec;
    double m_value{0.0};
    bool m_logarithmic{false};
    QCheckBox* m_check{nullptr};
    QComboBox* m_combo{nullptr};
    QSlider* m_slider{nullptr};
    QDoubleSpinBox* m_spin{nullptr};
};

} // namespace esa
