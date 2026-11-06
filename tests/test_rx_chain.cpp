#include "dsp/NrProcessor.h"
#include "dsp/RxChain.h"

#include <QtTest>

#include <algorithm>
#include <cmath>
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
};

QTEST_GUILESS_MAIN(TestRxChain)
#include "test_rx_chain.moc"
