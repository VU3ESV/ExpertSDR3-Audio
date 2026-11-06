#include "dsp/RxChain.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

using AetherSDR::ClientEq;
using AetherSDR::ClientGate;
using AetherSDR::ClientPudu;
using AetherSDR::ClientTube;

namespace esa {

namespace {

float blockRms(const QByteArray& pcm)
{
    const auto samples = pcm.size() / qsizetype(sizeof(float));
    if (samples == 0) return 0.0f;
    const auto* data = reinterpret_cast<const float*>(pcm.constData());
    double sum = 0.0;
    for (qsizetype i = 0; i < samples; ++i) sum += double(data[i]) * data[i];
    return float(std::sqrt(sum / double(samples)));
}

struct StageParam {
    ParamSpec spec;
    std::function<void(RxChain&, double)> apply;
};

QString bandKey(int band, const char* leaf)
{
    return QStringLiteral("eq.b%1.%2").arg(band).arg(QLatin1String(leaf));
}

// Band fields are written through a read-modify-write of the whole band.
void updateBand(RxChain& chain, int band, const std::function<void(ClientEq::BandParams&)>& edit)
{
    ClientEq::BandParams params = chain.eq().band(band);
    edit(params);
    chain.eq().setBand(band, params);
}

QString bandTypeLabel(ClientEq::FilterType type)
{
    switch (type) {
    case ClientEq::FilterType::HighPass: return QStringLiteral("High-pass");
    case ClientEq::FilterType::LowShelf: return QStringLiteral("Low shelf");
    case ClientEq::FilterType::Peak: return QStringLiteral("Peak");
    case ClientEq::FilterType::HighShelf: return QStringLiteral("High shelf");
    case ClientEq::FilterType::LowPass: return QStringLiteral("Low-pass");
    }
    return QString();
}

QList<StageParam> buildStageParams()
{
    QList<StageParam> params;
    const auto add = [&](const ParamSpec& spec, std::function<void(RxChain&, double)> apply) {
        params.append({spec, std::move(apply)});
    };

    // ── Gate ─ mode must stay ahead of ratio and floor: upstream's setMode() is a
    // preset that overwrites both, and settings load in this order. ───────────
    add(toggleParam(QStringLiteral("gate.enabled"), QStringLiteral("Enabled"), false),
        [](RxChain& c, double v) { c.gate().setEnabled(v != 0.0); });
    add(choiceParam(QStringLiteral("gate.mode"), QStringLiteral("Mode"),
                    {QStringLiteral("Expander"), QStringLiteral("Gate")}, 0),
        [](RxChain& c, double v) { c.gate().setMode(static_cast<ClientGate::Mode>(int(v))); });
    add({QStringLiteral("gate.threshold"), QStringLiteral("Threshold"), -80, 0, -40, 1, QStringLiteral("dB")},
        [](RxChain& c, double v) { c.gate().setThresholdDb(float(v)); });
    add({QStringLiteral("gate.ratio"), QStringLiteral("Ratio"), 1, 10, 2, 1, QStringLiteral(":1")},
        [](RxChain& c, double v) { c.gate().setRatio(float(v)); });
    add({QStringLiteral("gate.attack"), QStringLiteral("Attack"), 0.1, 100, ClientGate::kAttackMs, 1, QStringLiteral("ms")},
        [](RxChain& c, double v) { c.gate().setAttackMs(float(v)); });
    add({QStringLiteral("gate.release"), QStringLiteral("Release"), 5, 2000, 100, 0, QStringLiteral("ms")},
        [](RxChain& c, double v) { c.gate().setReleaseMs(float(v)); });
    add({QStringLiteral("gate.hold"), QStringLiteral("Hold"), 0, 500, 20, 0, QStringLiteral("ms")},
        [](RxChain& c, double v) { c.gate().setHoldMs(float(v)); });
    add({QStringLiteral("gate.floor"), QStringLiteral("Floor"), -80, 0, -15, 1, QStringLiteral("dB")},
        [](RxChain& c, double v) { c.gate().setFloorDb(float(v)); });
    add({QStringLiteral("gate.return"), QStringLiteral("Hysteresis"), 0, 20, 2, 1, QStringLiteral("dB")},
        [](RxChain& c, double v) { c.gate().setReturnDb(float(v)); });
    add({QStringLiteral("gate.lookahead"), QStringLiteral("Look-ahead"), 0, 5, 0, 1, QStringLiteral("ms")},
        [](RxChain& c, double v) { c.gate().setLookaheadMs(float(v)); });

    // ── EQ ───────────────────────────────────────────────────────────────────
    add(toggleParam(QStringLiteral("eq.enabled"), QStringLiteral("Enabled"), false),
        [](RxChain& c, double v) { c.eq().setEnabled(v != 0.0); });
    add({QStringLiteral("eq.master"), QStringLiteral("Output"), -24, 12, 0, 1, QStringLiteral("dB")},
        [](RxChain& c, double v) { c.eq().setMasterGain(float(std::pow(10.0, v / 20.0))); });
    add(choiceParam(QStringLiteral("eq.family"), QStringLiteral("HP/LP family"),
                    {QStringLiteral("Butterworth"), QStringLiteral("Chebyshev"),
                     QStringLiteral("Bessel"), QStringLiteral("Elliptic")}, 0),
        [](RxChain& c, double v) { c.eq().setFilterFamily(static_cast<ClientEq::FilterFamily>(int(v))); });

    for (int band = 0; band < ClientEq::kDefaultBandCount; ++band) {
        const ClientEq::BandParams def = ClientEq::defaultBand(band);
        const QString name = QStringLiteral("%1 %2").arg(band + 1).arg(bandTypeLabel(def.type));
        add(toggleParam(bandKey(band, "on"), name, false),
            [band](RxChain& c, double v) { updateBand(c, band, [v](auto& p) { p.enabled = v != 0.0; }); });
        add({bandKey(band, "freq"), QStringLiteral("Frequency"), 20, 20000, double(def.freqHz), 0, QStringLiteral("Hz")},
            [band](RxChain& c, double v) { updateBand(c, band, [v](auto& p) { p.freqHz = float(v); }); });
        add({bandKey(band, "gain"), QStringLiteral("Gain"), -24, 24, 0, 1, QStringLiteral("dB")},
            [band](RxChain& c, double v) { updateBand(c, band, [v](auto& p) { p.gainDb = float(v); }); });
        add({bandKey(band, "q"), QStringLiteral("Q"), 0.1, 18, double(def.q), 2},
            [band](RxChain& c, double v) { updateBand(c, band, [v](auto& p) { p.q = float(v); }); });
    }

    // ── Comp ─────────────────────────────────────────────────────────────────
    add(toggleParam(QStringLiteral("comp.enabled"), QStringLiteral("Enabled"), false),
        [](RxChain& c, double v) { c.comp().setEnabled(v != 0.0); });
    add({QStringLiteral("comp.threshold"), QStringLiteral("Threshold"), -60, 0, -18, 1, QStringLiteral("dB")},
        [](RxChain& c, double v) { c.comp().setThresholdDb(float(v)); });
    add({QStringLiteral("comp.ratio"), QStringLiteral("Ratio"), 1, 20, 3, 1, QStringLiteral(":1")},
        [](RxChain& c, double v) { c.comp().setRatio(float(v)); });
    add({QStringLiteral("comp.attack"), QStringLiteral("Attack"), 0.1, 300, 20, 1, QStringLiteral("ms")},
        [](RxChain& c, double v) { c.comp().setAttackMs(float(v)); });
    add({QStringLiteral("comp.release"), QStringLiteral("Release"), 5, 2000, 200, 0, QStringLiteral("ms")},
        [](RxChain& c, double v) { c.comp().setReleaseMs(float(v)); });
    add({QStringLiteral("comp.knee"), QStringLiteral("Knee"), 0, 24, 6, 1, QStringLiteral("dB")},
        [](RxChain& c, double v) { c.comp().setKneeDb(float(v)); });
    add({QStringLiteral("comp.makeup"), QStringLiteral("Makeup"), -12, 24, 0, 1, QStringLiteral("dB")},
        [](RxChain& c, double v) { c.comp().setMakeupDb(float(v)); });
    add({QStringLiteral("comp.drive"), QStringLiteral("Drive"), 0, 18, 0, 1, QStringLiteral("dB")},
        [](RxChain& c, double v) { c.comp().setDriveDb(float(v)); });
    add({QStringLiteral("comp.rotator"), QStringLiteral("Phase rotator"), 0, 6, 0, 0, QStringLiteral("stages")},
        [](RxChain& c, double v) { c.comp().setPhaseRotatorStages(int(v)); });
    add(toggleParam(QStringLiteral("comp.limiter"), QStringLiteral("Limiter"), true),
        [](RxChain& c, double v) { c.comp().setLimiterEnabled(v != 0.0); });
    add({QStringLiteral("comp.ceiling"), QStringLiteral("Limiter ceiling"), -24, 0, -1, 1, QStringLiteral("dB")},
        [](RxChain& c, double v) { c.comp().setLimiterCeilingDb(float(v)); });

    // ── Tube ─────────────────────────────────────────────────────────────────
    add(toggleParam(QStringLiteral("tube.enabled"), QStringLiteral("Enabled"), false),
        [](RxChain& c, double v) { c.tube().setEnabled(v != 0.0); });
    add(choiceParam(QStringLiteral("tube.model"), QStringLiteral("Model"),
                    {QStringLiteral("A: soft"), QStringLiteral("B: hard + soft"), QStringLiteral("C: asymmetric")}, 0),
        [](RxChain& c, double v) { c.tube().setModel(static_cast<ClientTube::Model>(int(v))); });
    add({QStringLiteral("tube.drive"), QStringLiteral("Drive"), 0, 24, 0, 1, QStringLiteral("dB")},
        [](RxChain& c, double v) { c.tube().setDriveDb(float(v)); });
    add({QStringLiteral("tube.bias"), QStringLiteral("Bias"), 0, 1, 0, 2},
        [](RxChain& c, double v) { c.tube().setBiasAmount(float(v)); });
    add({QStringLiteral("tube.tone"), QStringLiteral("Tone"), -1, 1, 0, 2},
        [](RxChain& c, double v) { c.tube().setTone(float(v)); });
    add({QStringLiteral("tube.output"), QStringLiteral("Output"), -24, 24, 0, 1, QStringLiteral("dB")},
        [](RxChain& c, double v) { c.tube().setOutputGainDb(float(v)); });
    add({QStringLiteral("tube.mix"), QStringLiteral("Dry/wet"), 0, 1, 1, 2},
        [](RxChain& c, double v) { c.tube().setDryWet(float(v)); });
    add({QStringLiteral("tube.envelope"), QStringLiteral("Envelope"), -1, 1, 0, 2},
        [](RxChain& c, double v) { c.tube().setEnvelopeAmount(float(v)); });
    add({QStringLiteral("tube.attack"), QStringLiteral("Attack"), 0.1, 30, ClientTube::kAttackMs, 1, QStringLiteral("ms")},
        [](RxChain& c, double v) { c.tube().setAttackMs(float(v)); });
    add({QStringLiteral("tube.release"), QStringLiteral("Release"), 10, 500, 35, 0, QStringLiteral("ms")},
        [](RxChain& c, double v) { c.tube().setReleaseMs(float(v)); });

    // ── Pudu (AetherVoice exciter) ───────────────────────────────────────────
    add(toggleParam(QStringLiteral("pudu.enabled"), QStringLiteral("Enabled"), false),
        [](RxChain& c, double v) { c.pudu().setEnabled(v != 0.0); });
    add(choiceParam(QStringLiteral("pudu.mode"), QStringLiteral("Mode"),
                    {QStringLiteral("Aural exciter + bottom"), QStringLiteral("Sonic exciter")}, 0),
        [](RxChain& c, double v) { c.pudu().setMode(static_cast<ClientPudu::Mode>(int(v))); });
    add({QStringLiteral("pudu.pooDrive"), QStringLiteral("Low drive"), 0, 24, 0, 1, QStringLiteral("dB")},
        [](RxChain& c, double v) { c.pudu().setPooDriveDb(float(v)); });
    add({QStringLiteral("pudu.pooTune"), QStringLiteral("Low tune"), 50, 160, 100, 0, QStringLiteral("Hz")},
        [](RxChain& c, double v) { c.pudu().setPooTuneHz(float(v)); });
    add({QStringLiteral("pudu.pooMix"), QStringLiteral("Low mix"), 0, 1, 0.5, 2},
        [](RxChain& c, double v) { c.pudu().setPooMix(float(v)); });
    add({QStringLiteral("pudu.dooTune"), QStringLiteral("High tune"), 1000, 10000, 5000, 0, QStringLiteral("Hz")},
        [](RxChain& c, double v) { c.pudu().setDooTuneHz(float(v)); });
    add({QStringLiteral("pudu.dooHarmonics"), QStringLiteral("Harmonics"), 0, 24, 6, 1, QStringLiteral("dB")},
        [](RxChain& c, double v) { c.pudu().setDooHarmonicsDb(float(v)); });
    add({QStringLiteral("pudu.dooMix"), QStringLiteral("High mix"), 0, 1, 0.5, 2},
        [](RxChain& c, double v) { c.pudu().setDooMix(float(v)); });

    return params;
}

const QList<StageParam>& stageParamTable()
{
    static const QList<StageParam> table = buildStageParams();
    return table;
}

} // namespace

RxChain::RxChain(int sampleRate)
    : m_sampleRate(sampleRate), m_order(stageIds())
{
    m_gate.prepare(sampleRate);
    m_eq.prepare(sampleRate);
    m_comp.prepare(sampleRate);
    m_tube.prepare(sampleRate);
    m_pudu.prepare(sampleRate);

    m_eq.setActiveBandCount(ClientEq::kDefaultBandCount);
    for (int band = 0; band < ClientEq::kDefaultBandCount; ++band) {
        m_eq.setBand(band, ClientEq::defaultBand(band));
    }
}

QStringList RxChain::stageIds()
{
    return {QStringLiteral("gate"), QStringLiteral("eq"), QStringLiteral("comp"),
            QStringLiteral("tube"), QStringLiteral("pudu")};
}

QString RxChain::stageLabel(const QString& id)
{
    if (id == QLatin1String("gate")) return QStringLiteral("Gate");
    if (id == QLatin1String("eq")) return QStringLiteral("EQ");
    if (id == QLatin1String("comp")) return QStringLiteral("Comp");
    if (id == QLatin1String("tube")) return QStringLiteral("Tube");
    if (id == QLatin1String("pudu")) return QStringLiteral("AetherVoice");
    return id;
}

const QList<ParamSpec>& RxChain::stageParams()
{
    static const QList<ParamSpec> specs = [] {
        QList<ParamSpec> list;
        for (const StageParam& param : stageParamTable()) list.append(param.spec);
        return list;
    }();
    return specs;
}

bool RxChain::setStageParam(const QString& key, double value)
{
    // Lazily built lookup; the table is immutable after first use.
    static const QHash<QString, const StageParam*> byKey = [] {
        QHash<QString, const StageParam*> map;
        for (const StageParam& param : stageParamTable()) map.insert(param.spec.key, &param);
        return map;
    }();
    const auto it = byKey.constFind(key);
    if (it == byKey.constEnd()) return false;
    (*it)->apply(*this, value);
    return true;
}

void RxChain::setStageOrder(const QStringList& order)
{
    const QStringList known = stageIds();
    QStringList result;
    for (const QString& id : order) {
        if (known.contains(id) && !result.contains(id)) result.append(id);
    }
    for (const QString& id : known) {
        if (!result.contains(id)) result.append(id);
    }
    m_order = result;
}

void RxChain::setNrProcessor(std::shared_ptr<NrProcessor> processor)
{
    m_nr = std::move(processor);
    m_nrGain = 1.0f;
}

void RxChain::setNrBypassed(bool bypassed)
{
    if (bypassed == m_nrBypassed) return;
    m_nrBypassed = bypassed;
    if (!bypassed && m_nr) m_nr->reset();
}

void RxChain::setTransmitBypass(bool bypassed)
{
    if (bypassed == m_txBypassed) return;
    m_txBypassed = bypassed;
    if (!bypassed && m_nr) m_nr->resetTransient();
}

void RxChain::reset()
{
    if (m_nr) m_nr->reset();
    m_gate.reset();
    m_eq.reset();
    m_comp.reset();
    m_tube.reset();
    m_pudu.reset();
}

const QByteArray& RxChain::process(const QByteArray& stereoIn)
{
    m_nrActive = false;
    if (m_txBypassed) {
        m_nrGain = 1.0f;
        m_output = stereoIn;
        return m_output;
    }

    if (m_nr && !m_nrBypassed) {
        m_nr->process(stereoIn, m_output);
        m_nrActive = true;
        // A block too quiet to divide by, or a buffering call that returned
        // nothing, keeps the previous reading.
        const float before = blockRms(stereoIn);
        if (before > 1e-6f && !m_output.isEmpty()) {
            m_nrGain = std::min(blockRms(m_output) / before, 1.0f);
        }
    } else {
        m_output = stereoIn;
        m_nrGain = 1.0f;
    }

    const int frames = int(m_output.size() / (2 * sizeof(float)));
    if (frames <= 0) return m_output;

    auto* samples = reinterpret_cast<float*>(m_output.data());
    for (const QString& stage : std::as_const(m_order)) {
        if (stage == QLatin1String("gate")) {
            if (m_gate.isEnabled()) m_gate.process(samples, frames, 2);
        } else if (stage == QLatin1String("eq")) {
            if (m_eq.isEnabled()) m_eq.process(samples, frames, 2);
        } else if (stage == QLatin1String("comp")) {
            if (m_comp.isEnabled()) m_comp.process(samples, frames, 2);
        } else if (stage == QLatin1String("tube")) {
            if (m_tube.isEnabled()) m_tube.process(samples, frames, 2);
        } else if (stage == QLatin1String("pudu")) {
            if (m_pudu.isEnabled()) m_pudu.process(samples, frames, 2);
        }
    }
    return m_output;
}

} // namespace esa
