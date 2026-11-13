#include "dsp/NrProcessor.h"

#include "RNNoiseFilter.h"
#include "SpectralNR.h"
#ifdef HAVE_SPECBLEACH
#include "SpecbleachFilter.h"
#endif
#ifdef __APPLE__
#include "MacNRFilter.h"
#endif
#ifdef HAVE_DFNR
#include "DeepFilterFilter.h"
#endif
#ifdef HAVE_NNR
#include "NnrControls.h"
#include "NnrFilter.h"
#endif

#include <algorithm>
#include <initializer_list>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace esa {

namespace {

#ifdef HAVE_DFNR
// DeepFilterNet's inference library always uses Apple's AMX coprocessor on macOS: in tract-linalg
// 0.21, has_amx() is simply `true`, with no detection and no override. A virtual machine does not
// expose AMX, so there DFNR would kill the whole app with an illegal instruction, leaving
// ExpertSDR3 muted. So it is not offered at all. GitHub's Mac runners are such VMs.
bool dfnrCanRunHere()
{
#ifdef __APPLE__
    int virtualized = 0;
    size_t size = sizeof(virtualized);
    if (sysctlbyname("kern.hv_vmm_present", &virtualized, &size, nullptr, 0) == 0 && virtualized != 0) {
        return false;
    }
#endif
    return true;
}
#endif

// ── RN2: RNNoise. Native 48 kHz, one state per channel ───────────────────────
class Rn2Processor final : public NrProcessor {
public:
    explicit Rn2Processor(int rate)
        : m_rate(rate),
          m_filter(AetherSDR::RNNoiseFilter::OutputMode::PreserveRxStereo,
                   rate == 48000 ? AetherSDR::RNNoiseFilter::RateDomain::Native48k
                                 : AetherSDR::RNNoiseFilter::RateDomain::Legacy24k) {}
    bool valid() const { return m_filter.isValid(); }
    void process(const QByteArray& in, QByteArray& out) override
    {
        if (m_rate == 48000) {
            m_filter.process48kStereo(in, out);
        } else {
            out = m_filter.process(in);
        }
    }
    void reset() override { m_filter.reset(); }
    void setParam(const QString& key, double value) override
    {
        if (key == QLatin1String("rn2.dry")) m_filter.setDryMix(float(value));
    }

private:
    int m_rate;
    AetherSDR::RNNoiseFilter m_filter;
};

// ── NR2: WDSP-derived spectral NR ────────────────────────────────────────────
class Nr2Processor final : public NrProcessor {
public:
    // Upstream's real-radio geometry: 1024-point FFT at 24 kHz scaled with the
    // rate, 75% overlap (AudioEngine::createNr2Filter).
    explicit Nr2Processor(int rate) : m_filter(1024 * rate / 24000, rate, 4) {}
    bool valid() const { return !m_filter.hasPlanFailed(); }
    void process(const QByteArray& in, QByteArray& out) override
    {
        const int frames = int(in.size() / (2 * sizeof(float)));
        out.resize(in.size());
        auto* dst = reinterpret_cast<float*>(out.data());
        m_filter.processStereo(reinterpret_cast<const float*>(in.constData()), dst, frames);
        // A gain above 1 could otherwise exceed full scale (upstream #1507).
        for (int i = 0; i < frames * 2; ++i) dst[i] = std::clamp(dst[i], -1.0f, 1.0f);
    }
    void reset() override { m_filter.reset(); }
    void resetTransient() override { m_filter.resetTransient(); }
    void setParam(const QString& key, double value) override
    {
        const QString name = key.mid(4);
        if (name == QLatin1String("gainMax")) m_filter.setGainMax(float(value));
        else if (name == QLatin1String("gainFloor")) m_filter.setGainFloor(float(value));
        else if (name == QLatin1String("qspp")) m_filter.setQspp(float(value));
        else if (name == QLatin1String("smooth")) m_filter.setGainSmooth(float(value));
        else if (name == QLatin1String("gainMethod")) m_filter.setGainMethod(int(value));
        else if (name == QLatin1String("npe")) m_filter.setNpeMethod(int(value));
        else if (name == QLatin1String("ae")) m_filter.setAeFilter(value != 0.0);
        else if (name == QLatin1String("post2")) m_filter.setPost2Run(value != 0.0);
        else if (name == QLatin1String("post2Level")) m_filter.setPost2Nlevel(float(value));
        else if (name == QLatin1String("post2Factor")) m_filter.setPost2Factor(float(value));
        else if (name == QLatin1String("post2Taper")) m_filter.setPost2TaperHz(float(value));
    }

private:
    AetherSDR::SpectralNR m_filter;
};

#ifdef HAVE_SPECBLEACH
// ── NR4: libspecbleach ───────────────────────────────────────────────────────
class Nr4Processor final : public NrProcessor {
public:
    explicit Nr4Processor(int rate) : m_filter(rate) {}
    bool valid() const { return m_filter.isValid(); }
    void process(const QByteArray& in, QByteArray& out) override { out = m_filter.process(in); }
    void reset() override { m_filter.reset(); }
    void setParam(const QString& key, double value) override
    {
        const QString name = key.mid(4);
        if (name == QLatin1String("reduction")) m_filter.setReductionAmount(float(value));
        else if (name == QLatin1String("smoothing")) m_filter.setSmoothingFactor(float(value));
        else if (name == QLatin1String("whitening")) m_filter.setWhiteningFactor(float(value));
        else if (name == QLatin1String("adaptive")) m_filter.setAdaptiveNoise(value != 0.0);
        else if (name == QLatin1String("method")) m_filter.setNoiseEstimationMethod(int(value));
        else if (name == QLatin1String("masking")) m_filter.setMaskingDepth(float(value));
        else if (name == QLatin1String("suppression")) m_filter.setSuppressionStrength(float(value));
    }

private:
    AetherSDR::SpecbleachFilter m_filter;
};
#endif

#ifdef __APPLE__
// ── MNR: Accelerate/vDSP spectral NR (macOS only) ────────────────────────────
class MnrProcessor final : public NrProcessor {
public:
    explicit MnrProcessor(int rate) : m_filter(rate) {}
    bool valid() const { return m_filter.isValid(); }
    void process(const QByteArray& in, QByteArray& out) override { out = m_filter.process(in); }
    void reset() override { m_filter.reset(); }
    void setParam(const QString& key, double value) override
    {
        if (key == QLatin1String("mnr.strength")) m_filter.setStrength(float(value));
    }

private:
    AetherSDR::MacNRFilter m_filter;
};
#endif

#ifdef HAVE_DFNR
// ── DFNR: DeepFilterNet3 ─────────────────────────────────────────────────────
class DfnrProcessor final : public NrProcessor {
public:
    explicit DfnrProcessor(int rate) : m_filter(rate) {}
    bool valid() const { return m_filter.isValid(); }
    void process(const QByteArray& in, QByteArray& out) override { out = m_filter.process(in); }
    void reset() override { m_filter.reset(); }
    void setParam(const QString& key, double value) override
    {
        if (key == QLatin1String("dfnr.atten")) m_filter.setAttenLimit(float(value));
        else if (key == QLatin1String("dfnr.beta")) m_filter.setPostFilterBeta(float(value));
    }

private:
    AetherSDR::DeepFilterFilter m_filter;
};
#endif

#ifdef HAVE_NNR
// ── NNR: WDSP 2.11 neural NR ─────────────────────────────────────────────────
class NnrProcessor final : public NrProcessor {
public:
    explicit NnrProcessor(int rate) : m_filter(rate) {}
    bool valid() const { return m_filter.isValid(); }
    void process(const QByteArray& in, QByteArray& out) override { out = m_filter.process(in); }
    void reset() override { m_filter.reset(); }
    void setParam(const QString& key, double value) override
    {
        const QString name = key.mid(4);
        if (name == QLatin1String("strength")) m_filter.setStrength(int(value));
        else if (name == QLatin1String("model")) m_filter.setModel(int(value));
        else if (name == QLatin1String("alpha")) m_filter.setAlpha(value);
        else if (name == QLatin1String("knee")) m_filter.setAlphaKnee(value);
        else if (name == QLatin1String("tau")) m_filter.setTau(value);
        else if (name == QLatin1String("maxGain")) m_filter.setMaxGain(value);
        else if (name == QLatin1String("attack")) {
            m_attack = value;
            m_filter.setSmoothing(m_attack, m_release);
        } else if (name == QLatin1String("release")) {
            m_release = value;
            m_filter.setSmoothing(m_attack, m_release);
        }
    }

private:
    AetherSDR::NnrFilter m_filter;
    double m_attack{AetherSDR::Nnr::kSmoothAttack.defaultValue};
    double m_release{AetherSDR::Nnr::kSmoothRelease.defaultValue};
};
#endif

template <class Processor>
std::unique_ptr<NrProcessor> makeIfValid(int rate)
{
    auto processor = std::make_unique<Processor>(rate);
    if (!processor->valid()) return nullptr;
    return processor;
}

// The NR page frames its controls under upstream's own headings (AetherDspWidget.cpp),
// named here by the part of each key after the dot.
void group(QList<ParamSpec>& params, const QString& section, std::initializer_list<const char*> names)
{
    for (ParamSpec& spec : params) {
        for (const char* name : names) {
            if (spec.key.section(QLatin1Char('.'), 1) == QLatin1String(name)) spec.section = section;
        }
    }
}

// Controls that do nothing unless `key` is on, as the noise fill's are without post2.
void enabledBy(QList<ParamSpec>& params, const QString& key, std::initializer_list<const char*> names)
{
    for (ParamSpec& spec : params) {
        for (const char* name : names) {
            if (spec.key.section(QLatin1Char('.'), 1) == QLatin1String(name)) {
                spec.enabledByKey = key;
                spec.enabledByValue = 1.0;
            }
        }
    }
}

QList<NrMethod> buildMethods()
{
    QList<NrMethod> methods;

    {
        NrMethod nr2{QStringLiteral("nr2"), QStringLiteral("NR2 (Spectral)"),
            QStringLiteral("WDSP-derived spectral noise reduction: light on CPU, tunable."),
            {
                {QStringLiteral("nr2.gainMax"), QStringLiteral("Gain ceiling"), 0.0, 2.0, 1.0, 2},
                {QStringLiteral("nr2.gainFloor"), QStringLiteral("Naturalness (floor)"), 0.0, 1.0, 0.0, 2},
                {QStringLiteral("nr2.qspp"), QStringLiteral("Speech prior"), 0.01, 0.99, 0.2, 2},
                {QStringLiteral("nr2.smooth"), QStringLiteral("Gain smoothing"), 0.0, 0.99, 0.85, 2},
                choiceParam(QStringLiteral("nr2.gainMethod"), QStringLiteral("Gain method"),
                            {QStringLiteral("Linear"), QStringLiteral("Log"), QStringLiteral("Gamma"),
                             QStringLiteral("Trained")}, 2),
                choiceParam(QStringLiteral("nr2.npe"), QStringLiteral("Noise estimator"),
                            {QStringLiteral("OSMS"), QStringLiteral("MMSE"), QStringLiteral("NSTAT")}, 0),
                toggleParam(QStringLiteral("nr2.ae"), QStringLiteral("Artifact elimination"), true),
                toggleParam(QStringLiteral("nr2.post2"), QStringLiteral("Comfort noise"), false),
                {QStringLiteral("nr2.post2Level"), QStringLiteral("Comfort noise level"), 0.0, 1.0, 0.15, 2},
                // 0 mixes back the noise actually removed, 1 synthetic white. The default is
                // the vendored filter's own, so adding the key changes no one's sound.
                {QStringLiteral("nr2.post2Factor"), QStringLiteral("Fill character"), 0.0, 1.0, 0.15, 2},
                // The vendored setter clamps to 300..6000 Hz, so that is the whole range.
                {QStringLiteral("nr2.post2Taper"), QStringLiteral("Comfort noise band"), 300.0, 6000.0, 2871.0, 0,
                 QStringLiteral("Hz")},
            },
            [](int rate) { return makeIfValid<Nr2Processor>(rate); }};
        group(nr2.params, QStringLiteral("GAIN METHOD"), {"gainMethod"});
        group(nr2.params, QStringLiteral("NOISE ESTIMATION"), {"npe"});
        group(nr2.params, QStringLiteral("POST PROCESSING"), {"ae", "post2"});
        group(nr2.params, QStringLiteral("MASK"),
              {"gainMax", "gainFloor", "qspp", "smooth", "post2Level", "post2Factor", "post2Taper"});
        enabledBy(nr2.params, QStringLiteral("nr2.post2"), {"post2Level", "post2Factor", "post2Taper"});
        methods.append(nr2);
    }

#ifdef HAVE_SPECBLEACH
    {
        NrMethod nr4{QStringLiteral("nr4"), QStringLiteral("NR4 (Specbleach)"),
            QStringLiteral("libspecbleach adaptive spectral subtraction."),
            {
                {QStringLiteral("nr4.reduction"), QStringLiteral("Reduction"), 0.0, 40.0, 10.0, 1, QStringLiteral("dB")},
                {QStringLiteral("nr4.smoothing"), QStringLiteral("Smoothing"), 0.0, 100.0, 0.0, 0, QStringLiteral("%")},
                {QStringLiteral("nr4.whitening"), QStringLiteral("Whitening"), 0.0, 100.0, 0.0, 0, QStringLiteral("%")},
                toggleParam(QStringLiteral("nr4.adaptive"), QStringLiteral("Adaptive noise"), true),
                choiceParam(QStringLiteral("nr4.method"), QStringLiteral("Noise estimator"),
                            {QStringLiteral("SPP-MMSE"), QStringLiteral("Brandt"), QStringLiteral("Martin")}, 0),
                {QStringLiteral("nr4.masking"), QStringLiteral("Masking depth"), 0.0, 1.0, 0.5, 2},
                {QStringLiteral("nr4.suppression"), QStringLiteral("Suppression"), 0.0, 1.0, 0.5, 2},
            },
            [](int rate) { return makeIfValid<Nr4Processor>(rate); }};
        group(nr4.params, QStringLiteral("NOISE ESTIMATION"), {"method"});
        group(nr4.params, QStringLiteral("SPECTRAL"),
              {"reduction", "smoothing", "whitening", "adaptive", "masking", "suppression"});
        methods.append(nr4);
    }
#endif

#ifdef __APPLE__
    {
        NrMethod mnr{QStringLiteral("mnr"), QStringLiteral("MNR (Apple vDSP)"),
            QStringLiteral("MMSE-Wiener spectral NR on Apple Accelerate. macOS only."),
            {
                {QStringLiteral("mnr.strength"), QStringLiteral("Strength"), 0.0, 1.0, 1.0, 2},
            },
            [](int rate) { return makeIfValid<MnrProcessor>(rate); }};
        group(mnr.params, QStringLiteral("MMSE-WIENER"), {"strength"});
        mnr.params[0].percent = true;  // upstream shows it as 0..100 %
        methods.append(mnr);
    }
#endif

#ifdef HAVE_DFNR
    if (dfnrCanRunHere()) {
        NrMethod dfnr{QStringLiteral("dfnr"), QStringLiteral("DFNR (DeepFilterNet3)"),
            QStringLiteral("Neural speech enhancement. Strongest on voice, heaviest on CPU."),
            {
                {QStringLiteral("dfnr.atten"), QStringLiteral("Attenuation limit"), 0.0, 100.0, 100.0, 0, QStringLiteral("dB")},
                {QStringLiteral("dfnr.beta"), QStringLiteral("Post-filter beta"), 0.0, 0.3, 0.0, 2},
            },
            [](int rate) { return makeIfValid<DfnrProcessor>(rate); }};
        group(dfnr.params, QStringLiteral("DEEPFILTERNET"), {"atten", "beta"});
        methods.append(dfnr);
    }
#endif

    {
        NrMethod rn2{QStringLiteral("rn2"), QStringLiteral("RN2 (RNNoise)"),
            QStringLiteral("Neural speech denoiser. Good on voice at low CPU."),
            {
                {QStringLiteral("rn2.dry"), QStringLiteral("Dry mix"), 0.0, 1.0, 0.0, 2},
            },
            [](int rate) { return makeIfValid<Rn2Processor>(rate); }};
        group(rn2.params, QStringLiteral("RNNOISE"), {"dry"});
        methods.append(rn2);
    }

#ifdef HAVE_NNR
    {
        namespace N = AetherSDR::Nnr;
        NrMethod nnr{QStringLiteral("nnr"), QStringLiteral("NNR (WDSP Neural)"),
            QStringLiteral("WDSP 2.11 neural NR. A speech model: not for CW or data."),
            {
                {QStringLiteral("nnr.strength"), QStringLiteral("Strength"), 0.0, 100.0,
                 double(N::kMaskFloorDefaultStrength), 0},
                choiceParam(QStringLiteral("nnr.model"), QStringLiteral("Model"),
                            {QStringLiteral("Standard"), QStringLiteral("Premium")}, 0),
                {QStringLiteral("nnr.alpha"), QStringLiteral("Expansion"), N::kAlpha.minimum, N::kAlpha.maximum,
                 N::kAlpha.defaultValue, 2},
                {QStringLiteral("nnr.knee"), QStringLiteral("Expansion knee"), N::kAlphaKnee.minimum,
                 N::kAlphaKnee.maximum, N::kAlphaKnee.defaultValue, 1, QStringLiteral("dB")},
                {QStringLiteral("nnr.tau"), QStringLiteral("Conditioner tau"), N::kTau.minimum, N::kTau.maximum,
                 N::kTau.defaultValue, 2, QStringLiteral("s")},
                {QStringLiteral("nnr.maxGain"), QStringLiteral("Max gain"), N::kMaxGain.minimum,
                 N::kMaxGain.maximum, N::kMaxGain.defaultValue, 1, QStringLiteral("dB")},
                {QStringLiteral("nnr.attack"), QStringLiteral("Smooth attack"), N::kSmoothAttack.minimum,
                 N::kSmoothAttack.maximum, N::kSmoothAttack.defaultValue, 0, QStringLiteral("ms")},
                {QStringLiteral("nnr.release"), QStringLiteral("Smooth release"), N::kSmoothRelease.minimum,
                 N::kSmoothRelease.maximum, N::kSmoothRelease.defaultValue, 0, QStringLiteral("ms")},
            },
            [](int rate) { return makeIfValid<NnrProcessor>(rate); }};
        group(nnr.params, QStringLiteral("NEURAL NR"), {"strength", "model"});
        group(nnr.params, QStringLiteral("ADVANCED \u2014 WDSP LEAVES THESE UNDOCUMENTED"),
              {"alpha", "knee", "tau", "maxGain", "attack", "release"});
        methods.append(nnr);
    }
#endif

    return methods;
}

} // namespace

