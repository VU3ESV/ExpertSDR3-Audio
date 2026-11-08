#include "ReferenceChain.h"

#include "audio/SpscRing.h"
#include "dsp/NrProcessor.h"
#include "dsp/RxChain.h"

#include <QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

using namespace esa;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Source-filter speech synthesis: a glottal pulse train with drifting pitch through
// formant resonators, in irregular syllables with pauses. Steady tones will not do
// here. Statistical estimators and neural speech models both, correctly, treat a
// stationary tone as noise, which tests nothing except the fixture. Deterministic,
// so a failure reproduces.
std::vector<float> synthSpeech(int rate, double seconds)
{
    const int frames = int(rate * seconds);
    std::vector<float> out(size_t(frames), 0.0f);
    std::mt19937 rng(77);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);

    struct Vowel { double f1, f2, f3; };
    static const Vowel kVowels[] = {
        {730, 1090, 2440}, {270, 2290, 3010}, {300, 870, 2240}, {530, 1840, 2480}, {570, 840, 2410},
    };

    int i = 0;
    while (i < frames) {
        const int syllable = int((0.16 + 0.22 * uniform(rng)) * rate);
        const int pause = int((0.05 + 0.25 * uniform(rng)) * rate);
        const Vowel a = kVowels[int(uniform(rng) * 5) % 5];
        const Vowel b = kVowels[int(uniform(rng) * 5) % 5];
        const double f0Start = 95.0 + 80.0 * uniform(rng);
        const double f0End = f0Start * (0.8 + 0.4 * uniform(rng));

        double z[3][2] = {{0, 0}, {0, 0}, {0, 0}};
        double phase = 0.0;
        for (int n = 0; n < syllable && i + n < frames; ++n) {
            const double t = double(n) / syllable;
            const double f0 = f0Start + (f0End - f0Start) * t;
            phase += f0 / rate;
            double source = 0.0;
            if (phase >= 1.0) {
                phase -= 1.0;
                source = 1.0;
            }
            const double envelope = std::sin(kPi * t);
            double sample = 0.0;
            const double formants[3] = {a.f1 + (b.f1 - a.f1) * t, a.f2 + (b.f2 - a.f2) * t, a.f3 + (b.f3 - a.f3) * t};
            const double bandwidths[3] = {90.0, 130.0, 170.0};
            for (int k = 0; k < 3; ++k) {
                const double r = std::exp(-kPi * bandwidths[k] / rate);
                const double w = 2.0 * kPi * formants[k] / rate;
                const double y = source + 2.0 * r * std::cos(w) * z[k][0] - r * r * z[k][1];
                z[k][1] = z[k][0];
                z[k][0] = y;
                sample += y * (k == 0 ? 1.0 : 0.6);
            }
            out[size_t(i + n)] = float(sample * envelope * 0.04);
        }
        i += syllable + pause;
    }
    return out;
}

struct TestSignal {
    QByteArray noisy;            // stereo: the same speech with independent noise per ear
    std::vector<float> clean;    // left channel's clean speech
    std::vector<float> noise;    // left channel's noise
};

// Speech at a realistic weak-signal SNR. The speech is scaled to a fixed level, so the
// score means the same at either rate, and it must not sit far above the noise: at a
// high SNR the residual is mostly a mask's harmless modulation of the speech, not noise.
TestSignal makeNoisySpeech(int rate, double seconds, double snrDb = 6.0)
{
    constexpr double kSpeechRms = 0.05;
    TestSignal signal;
    signal.clean = synthSpeech(rate, seconds);
    const int frames = int(signal.clean.size());
    double energy = 0.0;
    for (float v : signal.clean) energy += double(v) * v;
    const double scale = kSpeechRms / std::sqrt(energy / std::max(frames, 1));
    for (float& v : signal.clean) v = float(v * scale);
    const double noiseRms = kSpeechRms * std::pow(10.0, -snrDb / 20.0);
    signal.noise.resize(size_t(frames));
    signal.noisy.resize(frames * 2 * int(sizeof(float)));
    auto* samples = reinterpret_cast<float*>(signal.noisy.data());
    std::mt19937 rng(1234);
    std::normal_distribution<double> gaussian(0.0, noiseRms);
    for (int i = 0; i < frames; ++i) {
        signal.noise[size_t(i)] = float(gaussian(rng));
        samples[i * 2] = signal.clean[size_t(i)] + signal.noise[size_t(i)];
        samples[i * 2 + 1] = signal.clean[size_t(i)] + float(gaussian(rng));
    }
    return signal;
}

QByteArray makeNoisyBursts(int rate, double seconds)
{
    return makeNoisySpeech(rate, seconds).noisy;
}

// Feed blocks through as the live path does: 512 frames at a time.
QByteArray runThrough(RxChain& chain, const QByteArray& pcm, int blockFrames = 512)
{
    QByteArray output;
    const int blockBytes = blockFrames * 2 * int(sizeof(float));
    for (qsizetype offset = 0; offset + blockBytes <= pcm.size(); offset += blockBytes) {
        output += chain.process(pcm.mid(offset, blockBytes));
    }
    return output;
}

struct NrScore {
    double delayMs{0.0};
    double signalGainDb{0.0};    // how much of the clean speech survived
    double snrImprovementDb{0.0};
    double pauseAttenuationDb{0.0};  // noise removed where nobody is speaking
};

