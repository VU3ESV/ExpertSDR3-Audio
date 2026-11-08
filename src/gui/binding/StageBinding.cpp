#include "gui/binding/StageBinding.h"

#include "dsp/RxChain.h"
#include "dsp/StageSet.h"
#include "gui/ParamWidgets.h"

#include <QTimer>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <utility>

namespace esa {

namespace {

// Enough for a pull's own side effects to settle (the gate's mode presetting ratio and
// floor takes one more pass); a value that kept changing on every pass would otherwise
// loop for ever.
constexpr int kMaxPasses = 4;

const ParamSpec* stageSpec(const QString& key)
{
    static const QHash<QString, const ParamSpec*> byKey = [] {
        QHash<QString, const ParamSpec*> map;
        for (const ParamSpec& spec : RxChain::stageParams()) map.insert(spec.key, &spec);
        return map;
    }();
    return byKey.value(key, nullptr);
}

double halfStep(const ParamSpec& spec)
{
    return 0.5 * std::pow(10.0, -spec.decimals);
}

double rounded(double value, int decimals)
{
    const double scale = std::pow(10.0, decimals);
    return std::round(value * scale) / scale;
}

} // namespace

StageBinding::StageBinding(StageSet* view, Host host, QObject* parent)
    : QObject(parent), m_view(view), m_host(std::move(host))
{
#ifdef NDEBUG
    if (qEnvironmentVariableIsSet("ESA_CHECK_MIRROR")) checkEvery(1000);
#else
    checkEvery(1000);
#endif
}

void StageBinding::fillHooks(AetherSDR::AudioEngine::Hooks* hooks)
{
    hooks->gate = &m_view->gate;
    hooks->eq = &m_view->eq;
    hooks->comp = &m_view->comp;
    hooks->tube = &m_view->tube;
    hooks->pudu = &m_view->pudu;
    hooks->saveStage = [this](const QString& stage) { pull(stage); };
}

QString StageBinding::stageOf(const QString& key)
{
    return stageSpec(key) ? key.section(QLatin1Char('.'), 0, 0) : QString();
}

void StageBinding::own(const QString& stage, const QStringList& keys)
{
    QStringList owned;
    for (const ParamSpec& spec : RxChain::stageParams()) {
        if (keys.contains(spec.key) && stageOf(spec.key) == stage) owned.append(spec.key);
    }
    m_owned.insert(stage, owned);
}

QStringList StageBinding::ownedKeys(const QString& stage) const
{
    return m_owned.value(stage);
}

void StageBinding::goLive()
{
    m_live = true;
}

void StageBinding::pull(const QString& stage)
{
    if (!m_live) return;
    m_pending.insert(stage);
    if (m_pulling) return;  // the walk under way goes round again for it

    m_pulling = true;
    for (int pass = 0; pass < kMaxPasses && !m_pending.isEmpty(); ++pass) {
        const QSet<QString> stages = std::exchange(m_pending, {});
        for (const ParamSpec& spec : RxChain::stageParams()) {
            const QString id = stageOf(spec.key);
            if (stages.contains(id)) pullKey(spec.key, m_owned.value(id).contains(spec.key));
        }
    }
    m_pending.clear();
    m_pulling = false;
}

void StageBinding::pullKey(const QString& key, bool owned)
{
    const ParamSpec* spec = stageSpec(key);
    double read = 0.0;
    if (!spec || !RxChain::readStageParam(*m_view, key, &read)) return;
    const double held = m_host.value(key);
    // Within half a step it is the same value, read back through a float. Compared before
    // rounding, so a value kept with more decimals than the table's (from --set) is not
    // rewritten by the float it passed through.
    if (std::abs(read - held) < halfStep(*spec)) return;

    const double value = std::clamp(rounded(read, spec->decimals), spec->min, spec->max);
    ParamControl* control = owned && m_host.control ? m_host.control(key) : nullptr;
    if (control && std::abs(value - held) >= halfStep(*spec)) {
        control->setValue(value);  // reaches the window, which writes it back into the mirror
    } else {
        // A key the page does not own goes one way, from the settings to the page, so
        // whatever the page wrote there itself is put back. So is a value that clamps to
        // what is held already.
        RxChain::applyStageParam(*m_view, key, held);
    }
}

void StageBinding::apply(const QString& key, double value)
{
    RxChain::applyStageParam(*m_view, key, value);
}

void StageBinding::seed()
{
    for (const ParamSpec& spec : RxChain::stageParams()) {
        RxChain::applyStageParam(*m_view, spec.key, m_host.value(spec.key));
    }
}

QString StageBinding::firstMismatch() const
{
    for (const ParamSpec& spec : RxChain::stageParams()) {
        double read = 0.0;
        if (!RxChain::readStageParam(*m_view, spec.key, &read)) continue;
        const double held = m_host.value(spec.key);
        if (std::abs(read - held) >= halfStep(spec)) {
            return QStringLiteral("%1: the pages show %2, the settings hold %3")
                .arg(spec.key)
                .arg(read, 0, 'g', 8)
                .arg(held, 0, 'g', 8);
        }
    }
    return QString();
}

void StageBinding::checkEvery(int ms)
{
    if (!m_check) {
        m_check = new QTimer(this);
        connect(m_check, &QTimer::timeout, this, [this] {
            const QString mismatch = firstMismatch();
            if (!mismatch.isEmpty() && mismatch != m_lastLogged) {
                qWarning("The display mirror has drifted from the settings: %s", qPrintable(mismatch));
            }
            m_lastLogged = mismatch;
        });
    }
    m_check->start(ms);
}

} // namespace esa
