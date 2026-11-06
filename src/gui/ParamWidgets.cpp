#include "gui/ParamWidgets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>

#include <algorithm>
#include <cmath>

namespace esa {

namespace {
constexpr int kSliderSteps = 1000;
}

ParamControl::ParamControl(const ParamSpec& spec, double value, QGridLayout* grid, int row, QObject* parent)
    : QObject(parent), m_spec(spec), m_value(value)
{
    if (spec.toggle) {
        m_check = new QCheckBox(spec.label);
        m_check->setChecked(value != 0.0);
        grid->addWidget(m_check, row, 0, 1, 3);
        connect(m_check, &QCheckBox::toggled, this, [this](bool on) { apply(on ? 1.0 : 0.0, true); });
        return;
    }

    grid->addWidget(new QLabel(spec.label), row, 0);

    if (!spec.choices.isEmpty()) {
        m_combo = new QComboBox;
        m_combo->addItems(spec.choices);
        m_combo->setCurrentIndex(int(value));
        grid->addWidget(m_combo, row, 1, 1, 2);
        connect(m_combo, &QComboBox::currentIndexChanged, this, [this](int index) { apply(index, true); });
        return;
    }

    m_logarithmic = spec.min > 0.0 && spec.max / spec.min >= 100.0;

    m_slider = new QSlider(Qt::Horizontal);
    m_slider->setRange(0, kSliderSteps);
    m_slider->setValue(toSlider(value));
    grid->addWidget(m_slider, row, 1);

    m_spin = new QDoubleSpinBox;
    m_spin->setRange(spec.min, spec.max);
    m_spin->setDecimals(spec.decimals);
    m_spin->setSingleStep(std::pow(10.0, -spec.decimals));
    m_spin->setKeyboardTracking(false);
    m_spin->setValue(value);
    if (!spec.unit.isEmpty()) {
        m_spin->setSuffix((spec.unit.startsWith(QLatin1Char(':')) ? QString() : QStringLiteral(" ")) + spec.unit);
    }
    m_spin->setMinimumWidth(100);
    grid->addWidget(m_spin, row, 2);

    connect(m_slider, &QSlider::valueChanged, this, [this](int position) { apply(fromSlider(position), true); });
    connect(m_spin, &QDoubleSpinBox::valueChanged, this, [this](double v) { apply(v, true); });
}

int ParamControl::toSlider(double value) const
{
    const double t = m_logarithmic
                         ? std::log(value / m_spec.min) / std::log(m_spec.max / m_spec.min)
                         : (value - m_spec.min) / (m_spec.max - m_spec.min);
    return int(std::lround(std::clamp(t, 0.0, 1.0) * kSliderSteps));
}

double ParamControl::fromSlider(int position) const
{
    const double t = double(position) / kSliderSteps;
    const double raw = m_logarithmic ? m_spec.min * std::pow(m_spec.max / m_spec.min, t)
                                     : m_spec.min + (m_spec.max - m_spec.min) * t;
    const double scale = std::pow(10.0, m_spec.decimals);
    return std::round(raw * scale) / scale;
}

void ParamControl::apply(double value, bool notify)
{
    value = std::clamp(value, m_spec.min, m_spec.max);
    if (value == m_value && !notify) return;
    m_value = value;

    if (m_check) {
        const QSignalBlocker block(m_check);
        m_check->setChecked(value != 0.0);
    }
    if (m_combo) {
        const QSignalBlocker block(m_combo);
        m_combo->setCurrentIndex(int(value));
    }
    if (m_slider) {
        const QSignalBlocker block(m_slider);
        m_slider->setValue(toSlider(value));
    }
    if (m_spin) {
        const QSignalBlocker block(m_spin);
        m_spin->setValue(value);
    }
    if (notify) emit valueChanged(m_spec.key, value);
}

void ParamControl::setValue(double value)
{
    apply(value, true);
}

} // namespace esa