// Knowing the clean signal, split the output into the part that is still that speech
// and everything else, the standard way to score a denoiser. Unlike comparing levels
// in gated windows, this cannot be fooled by a method that simply turns everything
// down, and it needs no assumption about the method's latency, which it measures.
NrScore scoreDenoiser(const TestSignal& signal, const QByteArray& output, int rate, double skipSeconds)
{
    const auto* out = reinterpret_cast<const float*>(output.constData());
    const qsizetype outFrames = output.size() / (2 * qsizetype(sizeof(float)));
    const qsizetype start = qsizetype(skipSeconds * rate);
    const qsizetype maxDelay = qsizetype(0.5 * rate);

    // Latency: the lag with the strongest correlation against the clean speech. The
    // sum runs over a decimated grid; the speech is low-passed by nature, so this is
    // accurate to well under the resolution the result is used for.
    qsizetype bestLag = 0;
    double bestCorrelation = -1.0;
    for (qsizetype lag = 0; lag < maxDelay; lag += 4) {
        double sum = 0.0;
        for (qsizetype i = start; i + lag < outFrames && i < qsizetype(signal.clean.size()); i += 3) {
            sum += double(out[(i + lag) * 2]) * signal.clean[size_t(i)];
        }
        if (std::fabs(sum) > bestCorrelation) {
            bestCorrelation = std::fabs(sum);
            bestLag = lag;
        }
    }
    // Refine to the exact sample around the coarse peak.
    for (qsizetype lag = std::max<qsizetype>(0, bestLag - 4); lag <= bestLag + 4; ++lag) {
        double sum = 0.0;
        for (qsizetype i = start; i + lag < outFrames && i < qsizetype(signal.clean.size()); ++i) {
            sum += double(out[(i + lag) * 2]) * signal.clean[size_t(i)];
        }
        if (std::fabs(sum) > bestCorrelation) {
            bestCorrelation = std::fabs(sum);
            bestLag = lag;
        }
    }

    double cleanCross = 0.0;
    double cleanEnergy = 0.0;
    double noiseEnergy = 0.0;
    for (qsizetype i = start; i + bestLag < outFrames && i < qsizetype(signal.clean.size()); ++i) {
        cleanCross += double(out[(i + bestLag) * 2]) * signal.clean[size_t(i)];
        cleanEnergy += double(signal.clean[size_t(i)]) * signal.clean[size_t(i)];
        noiseEnergy += double(signal.noise[size_t(i)]) * signal.noise[size_t(i)];
    }
    const double gain = cleanCross / std::max(cleanEnergy, 1e-12);

    double residualEnergy = 0.0;
    for (qsizetype i = start; i + bestLag < outFrames && i < qsizetype(signal.clean.size()); ++i) {
        const double residual = double(out[(i + bestLag) * 2]) - gain * signal.clean[size_t(i)];
        residualEnergy += residual * residual;
    }

    NrScore score;
    score.delayMs = double(bestLag) * 1000.0 / rate;
    score.signalGainDb = 20.0 * std::log10(std::max(std::fabs(gain), 1e-9));
    const double snrIn = cleanEnergy / std::max(noiseEnergy, 1e-12);
    const double snrOut = gain * gain * cleanEnergy / std::max(residualEnergy, 1e-12);
    score.snrImprovementDb = 10.0 * std::log10(snrOut / snrIn);

    // In the pauses, all of the output is residual noise. A sample counts as "in a
    // pause" only well away from speech, so a method's smearing cannot leak in.
    const qsizetype guard = qsizetype(0.03 * rate);
    qsizetype sinceSpeech = 0;
    double pauseOut = 0.0;
    double pauseIn = 0.0;
    for (qsizetype i = start; i + bestLag + guard < outFrames && i + guard < qsizetype(signal.clean.size()); ++i) {
        sinceSpeech = signal.clean[size_t(i)] != 0.0f ? 0 : sinceSpeech + 1;
        if (sinceSpeech > guard && signal.clean[size_t(i + guard)] == 0.0f) {
            pauseOut += double(out[(i + bestLag) * 2]) * out[(i + bestLag) * 2];
            pauseIn += double(signal.noise[size_t(i)]) * signal.noise[size_t(i)];
        }
    }
    score.pauseAttenuationDb = 10.0 * std::log10(pauseIn / std::max(pauseOut, 1e-15));
    return score;
}

bool allFinite(const QByteArray& pcm)
{
    const auto* samples = reinterpret_cast<const float*>(pcm.constData());
    const qsizetype count = pcm.size() / qsizetype(sizeof(float));
    for (qsizetype i = 0; i < count; ++i) {
        if (!std::isfinite(samples[i])) return false;
    }
    return true;
}

std::shared_ptr<NrProcessor> makeMethod(const NrMethod& method, int rate)
{
    std::shared_ptr<NrProcessor> processor(method.create(rate).release());
    if (processor) {
        for (const ParamSpec& spec : method.params) processor->setParam(spec.key, spec.def);
    }
    return processor;
}

