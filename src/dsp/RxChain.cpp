#include "dsp/RxChain.h"

#include "audio/SpscRing.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
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

// Copies a block into the chain's own buffer, which keeps its allocation from block to block.
// Assigning the input instead would share it, and the first write by a stage would then copy
// it into a fresh allocation on every block, on the engine thread.
void copyBlock(const QByteArray& in, QByteArray& out)
{
    if (&in == &out) return;
    out.resize(in.size());
    if (!in.isEmpty()) std::memcpy(out.data(), in.constData(), size_t(in.size()));
}

// One table entry: what the setting is, how to set it on a set of stages, and how to
// read it back from one. A pair, so the window's display mirror and the engine's
// chain use the same mapping and cannot drift apart.
struct StageParam {
    ParamSpec spec;
    std::function<void(StageSet&, double)> apply;
    std::function<double(const StageSet&)> read;
};

QString bandKey(int band, const char* leaf)
{
    return QStringLiteral("eq.b%1.%2").arg(band).arg(QLatin1String(leaf));
}

// Band fields are written through a read-modify-write of the whole band.
void updateBand(StageSet& stages, int band, const std::function<void(ClientEq::BandParams&)>& edit)
{
    ClientEq::BandParams params = stages.eq.band(band);
    edit(params);
    stages.eq.setBand(band, params);
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

// eq.bN.type's choices, low to high as the EQ draws them. The stored value is the
// index here, not upstream's enum value, which is in another order.
constexpr ClientEq::FilterType kBandTypes[] = {
    ClientEq::FilterType::HighPass, ClientEq::FilterType::LowShelf, ClientEq::FilterType::Peak,
    ClientEq::FilterType::HighShelf, ClientEq::FilterType::LowPass,
};
constexpr int kBandTypeCount = int(std::size(kBandTypes));

ClientEq::FilterType bandTypeAt(double index)
{
    return kBandTypes[std::clamp(int(index), 0, kBandTypeCount - 1)];
}

int bandTypeIndex(ClientEq::FilterType type)
{
    for (int i = 0; i < kBandTypeCount; ++i) {
        if (kBandTypes[i] == type) return i;
    }
    return 2;  // Peak; unreachable for any type upstream defines
}

QStringList bandTypeChoices()
{
    QStringList choices;
    for (ClientEq::FilterType type : kBandTypes) choices.append(bandTypeLabel(type));
    return choices;
}

// eq.bN.slope's choices: high- and low-pass steepness, one biquad per 12 dB/oct.
constexpr int kSlopes[] = {12, 24, 36, 48};
constexpr int kSlopeCount = int(std::size(kSlopes));

// The DSP rounds an in-between slope down to whole sections, so read it the same way.
int slopeIndex(int slopeDbPerOct)
{
    for (int i = kSlopeCount - 1; i > 0; --i) {
        if (slopeDbPerOct >= kSlopes[i]) return i;
    }
    return 0;
}

double flag(bool on)
{
    return on ? 1.0 : 0.0;
}

QList<StageParam> buildStageParams()
{
    using Apply = std::function<void(StageSet&, double)>;
    using Read = std::function<double(const StageSet&)>;
    QList<StageParam> params;
    const auto add = [&](const ParamSpec& spec, Apply apply, Read read) {
        params.append({spec, std::move(apply), std::move(read)});
    };

    // ── Gate ─ mode must stay ahead of ratio and floor: upstream's setMode() is a
    // preset that overwrites both, and settings load in this order. ───────────
    add(toggleParam(QStringLiteral("gate.enabled"), QStringLiteral("Enabled"), false),
        [](StageSet& s, double v) { s.gate.setEnabled(v != 0.0); },
        [](const StageSet& s) { return flag(s.gate.isEnabled()); });
    add(choiceParam(QStringLiteral("gate.mode"), QStringLiteral("Mode"),
                    {QStringLiteral("Expander"), QStringLiteral("Gate")}, 0),
        [](StageSet& s, double v) { s.gate.setMode(static_cast<ClientGate::Mode>(int(v))); },
        [](const StageSet& s) { return double(int(s.gate.mode())); });
    add({QStringLiteral("gate.threshold"), QStringLiteral("Threshold"), -80, 0, -40, 1, QStringLiteral("dB")},
        [](StageSet& s, double v) { s.gate.setThresholdDb(float(v)); },
        [](const StageSet& s) { return double(s.gate.thresholdDb()); });
    add({QStringLiteral("gate.ratio"), QStringLiteral("Ratio"), 1, 10, 2, 1, QStringLiteral(":1")},
        [](StageSet& s, double v) { s.gate.setRatio(float(v)); },
        [](const StageSet& s) { return double(s.gate.ratio()); });
    add({QStringLiteral("gate.attack"), QStringLiteral("Attack"), 0.1, 100, ClientGate::kAttackMs, 1, QStringLiteral("ms")},
        [](StageSet& s, double v) { s.gate.setAttackMs(float(v)); },
        [](const StageSet& s) { return double(s.gate.attackMs()); });
    add({QStringLiteral("gate.release"), QStringLiteral("Release"), 5, 2000, 100, 1, QStringLiteral("ms")},
        [](StageSet& s, double v) { s.gate.setReleaseMs(float(v)); },
        [](const StageSet& s) { return double(s.gate.releaseMs()); });
    add({QStringLiteral("gate.hold"), QStringLiteral("Hold"), 0, 500, 20, 1, QStringLiteral("ms")},
        [](StageSet& s, double v) { s.gate.setHoldMs(float(v)); },
        [](const StageSet& s) { return double(s.gate.holdMs()); });
    add({QStringLiteral("gate.floor"), QStringLiteral("Floor"), -80, 0, -15, 1, QStringLiteral("dB")},
        [](StageSet& s, double v) { s.gate.setFloorDb(float(v)); },
        [](const StageSet& s) { return double(s.gate.floorDb()); });
    add({QStringLiteral("gate.return"), QStringLiteral("Hysteresis"), 0, 20, 2, 2, QStringLiteral("dB")},
        [](StageSet& s, double v) { s.gate.setReturnDb(float(v)); },
        [](const StageSet& s) { return double(s.gate.returnDb()); });
    add({QStringLiteral("gate.lookahead"), QStringLiteral("Look-ahead"), 0, 5, 0, 1, QStringLiteral("ms")},
        [](StageSet& s, double v) { s.gate.setLookaheadMs(float(v)); },
        [](const StageSet& s) { return double(s.gate.lookaheadMs()); });

    // ── EQ ───────────────────────────────────────────────────────────────────
    add(toggleParam(QStringLiteral("eq.enabled"), QStringLiteral("Enabled"), false),
        [](StageSet& s, double v) { s.eq.setEnabled(v != 0.0); },
        [](const StageSet& s) { return flag(s.eq.isEnabled()); });
    add({QStringLiteral("eq.master"), QStringLiteral("Output"), -24, 12, 0, 1, QStringLiteral("dB")},
        [](StageSet& s, double v) { s.eq.setMasterGain(float(std::pow(10.0, v / 20.0))); },
        // The DSP allows silence (a gain of 0), which the table cannot: read it as very quiet.
        [](const StageSet& s) { return 20.0 * std::log10(std::max(double(s.eq.masterGain()), 1e-6)); });
    add(choiceParam(QStringLiteral("eq.family"), QStringLiteral("HP/LP family"),
                    {QStringLiteral("Butterworth"), QStringLiteral("Chebyshev"),
                     QStringLiteral("Bessel"), QStringLiteral("Elliptic")}, 0),
        [](StageSet& s, double v) { s.eq.setFilterFamily(static_cast<ClientEq::FilterFamily>(int(v))); },
        [](const StageSet& s) { return double(int(s.eq.filterFamily())); });

    for (int band = 0; band < ClientEq::kDefaultBandCount; ++band) {
        const ClientEq::BandParams def = ClientEq::defaultBand(band);
        // No type in the name: a band's type can change.
        const QString name = QStringLiteral("Band %1").arg(band + 1);
        add(toggleParam(bandKey(band, "on"), name, false),
            [band](StageSet& s, double v) { updateBand(s, band, [v](auto& p) { p.enabled = v != 0.0; }); },
            [band](const StageSet& s) { return flag(s.eq.band(band).enabled); });
        add({bandKey(band, "freq"), QStringLiteral("Frequency"), 20, 20000, double(def.freqHz), 0, QStringLiteral("Hz")},
            [band](StageSet& s, double v) { updateBand(s, band, [v](auto& p) { p.freqHz = float(v); }); },
            [band](const StageSet& s) { return double(s.eq.band(band).freqHz); });
        add({bandKey(band, "gain"), QStringLiteral("Gain"), -24, 24, 0, 1, QStringLiteral("dB")},
            [band](StageSet& s, double v) { updateBand(s, band, [v](auto& p) { p.gainDb = float(v); }); },
            [band](const StageSet& s) { return double(s.eq.band(band).gainDb); });
        add({bandKey(band, "q"), QStringLiteral("Q"), 0.1, 18, double(def.q), 2},
            [band](StageSet& s, double v) { updateBand(s, band, [v](auto& p) { p.q = float(v); }); },
            [band](const StageSet& s) { return double(s.eq.band(band).q); });
        // Defaults are the fixed layout every band had before these existed, so a
        // profile without them sounds as it always did.
        add(choiceParam(bandKey(band, "type"), QStringLiteral("Type"), bandTypeChoices(), bandTypeIndex(def.type)),
            [band](StageSet& s, double v) { updateBand(s, band, [v](auto& p) { p.type = bandTypeAt(v); }); },
            [band](const StageSet& s) { return double(bandTypeIndex(s.eq.band(band).type)); });
        add(choiceParam(bandKey(band, "slope"), QStringLiteral("Slope"),
                        {QStringLiteral("12 dB/oct"), QStringLiteral("24 dB/oct"), QStringLiteral("36 dB/oct"),
                         QStringLiteral("48 dB/oct")},
                        slopeIndex(def.slopeDbPerOct)),
            [band](StageSet& s, double v) {
                updateBand(s, band, [v](auto& p) { p.slopeDbPerOct = kSlopes[std::clamp(int(v), 0, kSlopeCount - 1)]; });
            },
            [band](const StageSet& s) { return double(slopeIndex(s.eq.band(band).slopeDbPerOct)); });
    }

    // ── Comp ─────────────────────────────────────────────────────────────────
    add(toggleParam(QStringLiteral("comp.enabled"), QStringLiteral("Enabled"), false),
        [](StageSet& s, double v) { s.comp.setEnabled(v != 0.0); },
        [](const StageSet& s) { return flag(s.comp.isEnabled()); });
    add({QStringLiteral("comp.threshold"), QStringLiteral("Threshold"), -60, 0, -18, 1, QStringLiteral("dB")},
        [](StageSet& s, double v) { s.comp.setThresholdDb(float(v)); },
        [](const StageSet& s) { return double(s.comp.thresholdDb()); });
    add({QStringLiteral("comp.ratio"), QStringLiteral("Ratio"), 1, 20, 3, 2, QStringLiteral(":1")},
        [](StageSet& s, double v) { s.comp.setRatio(float(v)); },
        [](const StageSet& s) { return double(s.comp.ratio()); });
    add({QStringLiteral("comp.attack"), QStringLiteral("Attack"), 0.1, 300, 20, 1, QStringLiteral("ms")},
        [](StageSet& s, double v) { s.comp.setAttackMs(float(v)); },
        [](const StageSet& s) { return double(s.comp.attackMs()); });
    add({QStringLiteral("comp.release"), QStringLiteral("Release"), 5, 2000, 200, 0, QStringLiteral("ms")},
        [](StageSet& s, double v) { s.comp.setReleaseMs(float(v)); },
        [](const StageSet& s) { return double(s.comp.releaseMs()); });
    add({QStringLiteral("comp.knee"), QStringLiteral("Knee"), 0, 24, 6, 1, QStringLiteral("dB")},
        [](StageSet& s, double v) { s.comp.setKneeDb(float(v)); },
        [](const StageSet& s) { return double(s.comp.kneeDb()); });
    add({QStringLiteral("comp.makeup"), QStringLiteral("Makeup"), -12, 24, 0, 1, QStringLiteral("dB")},
        [](StageSet& s, double v) { s.comp.setMakeupDb(float(v)); },
        [](const StageSet& s) { return double(s.comp.makeupDb()); });
    add({QStringLiteral("comp.drive"), QStringLiteral("Drive"), 0, 18, 0, 1, QStringLiteral("dB")},
        [](StageSet& s, double v) { s.comp.setDriveDb(float(v)); },
        [](const StageSet& s) { return double(s.comp.driveDb()); });
    add({QStringLiteral("comp.rotator"), QStringLiteral("Phase rotator"), 0, 6, 0, 0, QStringLiteral("stages")},
        [](StageSet& s, double v) { s.comp.setPhaseRotatorStages(int(v)); },
        [](const StageSet& s) { return double(s.comp.phaseRotatorStages()); });
    add(toggleParam(QStringLiteral("comp.limiter"), QStringLiteral("Limiter"), true),
        [](StageSet& s, double v) { s.comp.setLimiterEnabled(v != 0.0); },
        [](const StageSet& s) { return flag(s.comp.limiterEnabled()); });
    add({QStringLiteral("comp.ceiling"), QStringLiteral("Limiter ceiling"), -24, 0, -1, 1, QStringLiteral("dB")},
        [](StageSet& s, double v) { s.comp.setLimiterCeilingDb(float(v)); },
        [](const StageSet& s) { return double(s.comp.limiterCeilingDb()); });

    // ── Tube ─────────────────────────────────────────────────────────────────
    add(toggleParam(QStringLiteral("tube.enabled"), QStringLiteral("Enabled"), false),
        [](StageSet& s, double v) { s.tube.setEnabled(v != 0.0); },
        [](const StageSet& s) { return flag(s.tube.isEnabled()); });
    add(choiceParam(QStringLiteral("tube.model"), QStringLiteral("Model"),
                    {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}, 0),
        [](StageSet& s, double v) { s.tube.setModel(static_cast<ClientTube::Model>(int(v))); },
        [](const StageSet& s) { return double(int(s.tube.model())); });
    add({QStringLiteral("tube.drive"), QStringLiteral("Drive"), 0, 24, 0, 2, QStringLiteral("dB")},
        [](StageSet& s, double v) { s.tube.setDriveDb(float(v)); },
        [](const StageSet& s) { return double(s.tube.driveDb()); });
    add({QStringLiteral("tube.bias"), QStringLiteral("Bias"), 0, 1, 0, 2},
        [](StageSet& s, double v) { s.tube.setBiasAmount(float(v)); },
        [](const StageSet& s) { return double(s.tube.biasAmount()); });
    add({QStringLiteral("tube.tone"), QStringLiteral("Tone"), -1, 1, 0, 2},
        [](StageSet& s, double v) { s.tube.setTone(float(v)); },
        [](const StageSet& s) { return double(s.tube.tone()); });
    // +12 dB is as far as the DSP goes; it clamps anything above.
    add({QStringLiteral("tube.output"), QStringLiteral("Output"), -24, 12, 0, 2, QStringLiteral("dB")},
        [](StageSet& s, double v) { s.tube.setOutputGainDb(float(v)); },
        [](const StageSet& s) { return double(s.tube.outputGainDb()); });
    add({QStringLiteral("tube.mix"), QStringLiteral("Dry/wet"), 0, 1, 1, 2},
        [](StageSet& s, double v) { s.tube.setDryWet(float(v)); },
        [](const StageSet& s) { return double(s.tube.dryWet()); });
    add({QStringLiteral("tube.envelope"), QStringLiteral("Envelope"), -1, 1, 0, 2},
        [](StageSet& s, double v) { s.tube.setEnvelopeAmount(float(v)); },
        [](const StageSet& s) { return double(s.tube.envelopeAmount()); });
    add({QStringLiteral("tube.attack"), QStringLiteral("Attack"), 0.1, 30, ClientTube::kAttackMs, 1, QStringLiteral("ms")},
        [](StageSet& s, double v) { s.tube.setAttackMs(float(v)); },
        [](const StageSet& s) { return double(s.tube.attackMs()); });
    add({QStringLiteral("tube.release"), QStringLiteral("Release"), 10, 500, 35, 1, QStringLiteral("ms")},
        [](StageSet& s, double v) { s.tube.setReleaseMs(float(v)); },
        [](const StageSet& s) { return double(s.tube.releaseMs()); });

    // ── Pudu (AetherVoice exciter) ───────────────────────────────────────────
    add(toggleParam(QStringLiteral("pudu.enabled"), QStringLiteral("Enabled"), false),
        [](StageSet& s, double v) { s.pudu.setEnabled(v != 0.0); },
        [](const StageSet& s) { return flag(s.pudu.isEnabled()); });
    // Even is the aural exciter with its bottom end, Odd the sonic exciter.
    add(choiceParam(QStringLiteral("pudu.mode"), QStringLiteral("Mode"),
                    {QStringLiteral("Even"), QStringLiteral("Odd")}, 0),
        [](StageSet& s, double v) { s.pudu.setMode(static_cast<ClientPudu::Mode>(int(v))); },
        [](const StageSet& s) { return double(int(s.pudu.mode())); });
    add({QStringLiteral("pudu.pooDrive"), QStringLiteral("Low drive"), 0, 24, 0, 1, QStringLiteral("dB")},
        [](StageSet& s, double v) { s.pudu.setPooDriveDb(float(v)); },
        [](const StageSet& s) { return double(s.pudu.pooDriveDb()); });
    add({QStringLiteral("pudu.pooTune"), QStringLiteral("Low tune"), 50, 160, 100, 0, QStringLiteral("Hz")},
        [](StageSet& s, double v) { s.pudu.setPooTuneHz(float(v)); },
        [](const StageSet& s) { return double(s.pudu.pooTuneHz()); });
    add({QStringLiteral("pudu.pooMix"), QStringLiteral("Low mix"), 0, 1, 0.5, 2},
        [](StageSet& s, double v) { s.pudu.setPooMix(float(v)); },
        [](const StageSet& s) { return double(s.pudu.pooMix()); });
    add({QStringLiteral("pudu.dooTune"), QStringLiteral("High tune"), 1000, 10000, 5000, 0, QStringLiteral("Hz")},
        [](StageSet& s, double v) { s.pudu.setDooTuneHz(float(v)); },
        [](const StageSet& s) { return double(s.pudu.dooTuneHz()); });
    add({QStringLiteral("pudu.dooHarmonics"), QStringLiteral("Harmonics"), 0, 24, 6, 1, QStringLiteral("dB")},
        [](StageSet& s, double v) { s.pudu.setDooHarmonicsDb(float(v)); },
        [](const StageSet& s) { return double(s.pudu.dooHarmonicsDb()); });
    add({QStringLiteral("pudu.dooMix"), QStringLiteral("High mix"), 0, 1, 0.5, 2},
        [](StageSet& s, double v) { s.pudu.setDooMix(float(v)); },
        [](const StageSet& s) { return double(s.pudu.dooMix()); });

    return params;
}

const QList<StageParam>& stageParamTable()
{
    static const QList<StageParam> table = buildStageParams();
    return table;
}

// Built once, then only read, so any thread may look keys up.
const StageParam* findStageParam(const QString& key)
{
    static const QHash<QString, const StageParam*> byKey = [] {
        QHash<QString, const StageParam*> map;
        for (const StageParam& param : stageParamTable()) map.insert(param.spec.key, &param);
        return map;
    }();
    return byKey.value(key, nullptr);
}

} // namespace