const QList<NrMethod>& nrMethods()
{
    static const QList<NrMethod> methods = buildMethods();
    return methods;
}

const NrMethod* findNrMethod(const QString& id)
{
    for (const NrMethod& method : nrMethods()) {
        if (method.id == id) return &method;
    }
    return nullptr;
}

QString nrMethodUnavailableReason(const QString& id)
{
    if (findNrMethod(id)) return QString();
    if (id == QLatin1String("mnr")) return QStringLiteral("Needs macOS");
#ifdef HAVE_DFNR
    // Compiled in, so dfnrCanRunHere() is what withheld it.
    if (id == QLatin1String("dfnr")) return QStringLiteral("Unsafe on a virtual Mac");
#endif
    if (id == QLatin1String("nr4") || id == QLatin1String("dfnr") || id == QLatin1String("nnr")) {
        return QStringLiteral("Not built into this version");
    }
    return QStringLiteral("Not available in this build");  // BNR (NVIDIA), and anything unknown
}

bool modeBypassesNr(const QString& modulation)
{
    static const QStringList kModes = {
        QStringLiteral("CW"), QStringLiteral("CWU"), QStringLiteral("CWL"), QStringLiteral("CWR"),
        QStringLiteral("DIGU"), QStringLiteral("DIGL"), QStringLiteral("RTTY"), QStringLiteral("BPSK"),
        QStringLiteral("WSPR"), QStringLiteral("FT8"), QStringLiteral("FT4"), QStringLiteral("JT65"),
        QStringLiteral("JT9"),
    };
    return kModes.contains(modulation, Qt::CaseInsensitive);
}

bool modeIsComplexStereo(const QString& modulation)
{
    return modulation.compare(QLatin1String("DIGL"), Qt::CaseInsensitive) == 0
        || modulation.compare(QLatin1String("DIGU"), Qt::CaseInsensitive) == 0;
}

} // namespace esa