// Counts what the chain asks of its method. Halves the level, so whether it ran shows.
class CountingNr final : public NrProcessor {
public:
    void process(const QByteArray& in, QByteArray& out) override
    {
        ++processed;
        if (swallow) {  // a method still filling its first block
            out.clear();
            return;
        }
        out = in;
        auto* samples = reinterpret_cast<float*>(out.data());
        for (qsizetype i = 0; i < out.size() / qsizetype(sizeof(float)); ++i) samples[i] *= 0.5f;
    }
    void reset() override { ++resets; }
    void resetTransient() override { ++transientResets; }
    void setParam(const QString&, double) override {}

    bool swallow{false};
    int processed{0};
    int resets{0};
    int transientResets{0};
};

double rounded(double value, int decimals)
{
    const double scale = std::pow(10.0, decimals);
    return std::round(value * scale) / scale;
}

double rmsOf(const QByteArray& pcm)
{
    const auto* samples = reinterpret_cast<const float*>(pcm.constData());
    const qsizetype count = pcm.size() / qsizetype(sizeof(float));
    double sum = 0.0;
    for (qsizetype i = 0; i < count; ++i) sum += double(samples[i]) * samples[i];
    return count ? std::sqrt(sum / double(count)) : 0.0;
}

QByteArray drain(SpscStereoRing& ring)
{
    QByteArray bytes(qsizetype(ring.size()) * 2 * qsizetype(sizeof(float)), Qt::Uninitialized);
    const size_t read = ring.read(reinterpret_cast<float*>(bytes.data()), ring.size());
    bytes.truncate(qsizetype(read) * 2 * qsizetype(sizeof(float)));
    return bytes;
}

// v0.1.1's chain, through RxChain, the way the engine builds it from a configuration:
// every key in this table's order, with the default for any the settings lack.
void applyLikeTheEngine(RxChain& chain, const QHash<QString, double>& settings)
{
    for (const ParamSpec& spec : RxChain::stageParams()) chain.setStageParam(spec.key, settings.value(spec.key, spec.def));
}

const ParamSpec* stageSpec(const QString& key)
{
    for (const ParamSpec& spec : RxChain::stageParams()) {
        if (spec.key == key) return &spec;
    }
    return nullptr;
}

} // namespace

class TestRxChain : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qInfo() << "NR methods compiled in:";
        for (const NrMethod& m : nrMethods()) qInfo() << " " << m.id;
    }

    void passesThroughUntouchedWithNothingEnabled()
    {
        RxChain chain(48000);
        const QByteArray in = makeNoisyBursts(48000, 0.2);
        const QByteArray out = runThrough(chain, in);
        QCOMPARE(out.left(1024), in.left(1024));
    }

    // The real vendored code on real samples, for every method this build has.
    void everyMethodReducesNoiseAndKeepsSignal_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<int>("rate");
        for (const NrMethod& method : nrMethods()) {
            for (int rate : {48000, 24000}) {
                QTest::addRow("%s@%d", qPrintable(method.id), rate) << method.id << rate;
            }
        }
    }

    void everyMethodReducesNoiseAndKeepsSignal()
    {
        QFETCH(QString, id);
        QFETCH(int, rate);
        const NrMethod* method = findNrMethod(id);
        QVERIFY(method);

        RxChain chain(rate);
        auto processor = makeMethod(*method, rate);
        QVERIFY2(processor, "failed to construct");
        chain.setNrProcessor(processor);

        constexpr double kSeconds = 14.0;
        constexpr double kSkip = 6.0;  // estimators and models need time to settle
        const TestSignal signal = makeNoisySpeech(rate, kSeconds);
        const QByteArray out = runThrough(chain, signal.noisy);

        QVERIFY(allFinite(out));

        // Conservation: a method may buffer, but must not lose or invent audio.
        const qsizetype inFrames = signal.noisy.size() / (2 * qsizetype(sizeof(float)));
        const qsizetype outFrames = out.size() / (2 * qsizetype(sizeof(float)));
        QVERIFY2(outFrames > inFrames - rate / 2 && outFrames <= inFrames,
                 qPrintable(QStringLiteral("in %1 frames, out %2").arg(inFrames).arg(outFrames)));

        const NrScore score = scoreDenoiser(signal, out, rate, kSkip);
        qInfo().nospace() << qPrintable(id) << "@" << rate << ": SNR +" << score.snrImprovementDb
                          << " dB, pause noise -" << score.pauseAttenuationDb
                          << " dB, speech " << score.signalGainDb
                          << " dB, delay " << score.delayMs << " ms";

        // These separate "working" from "passthrough or broken", and are not a quality
        // ranking. Passthrough scores 0 and 0; a method silencing everything fails on
        // speech. Methods differ a lot: at this SNR, NR2 at upstream's own defaults
        // measures about +1.9 / -19 / -3.5 dB, and DFNR about +10 / -32 / -0.7 dB.
        QVERIFY2(score.snrImprovementDb > 1.0,
                 qPrintable(QStringLiteral("SNR improved only %1 dB").arg(score.snrImprovementDb)));
        QVERIFY2(score.pauseAttenuationDb > 4.0,
                 qPrintable(QStringLiteral("pause noise fell only %1 dB").arg(score.pauseAttenuationDb)));
        QVERIFY2(score.signalGainDb > -6.0,
                 qPrintable(QStringLiteral("speech dropped by %1 dB").arg(score.signalGainDb)));
        QVERIFY2(score.delayMs < 400.0, qPrintable(QStringLiteral("delay %1 ms").arg(score.delayMs)));
    }

