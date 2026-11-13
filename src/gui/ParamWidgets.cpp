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
    : QObject(parent), m_spec(spec), m_value(rounded(value))
{
    if (spec.toggle) {
        m_check = new QCheckBox(spec.label);
        m_check->setChecked(m_value != 0.0);
        grid->addWidget(m_check, row, 0, 1, 3);
        connect(m_check, &QCheckBox::toggled, this, [this](bool on) { apply(on ? 1.0 : 0.0, true); });
        return;
    }

    m_label = new QLabel(spec.label);
    // Named so a page's sheet can set the row labels apart from its other labels.
    m_label->setObjectName(QStringLiteral("ParamLabel"));
    grid->addWidget(m_label, row, 0);

    if (!spec.choices.isEmpty()) {
        m_combo = new QComboBox;
        m_combo->addItems(spec.choices);
        m_combo->setCurrentIndex(int(m_value));
        grid->addWidget(m_combo, row, 1, 1, 2);
        connect(m_combo, &QComboBox::currentIndexChanged, this, [this](int index) { apply(index, true); });
        return;
    }

    m_logarithmic = spec.min > 0.0 && spec.max / spec.min >= 100.0;

    m_slider = new QSlider(Qt::Horizontal);
    m_slider->setRange(0, kSliderSteps);
    m_slider->setValue(toSlider(m_value));
    grid->addWidget(m_slider, row, 1);

    makeSpin(m_value);
    m_spin->setMinimumWidth(100);
    grid->addWidget(m_spin, row, 2);

    connect(m_slider, &QSlider::valueChanged, this, [this](int position) { apply(fromSlider(position), true); });
}

ParamControl::ParamControl(const ParamSpec& spec, double value, QGridLayout* grid, int row, int column,
                           QObject* parent)
    : QObject(parent), m_spec(spec), m_value(rounded(value))
{
    if (spec.toggle) {
        m_check = new QCheckBox(spec.label);
        m_check->setChecked(m_value != 0.0);
        grid->addWidget(m_check, row, column);
        connect(m_check, &QCheckBox::toggled, this, [this](bool on) { apply(on ? 1.0 : 0.0, true); });
    } else if (!spec.choices.isEmpty()) {
        m_combo = new QComboBox;
        m_combo->addItems(spec.choices);
        m_combo->setCurrentIndex(int(m_value));
        grid->addWidget(m_combo, row, column);
        connect(m_combo, &QComboBox::currentIndexChanged, this, [this](int index) { apply(index, true); });
    } else {
        makeSpin(m_value);
        grid->addWidget(m_spin, row, column);
    }
}

void ParamControl::makeSpin(double value)
{
    // A percent spec's 0.01 steps are whole percent: two fewer decimals on show.
    const double scale = spinScale();
    const int decimals = m_spec.percent ? std::max(0, m_spec.decimals - 2) : m_spec.decimals;
    m_spin = new QDoubleSpinBox;
    m_spin->setRange(m_spec.min * scale, m_spec.max * scale);
    m_spin->setDecimals(decimals);
    m_spin->setSingleStep(std::pow(10.0, -decimals));
    m_spin->setKeyboardTracking(false);
    m_spin->setValue(value * scale);
    if (m_spec.percent) {
        m_spin->setSuffix(QStringLiteral(" %"));
    } else if (!m_spec.unit.isEmpty()) {
        m_spin->setSuffix((m_spec.unit.startsWith(QLatin1Char(':')) ? QString() : QStringLiteral(" ")) + m_spec.unit);
    }
    connect(m_spin, &QDoubleSpinBox::valueChanged, this, [this](double v) { apply(v / spinScale(), true); });
}

double ParamControl::rounded(double value) const
{
    const double scale = std::pow(10.0, m_spec.decimals);
    return std::round(value * scale) / scale;
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
    return rounded(raw);
}

void ParamControl::apply(double value, bool notify)
{
    // Rounded first, so a value just outside the range still lands on its edge.
    value = std::clamp(rounded(value), m_spec.min, m_spec.max);
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
        m_spin->setValue(value * spinScale());
    }
    emit valueShown(value);
    if (notify) emit valueChanged(m_spec.key, value);
}

void ParamControl::setValue(double value)
{
    apply(value, true);
}

void ParamControl::setValueSilently(double value)
{
    apply(value, false);
}

void ParamControl::setEnabled(bool enabled)
{
    m_enabled = enabled;
    for (QWidget* widget : widgets()) widget->setEnabled(enabled);
}

QList<QWidget*> ParamControl::widgets() const
{
    QList<QWidget*> all;
    for (QWidget* widget : {static_cast<QWidget*>(m_label), static_cast<QWidget*>(m_check),
                            static_cast<QWidget*>(m_combo), static_cast<QWidget*>(m_slider),
                            static_cast<QWidget*>(m_spin)}) {
        if (widget) all.append(widget);
    }
    return all;
}

void followEnabledBy(const QList<ParamControl*>& controls)
{
    for (ParamControl* control : controls) {
        const ParamSpec& spec = control->spec();
        if (spec.enabledByKey.isEmpty()) continue;
        const auto by = std::find_if(controls.begin(), controls.end(), [&](const ParamControl* c) {
            return c->spec().key == spec.enabledByKey;
        });
        if (by == controls.end()) continue;
        ParamControl* controller = *by;
        // Within half the controller's last decimal counts as the value.
        const double tolerance = 0.5 * std::pow(10.0, -controller->spec().decimals);
        const auto follow = [control, tolerance, want = spec.enabledByValue](double value) {
            control->setEnabled(std::abs(value - want) < tolerance);
        };
        follow(controller->value());
        QObject::connect(controller, &ParamControl::valueShown, control, follow);
    }
}

} // namespace esa