RxChain::RxChain(int sampleRate)
    : m_sampleRate(sampleRate), m_order(stageIds())
{
    m_stages.prepare(sampleRate);
    m_stages.initialiseBands();
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
    if (id == QLatin1String("comp")) return QStringLiteral("Compressor");
    if (id == QLatin1String("tube")) return QStringLiteral("Tube");
    if (id == QLatin1String("pudu")) return QStringLiteral("Exciter");
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
    return applyStageParam(m_stages, key, value);
}

bool RxChain::applyStageParam(StageSet& stages, const QString& key, double value)
{
    const StageParam* param = findStageParam(key);
    if (!param) return false;
    param->apply(stages, value);
    return true;
}

bool RxChain::readStageParam(const StageSet& stages, const QString& key, double* value)
{
    const StageParam* param = findStageParam(key);
    if (!param) return false;
    if (value) *value = param->read(stages);
    return true;
}

QStringList RxChain::normalizedOrder(const QStringList& order)
{
    const QStringList known = stageIds();
    QStringList result;
    for (const QString& id : order) {
        if (known.contains(id) && !result.contains(id)) result.append(id);
    }
    for (const QString& id : known) {
        if (!result.contains(id)) result.append(id);
    }
    return result;
}

void RxChain::setStageOrder(const QStringList& order)
{
    m_order = normalizedOrder(order);
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

void RxChain::setOperatorBypass(bool bypassed)
{
    if (bypassed == m_operatorBypassed) return;
    m_operatorBypassed = bypassed;
    if (!bypassed && m_nr) m_nr->resetTransient();
}

void RxChain::setPostEqTap(SpscStereoRing* ring, const std::atomic<bool>* wanted)
{
    m_eqTap = ring;
    m_eqTapWanted = wanted;
}

void RxChain::reset()
{
    if (m_nr) m_nr->reset();
    m_stages.gate.reset();
    m_stages.eq.reset();
    m_stages.comp.reset();
    m_stages.tube.reset();
    m_stages.pudu.reset();
}

const QByteArray& RxChain::process(const QByteArray& stereoIn)
{
    m_nrActive = false;
    m_lastRun = 0;
    if (m_txBypassed || m_operatorBypassed) {
        m_nrGain = 1.0f;
        copyBlock(stereoIn, m_output);
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
        copyBlock(stereoIn, m_output);
        m_nrGain = 1.0f;
    }

    const int frames = int(m_output.size() / (2 * sizeof(float)));
    if (frames <= 0) return m_output;

    auto* samples = reinterpret_cast<float*>(m_output.data());
    unsigned ran = 0;
    for (const QString& stage : std::as_const(m_order)) {
        if (stage == QLatin1String("gate")) {
            if (m_stages.gate.isEnabled()) {
                m_stages.gate.process(samples, frames, 2);
                ran |= GateBit;
            }
        } else if (stage == QLatin1String("eq")) {
            if (m_stages.eq.isEnabled()) {
                m_stages.eq.process(samples, frames, 2);
                ran |= EqBit;
            }
            if (m_eqTap && m_eqTapWanted && m_eqTapWanted->load(std::memory_order_relaxed)) {
                m_eqTap->write(samples, size_t(frames));
            }
        } else if (stage == QLatin1String("comp")) {
            if (m_stages.comp.isEnabled()) {
                m_stages.comp.process(samples, frames, 2);
                ran |= CompBit;
            }
        } else if (stage == QLatin1String("tube")) {
            if (m_stages.tube.isEnabled()) {
                m_stages.tube.process(samples, frames, 2);
                ran |= TubeBit;
            }
        } else if (stage == QLatin1String("pudu")) {
            if (m_stages.pudu.isEnabled()) {
                m_stages.pudu.process(samples, frames, 2);
                ran |= PuduBit;
            }
        }
    }
    m_lastRun = ran;
    return m_output;
}

} // namespace esa