#ifdef HAVE_DFNR
    // On a Mac VM DFNR would crash the app, because its library needs AMX unconditionally.
    void dfnrIsOfferedExceptInAMacVirtualMachine()
    {
        bool expected = true;
#ifdef __APPLE__
        int virtualized = 0;
        size_t size = sizeof(virtualized);
        if (sysctlbyname("kern.hv_vmm_present", &virtualized, &size, nullptr, 0) == 0) {
            expected = virtualized == 0;
        }
#endif
        QCOMPARE(findNrMethod(QStringLiteral("dfnr")) != nullptr, expected);
    }
#endif

    void everyMethodSurvivesResetAndSilence()
    {
        for (const NrMethod& method : nrMethods()) {
            RxChain chain(48000);
            auto processor = makeMethod(method, 48000);
            QVERIFY(processor);
            chain.setNrProcessor(processor);
            runThrough(chain, makeNoisyBursts(48000, 1.0));
            chain.reset();
            const QByteArray silence(48000 * 2 * int(sizeof(float)), '\0');
            const QByteArray out = runThrough(chain, silence);
            QVERIFY2(allFinite(out), qPrintable(method.id));
            const QByteArray after = runThrough(chain, makeNoisyBursts(48000, 1.0));
            QVERIFY2(allFinite(after), qPrintable(method.id));
        }
    }

    void irregularBlockSizesDoNotBreakAMethod()
    {
        for (const NrMethod& method : nrMethods()) {
            RxChain chain(48000);
            auto processor = makeMethod(method, 48000);
            QVERIFY(processor);
            chain.setNrProcessor(processor);
            const QByteArray in = makeNoisyBursts(48000, 2.0);
            qsizetype offset = 0;
            const int sizes[] = {128, 256, 512, 1024, 2048, 100, 333};
            int index = 0;
            QByteArray out;
            while (offset < in.size()) {
                const qsizetype bytes = std::min<qsizetype>(sizes[index++ % 7] * 8, in.size() - offset);
                out += chain.process(in.mid(offset, bytes));
                offset += bytes;
            }
            QVERIFY2(allFinite(out), qPrintable(method.id));
            QVERIFY2(out.size() > in.size() / 2, qPrintable(method.id));
        }
    }

    // The reason an operator-chosen order exists. It must change the samples, or it
    // is cosmetic, which upstream's own test comment records was once the case.
    void stageOrderChangesTheOutput()
    {
        const auto run = [](const QStringList& order) {
            RxChain chain(48000);
            chain.setStageOrder(order);
            chain.setStageParam(QStringLiteral("eq.enabled"), 1);
            chain.setStageParam(QStringLiteral("eq.b4.on"), 1);
            chain.setStageParam(QStringLiteral("eq.b4.gain"), 18);
            chain.setStageParam(QStringLiteral("tube.enabled"), 1);
            chain.setStageParam(QStringLiteral("tube.drive"), 18);
            return runThrough(chain, makeNoisyBursts(48000, 0.5));
        };
        const QByteArray eqFirst = run({QStringLiteral("eq"), QStringLiteral("tube")});
        const QByteArray tubeFirst = run({QStringLiteral("tube"), QStringLiteral("eq")});
        QVERIFY(eqFirst != tubeFirst);
    }

    void aStaleOrderCannotLoseAStage()
    {
        RxChain chain(48000);
        chain.setStageOrder({QStringLiteral("tube"), QStringLiteral("retired"), QStringLiteral("tube")});
        const QStringList order = chain.stageOrder();
        QCOMPARE(order.first(), QStringLiteral("tube"));
        QCOMPARE(order.size(), RxChain::stageIds().size());
        for (const QString& id : RxChain::stageIds()) QVERIFY(order.contains(id));
    }

    // Each stage enabled at its defaults must actually do something audible.
    void everyStageAffectsTheAudio_data()
    {
        QTest::addColumn<QStringList>("settings");
        QTest::newRow("gate") << QStringList{"gate.enabled=1", "gate.threshold=-20"};
        QTest::newRow("eq") << QStringList{"eq.enabled=1", "eq.b4.on=1", "eq.b4.gain=12"};
        QTest::newRow("comp") << QStringList{"comp.enabled=1", "comp.threshold=-30", "comp.ratio=8"};
        QTest::newRow("tube") << QStringList{"tube.enabled=1", "tube.drive=18"};
        QTest::newRow("pudu") << QStringList{"pudu.enabled=1", "pudu.dooHarmonics=18"};
    }

    void everyStageAffectsTheAudio()
    {
        QFETCH(QStringList, settings);
        RxChain chain(48000);
        for (const QString& setting : settings) {
            QVERIFY(chain.setStageParam(setting.section('=', 0, 0), setting.section('=', 1).toDouble()));
        }
        const QByteArray in = makeNoisyBursts(48000, 1.0);
        const QByteArray out = runThrough(chain, in);
        QVERIFY(allFinite(out));
        QVERIFY(out.left(in.size() / 2) != in.left(in.size() / 2));
    }

    void nrBypassPassesAudioThrough()
    {
        const NrMethod& method = nrMethods().first();
        RxChain chain(48000);
        chain.setNrProcessor(makeMethod(method, 48000));
        chain.setNrBypassed(true);
        const QByteArray in = makeNoisyBursts(48000, 0.2);
        QCOMPARE(runThrough(chain, in).left(1024), in.left(1024));
        QVERIFY(!chain.nrActive());
    }

    void transmitBypassSkipsTheWholeChain()
    {
        RxChain chain(48000);
        chain.setNrProcessor(makeMethod(nrMethods().first(), 48000));
        chain.setStageParam(QStringLiteral("tube.enabled"), 1);
        chain.setStageParam(QStringLiteral("tube.drive"), 18);
        chain.setTransmitBypass(true);
        const QByteArray in = makeNoisyBursts(48000, 0.2);
        QCOMPARE(runThrough(chain, in).left(1024), in.left(1024));
        chain.setTransmitBypass(false);
        QVERIFY(runThrough(chain, in).left(1024) != in.left(1024));
    }

    void modePolicyMatchesUpstreamAndCoversTciNames()
    {
        for (const char* mode : {"CW", "cw", "DIGU", "DIGL", "RTTY", "FT8", "FT4", "WSPR", "JT65", "JT9", "BPSK"}) {
            QVERIFY2(modeBypassesNr(QString::fromLatin1(mode)), mode);
        }
        for (const char* mode : {"LSB", "USB", "AM", "NFM", "WFM"}) {
            QVERIFY2(!modeBypassesNr(QString::fromLatin1(mode)), mode);
        }
        QVERIFY(modeIsComplexStereo(QStringLiteral("DIGU")));
        QVERIFY(modeIsComplexStereo(QStringLiteral("digl")));
        QVERIFY(!modeIsComplexStereo(QStringLiteral("USB")));
    }

    // Every method's parameter key must be namespaced by its id: Engine::setParam routes
    // on that prefix, and adapters match on the part after the dot.
    void paramKeysAreNamespacedByMethod()
    {
        for (const NrMethod& method : nrMethods()) {
            for (const ParamSpec& spec : method.params) {
                QVERIFY2(spec.key.startsWith(method.id + QLatin1Char('.')), qPrintable(spec.key));
                QVERIFY2(spec.def >= spec.min && spec.def <= spec.max, qPrintable(spec.key));
            }
        }
    }

    // The NR page shows all seven of upstream's methods, and a missing one must say why.
    void everyUpstreamMethodIsAvailableOrSaysWhyNot()
    {
        for (const char* id : {"nr2", "nr4", "mnr", "dfnr", "rn2", "bnr", "nnr"}) {
            const QString method = QString::fromLatin1(id);
            const QString reason = nrMethodUnavailableReason(method);
            QVERIFY2(reason.isEmpty() == (findNrMethod(method) != nullptr), id);
        }
        QVERIFY(nrMethodUnavailableReason(QStringLiteral("nr2")).isEmpty());
        QVERIFY(nrMethodUnavailableReason(QStringLiteral("rn2")).isEmpty());
        QCOMPARE(nrMethodUnavailableReason(QStringLiteral("bnr")), QStringLiteral("Not available in this build"));
#ifndef __APPLE__
        QCOMPARE(nrMethodUnavailableReason(QStringLiteral("mnr")), QStringLiteral("Needs macOS"));
#endif
#ifndef HAVE_DFNR
        QCOMPARE(nrMethodUnavailableReason(QStringLiteral("dfnr")), QStringLiteral("Not built into this version"));
#endif
    }

    // Display hints for the NR page. They must never change a value or a range.
    void nrParamsCarryThePagesHints()
    {
        for (const NrMethod& method : nrMethods()) {
            for (const ParamSpec& spec : method.params) QVERIFY2(!spec.section.isEmpty(), qPrintable(spec.key));
        }
        const NrMethod* nr2 = findNrMethod(QStringLiteral("nr2"));
        QVERIFY(nr2);
        int fills = 0;
        for (const ParamSpec& spec : nr2->params) {
            if (!spec.enabledByKey.isEmpty()) {
                QCOMPARE(spec.enabledByKey, QStringLiteral("nr2.post2"));
                QCOMPARE(spec.enabledByValue, 1.0);
                ++fills;
            }
        }
        QCOMPARE(fills, 3);  // level, character and band
        if (const NrMethod* mnr = findNrMethod(QStringLiteral("mnr"))) QVERIFY(mnr->params.first().percent);
    }

    // The fill character reaches the filter: with the fill on, it changes what is heard.
    void nr2FillCharacterChangesTheFill()
    {
        const NrMethod* nr2 = findNrMethod(QStringLiteral("nr2"));
        QVERIFY(nr2);
        const QByteArray in = makeNoisyBursts(48000, 2.0);
        const auto run = [&](double factor) {
            std::shared_ptr<NrProcessor> processor(nr2->create(48000).release());
            for (const ParamSpec& spec : nr2->params) processor->setParam(spec.key, spec.def);
            processor->setParam(QStringLiteral("nr2.post2"), 1);
            processor->setParam(QStringLiteral("nr2.post2Level"), 1);
            processor->setParam(QStringLiteral("nr2.post2Factor"), factor);
            RxChain chain(48000);
            chain.setNrProcessor(processor);
            return runThrough(chain, in);
        };
        // The fill's noise is seeded per filter, so two runs never match sample for
        // sample. Compare levels instead, against how far two identical runs differ:
        // all synthetic white noise (1) against all of the noise actually removed (0).
        const double removed = rmsOf(run(0.0));
        const double white = rmsOf(run(1.0));
        const double seedSpread = std::fabs(rmsOf(run(0.15)) - rmsOf(run(0.15)));
        qInfo() << "fill rms: removed noise" << removed << "white" << white << "seed spread" << seedSpread;
        QVERIFY(std::fabs(white - removed) > 10.0 * seedSpread + 1e-4);
    }

    void stageParamsHaveSaneRangesAndAllApply()
    {
        RxChain chain(48000);
        for (const ParamSpec& spec : RxChain::stageParams()) {
            QVERIFY2(spec.def >= spec.min && spec.def <= spec.max, qPrintable(spec.key));
            QVERIFY2(chain.setStageParam(spec.key, spec.def), qPrintable(spec.key));
        }
        QVERIFY(!chain.setStageParam(QStringLiteral("no.such"), 1.0));
    }

    // Upstream's setMode() is a preset overwriting ratio and floor. Loading in table
    // order keeps a saved ratio; that is what this pins.
    void gateModeLoadsBeforeTheValuesItPresets()
    {
        QStringList keys;
        for (const ParamSpec& spec : RxChain::stageParams()) keys << spec.key;
        QVERIFY(keys.indexOf(QStringLiteral("gate.mode")) < keys.indexOf(QStringLiteral("gate.ratio")));
        QVERIFY(keys.indexOf(QStringLiteral("gate.mode")) < keys.indexOf(QStringLiteral("gate.floor")));

        RxChain chain(48000);
        for (const ParamSpec& spec : RxChain::stageParams()) {
            if (spec.key == QLatin1String("gate.mode")) chain.setStageParam(spec.key, 1);
            else if (spec.key == QLatin1String("gate.ratio")) chain.setStageParam(spec.key, 3.5);
            else if (spec.key.startsWith(QLatin1String("gate."))) chain.setStageParam(spec.key, spec.def);
        }
        QCOMPARE(chain.gate().ratio(), 3.5f);
    }

    // ── The table and StageSet ───────────────────────────────────────────────

    // Every stage key through the table and back, on a set of stages that is never
    // processed, as the window's display mirror is used. A read that drifts from its
    // apply shows here.
    void everyStageParamReadsBackWhatWasApplied()
    {
        for (const ParamSpec& spec : RxChain::stageParams()) {
            QList<double> values;
            if (spec.toggle || !spec.choices.isEmpty()) {
                for (int i = int(spec.min); i <= int(spec.max); ++i) values << double(i);
            } else {
                const double step = std::pow(10.0, -spec.decimals);
                values << spec.min << spec.max << spec.def
                       << rounded(spec.min + 0.37 * (spec.max - spec.min), spec.decimals)
                       << rounded(spec.max - step, spec.decimals);  // the finest step the table keeps
            }
            for (double value : values) {
                StageSet stages;
                stages.prepare(48000);
                stages.initialiseBands();
                QVERIFY2(RxChain::applyStageParam(stages, spec.key, value), qPrintable(spec.key));
                double read = 0.0;
                QVERIFY2(RxChain::readStageParam(stages, spec.key, &read), qPrintable(spec.key));
                QVERIFY2(rounded(read, spec.decimals) == rounded(value, spec.decimals),
                         qPrintable(QStringLiteral("%1: applied %2, read %3").arg(spec.key).arg(value).arg(read)));
            }
        }
        StageSet stages;
        double read = 0.0;
        QVERIFY(!RxChain::applyStageParam(stages, QStringLiteral("no.such"), 1.0));
        QVERIFY(!RxChain::readStageParam(stages, QStringLiteral("no.such"), &read));
    }

    // A profile names keys; renaming or dropping one would silently lose it on load.
    void everyKeyAnOlderVersionSavedIsStillInTheTable()
    {
        for (const QString& key : reference::OldChain::keys()) QVERIFY2(stageSpec(key), qPrintable(key));
    }

    // The refactor moved the stages into StageSet and the table onto it. The same
    // settings must still give the same samples, bit for bit, as v0.1.1's chain.
    void theChainSoundsExactlyAsBeforeStageSet_data()
    {
        QTest::addColumn<int>("rate");
        QTest::newRow("48 kHz") << 48000;
        QTest::newRow("24 kHz") << 24000;
    }

    void theChainSoundsExactlyAsBeforeStageSet()
    {
        QFETCH(int, rate);
        const QHash<QString, double> settings = reference::OldChain::v011Settings();
        const QStringList order = {QStringLiteral("tube"), QStringLiteral("gate"), QStringLiteral("pudu"),
                                   QStringLiteral("eq"), QStringLiteral("comp")};

        RxChain chain(rate);
        chain.setStageOrder(order);
        applyLikeTheEngine(chain, settings);

        reference::OldChain old(rate);
        old.setOrder(order);
        for (const QString& key : reference::OldChain::keys()) QVERIFY(old.set(key, settings.value(key, stageSpec(key)->def)));

        const QByteArray in = makeNoisyBursts(rate, 3.0);
        QByteArray expected;
        const int blockBytes = 512 * 2 * int(sizeof(float));
        for (qsizetype offset = 0; offset + blockBytes <= in.size(); offset += blockBytes) {
            expected += old.process(in.mid(offset, blockBytes));
        }
        const QByteArray out = runThrough(chain, in);
        QVERIFY(out != in.left(out.size()));  // the stages did something
        QCOMPARE(out.size(), expected.size());
        QVERIFY2(std::memcmp(out.constData(), expected.constData(), size_t(out.size())) == 0,
                 "a sample differs from v0.1.1's chain");
        QCOMPARE(chain.lastRun(), unsigned(RxChain::GateBit | RxChain::EqBit | RxChain::CompBit | RxChain::TubeBit
                                           | RxChain::PuduBit));
    }

    void theTableNamesWhatThePagesShow()
    {
        QCOMPARE(stageSpec(QStringLiteral("tube.model"))->choices,
                 (QStringList{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}));
        QCOMPARE(stageSpec(QStringLiteral("pudu.mode"))->choices, (QStringList{QStringLiteral("Even"), QStringLiteral("Odd")}));
        // The DSP stops at +12 dB, so a larger value would only have been a dead stretch of knob.
        QCOMPARE(stageSpec(QStringLiteral("tube.output"))->max, 12.0);
        for (int band = 0; band < AetherSDR::ClientEq::kDefaultBandCount; ++band) {
            const ParamSpec* type = stageSpec(QStringLiteral("eq.b%1.type").arg(band));
            const ParamSpec* slope = stageSpec(QStringLiteral("eq.b%1.slope").arg(band));
            QVERIFY(type && slope);
            QCOMPARE(type->choices.size(), 5);
            QCOMPARE(slope->choices.size(), 4);
            QCOMPARE(slope->def, 0.0);  // 12 dB/oct, what every band had
        }
    }

    // Each key keeps at least the precision its vendored page shows (the label formats
    // in Strip*Panel.cpp at the vendored commit), so a value read off a knob saves as
    // shown: 2.55:1 must not come back as 2.6:1.
    void decimalsMatchWhatThePagesShow_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<int>("decimals");
        const QList<QPair<const char*, int>> shown = {
            {"gate.threshold", 1}, {"gate.return", 2}, {"gate.hold", 1}, {"gate.release", 1},
            {"gate.floor", 1}, {"gate.ratio", 1}, {"gate.lookahead", 1},
            {"comp.threshold", 1}, {"comp.ratio", 2}, {"comp.attack", 1}, {"comp.release", 0},
            {"comp.knee", 1}, {"comp.makeup", 1}, {"comp.ceiling", 1},
            {"tube.drive", 2}, {"tube.tone", 2}, {"tube.bias", 2}, {"tube.envelope", 2},
            {"tube.release", 1}, {"tube.output", 2}, {"tube.mix", 2},
            {"pudu.pooDrive", 1}, {"pudu.pooTune", 0}, {"pudu.pooMix", 2}, {"pudu.dooTune", 0},
            {"pudu.dooHarmonics", 1}, {"pudu.dooMix", 2},
            {"eq.b4.freq", 0}, {"eq.b4.gain", 1}, {"eq.b4.q", 2},
        };
        for (const auto& [key, decimals] : shown) QTest::newRow(key) << QString::fromLatin1(key) << decimals;
    }

    void decimalsMatchWhatThePagesShow()
    {
        QFETCH(QString, key);
        QFETCH(int, decimals);
        QVERIFY(stageSpec(key));
        QCOMPARE(stageSpec(key)->decimals, decimals);
    }

    // ── EQ band type and slope ───────────────────────────────────────────────

    void bandTypeAndSlopeChangeTheSound()
    {
        const QByteArray in = makeNoisyBursts(48000, 1.0);
        const auto run = [&](const QList<QPair<const char*, double>>& settings) {
            RxChain chain(48000);
            chain.setStageParam(QStringLiteral("eq.enabled"), 1);
            for (const auto& [key, value] : settings) chain.setStageParam(QString::fromLatin1(key), value);
            return runThrough(chain, in);
        };

        // Band 5, a peak by default, raised 12 dB.
        const QByteArray peak = run({{"eq.b4.on", 1}, {"eq.b4.gain", 12}});
        QCOMPARE(run({{"eq.b4.on", 1}, {"eq.b4.gain", 12}, {"eq.b4.type", 2}}), peak);  // the type it always had
        const QByteArray shelf = run({{"eq.b4.on", 1}, {"eq.b4.gain", 12}, {"eq.b4.type", 1}});
        QVERIFY(shelf != peak);
        QVERIFY(rmsOf(shelf) > rmsOf(peak));  // a low shelf lifts everything below, not one band

        // Band 1, a high-pass by default, moved up to 1 kHz.
        const QByteArray gentle = run({{"eq.b0.on", 1}, {"eq.b0.freq", 1000}});
        QCOMPARE(run({{"eq.b0.on", 1}, {"eq.b0.freq", 1000}, {"eq.b0.slope", 0}}), gentle);  // 12 dB/oct, as always
        const QByteArray steep = run({{"eq.b0.on", 1}, {"eq.b0.freq", 1000}, {"eq.b0.slope", 3}});
        QVERIFY(steep != gentle);
        QVERIFY(rmsOf(steep) < rmsOf(gentle));  // 48 dB/oct removes more of the speech below 1 kHz
    }

    // ── Operator bypass, the run mask and the post-EQ tap ────────────────────

    void operatorBypassPassesTheRadioThroughUntouched()
    {
        RxChain chain(48000);
        auto nr = std::make_shared<CountingNr>();
        chain.setNrProcessor(nr);
        chain.setStageParam(QStringLiteral("tube.enabled"), 1);
        chain.setStageParam(QStringLiteral("tube.drive"), 18);
        chain.setStageParam(QStringLiteral("eq.enabled"), 1);
        chain.setStageParam(QStringLiteral("eq.b4.on"), 1);
        chain.setStageParam(QStringLiteral("eq.b4.gain"), 12);
        const QByteArray in = makeNoisyBursts(48000, 0.5);

        chain.setOperatorBypass(true);
        QVERIFY(chain.operatorBypassed());
        const QByteArray bypassed = runThrough(chain, in);
        QVERIFY(!bypassed.isEmpty());
        QVERIFY(bypassed == in.left(bypassed.size()));  // bit for bit, NR included
        QCOMPARE(nr->processed, 0);
        QVERIFY(!chain.nrActive());
        QCOMPARE(chain.lastRun(), 0u);

        chain.setOperatorBypass(false);
        QCOMPARE(nr->transientResets, 1);  // what played meanwhile is not the noise it knew
        QCOMPARE(nr->resets, 0);           // but the band is the same, so its estimate is kept
        QVERIFY(runThrough(chain, in) != bypassed);
        QVERIFY(nr->processed > 0);
        chain.setOperatorBypass(false);
        QCOMPARE(nr->transientResets, 1);  // not leaving it twice
    }

    void theRunMaskNamesOnlyTheStagesThatRan()
    {
        RxChain chain(48000);
        const QByteArray block = makeNoisyBursts(48000, 0.1).left(512 * 2 * int(sizeof(float)));
        chain.process(block);
        QCOMPARE(chain.lastRun(), 0u);  // all disabled, as they start

        chain.setStageParam(QStringLiteral("gate.enabled"), 1);
        chain.setStageParam(QStringLiteral("comp.enabled"), 1);
        chain.process(block);
        QCOMPARE(chain.lastRun(), unsigned(RxChain::GateBit | RxChain::CompBit));

        for (const QString& id : RxChain::stageIds()) chain.setStageParam(id + QStringLiteral(".enabled"), 1);
        const unsigned all = RxChain::GateBit | RxChain::EqBit | RxChain::CompBit | RxChain::TubeBit | RxChain::PuduBit;
        chain.process(block);
        QCOMPARE(chain.lastRun(), all);

        chain.setTransmitBypass(true);
        chain.process(block);
        QCOMPARE(chain.lastRun(), 0u);
        chain.setTransmitBypass(false);
        chain.process(block);
        QCOMPARE(chain.lastRun(), all);

        chain.setOperatorBypass(true);
        chain.process(block);
        QCOMPARE(chain.lastRun(), 0u);
        chain.setOperatorBypass(false);
        chain.process(block);
        QCOMPARE(chain.lastRun(), all);

        auto nr = std::make_shared<CountingNr>();
        nr->swallow = true;
        chain.setNrProcessor(nr);
        QVERIFY(chain.process(block).isEmpty());
        QCOMPARE(chain.lastRun(), 0u);  // an empty block ran nothing
    }

    void thePostEqTapCarriesWhatTheEqPassedOn()
    {
        SpscStereoRing ring(48000);
        std::atomic<bool> wanted{false};
        const QByteArray in = makeNoisyBursts(48000, 0.5).left(8 * 512 * 2 * int(sizeof(float)));

        RxChain chain(48000);
        chain.setPostEqTap(&ring, &wanted);
        chain.setStageOrder({QStringLiteral("eq"), QStringLiteral("tube")});
        chain.setStageParam(QStringLiteral("tube.enabled"), 1);
        chain.setStageParam(QStringLiteral("tube.drive"), 18);
        runThrough(chain, in);
        QCOMPARE(ring.size(), size_t(0));  // nobody asked

        wanted = true;
        const QByteArray out = runThrough(chain, in);
        // The EQ is off, and its place is still tapped, as upstream does: the radio's
        // audio, before the tube after it shaped it.
        QCOMPARE(drain(ring), in);
        QVERIFY(out != in);

        // On, the tap holds the EQ's own output.
        const auto raise = [](RxChain& c) {
            c.setStageParam(QStringLiteral("eq.enabled"), 1);
            c.setStageParam(QStringLiteral("eq.b4.on"), 1);
            c.setStageParam(QStringLiteral("eq.b4.gain"), 12);
        };
        raise(chain);
        runThrough(chain, in);
        RxChain eqOnly(48000);
        raise(eqOnly);
        QCOMPARE(drain(ring), runThrough(eqOnly, in));

        // Nothing while bypassed, whichever bypass.
        chain.setOperatorBypass(true);
        runThrough(chain, in);
        chain.setOperatorBypass(false);
        chain.setTransmitBypass(true);
        runThrough(chain, in);
        chain.setTransmitBypass(false);
        QCOMPARE(ring.size(), size_t(0));

        // A full ring drops what does not fit, and the chain plays on.
        SpscStereoRing small(1000);
        chain.setPostEqTap(&small, &wanted);
        QCOMPARE(runThrough(chain, in).size(), in.size());
        QCOMPARE(small.size(), size_t(1000));

        chain.setPostEqTap(nullptr, nullptr);
        runThrough(chain, in);
        QCOMPARE(ring.size(), size_t(0));
    }
};

QTEST_GUILESS_MAIN(TestRxChain)
#include "test_rx_chain.moc"
