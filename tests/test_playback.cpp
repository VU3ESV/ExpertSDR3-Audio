#include "audio/OutputShape.h"
#include "audio/PlaybackBuffer.h"

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

using esa::PlaybackBuffer;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kBufferFrames = 256;

std::vector<float> stereoSine(double hz, int rate, int frames, double amplitude = 0.5, double phaseFrames = 0.0)
{
    std::vector<float> out(size_t(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const auto v = float(amplitude * std::sin(2.0 * kPi * hz * (i + phaseFrames) / rate));
        out[size_t(i) * 2] = v;
        out[size_t(i) * 2 + 1] = v;
    }
    return out;
}

std::vector<float> renderFrames(PlaybackBuffer& buffer, int frames, int channels = 2)
{
    std::vector<float> out(size_t(frames) * size_t(channels));
    int done = 0;
    while (done < frames) {
        const int n = std::min(kBufferFrames, frames - done);
        buffer.render(out.data() + size_t(done) * size_t(channels), (unsigned long)n);
        done += n;
    }
    return out;
}

// Frequency from upward zero crossings of the left channel: simple and unambiguous.
double measureHz(const std::vector<float>& stereo, int rate, int channels = 2)
{
    int crossings = 0;
    size_t first = 0;
    size_t last = 0;
    const size_t frames = stereo.size() / size_t(channels);
    for (size_t i = 1; i < frames; ++i) {
        if (stereo[(i - 1) * size_t(channels)] < 0.0f && stereo[i * size_t(channels)] >= 0.0f) {
            if (crossings == 0) first = i;
            last = i;
            ++crossings;
        }
    }
    if (crossings < 2) return 0.0;
    return double(crossings - 1) * rate / double(last - first);
}

float peakOf(const std::vector<float>& samples, size_t start = 0)
{
    float peak = 0.0f;
    for (size_t i = start; i < samples.size(); ++i) peak = std::max(peak, std::fabs(samples[i]));
    return peak;
}

// Feeds and renders in lockstep the way a live stream does. Dumping many seconds into
// the buffer at once would, correctly, trip its overrun recovery instead.
class LiveFeed {
public:
    using Generator = std::function<void(float* stereo, int frames, qint64 position)>;

    LiveFeed(PlaybackBuffer& buffer, Generator generator, int channels = 2, int primeFrames = 2880)
        : m_buffer(buffer), m_generator(std::move(generator)), m_channels(channels)
    {
        push(primeFrames);
    }

    std::vector<float> play(int frames)
    {
        std::vector<float> out(size_t(frames) * size_t(m_channels));
        int done = 0;
        while (done < frames) {
            const int n = std::min(kBufferFrames, frames - done);
            push(n);
            m_buffer.render(out.data() + size_t(done) * size_t(m_channels), (unsigned long)n);
            done += n;
        }
        return out;
    }

private:
    void push(int frames)
    {
        std::vector<float> block(size_t(frames) * 2);
        m_generator(block.data(), frames, m_position);
        m_position += frames;
        m_buffer.write(block.data(), frames);
    }

    PlaybackBuffer& m_buffer;
    Generator m_generator;
    int m_channels;
    qint64 m_position{0};
};

LiveFeed::Generator sine(double hz, int rate, double amplitude)
{
    return [=](float* stereo, int frames, qint64 position) {
        for (int i = 0; i < frames; ++i) {
            const auto v = float(amplitude * std::sin(2.0 * kPi * hz * double(position + i) / rate));
            stereo[i * 2] = v;
            stereo[i * 2 + 1] = v;
        }
    };
}

} // namespace

class TestPlayback : public QObject {
    Q_OBJECT

private slots:
    void staysSilentUntilTheCushionIsBuilt()
    {
        PlaybackBuffer buffer(48000, 48000, 2);
        buffer.setTargetLatencyMs(50);
        const auto almost = stereoSine(1000, 48000, 2399);  // one frame short of 50 ms
        buffer.write(almost.data(), 2399);
        QCOMPARE(peakOf(renderFrames(buffer, kBufferFrames)), 0.0f);
        QCOMPARE(buffer.stats().framesPlayed, quint64(0));

        const auto rest = stereoSine(1000, 48000, 48000);
        buffer.write(rest.data(), 2400);
        QVERIFY(peakOf(renderFrames(buffer, 2000)) > 0.1f);
        QVERIFY(buffer.stats().framesPlayed > 0);
    }

    void aSineSurvivesUnchangedInPitchAndLevel()
    {
        PlaybackBuffer buffer(48000, 48000, 2);
        LiveFeed feed(buffer, sine(1000, 48000, 0.5));
        const auto out = feed.play(48000);
        QVERIFY2(std::abs(measureHz(out, 48000) - 1000.0) < 1.0, qPrintable(QString::number(measureHz(out, 48000))));
        QVERIFY2(std::abs(peakOf(out, 4800) - 0.5f) < 0.01f, qPrintable(QString::number(peakOf(out, 4800))));
        QCOMPARE(buffer.stats().underruns, quint64(0));
    }

    void leftAndRightStayOnTheirOwnSides()
    {
        PlaybackBuffer buffer(48000, 48000, 2);
        LiveFeed feed(buffer, [](float* stereo, int frames, qint64) {
            for (int i = 0; i < frames; ++i) {
                stereo[i * 2] = 0.5f;
                stereo[i * 2 + 1] = 0.0f;
            }
        });
        const auto out = feed.play(12000);
        float left = 0.0f;
        float right = 0.0f;
        for (size_t i = 4800; i + 1 < out.size(); i += 2) {
            left = std::max(left, std::fabs(out[i]));
            right = std::max(right, std::fabs(out[i + 1]));
        }
        QVERIFY(left > 0.4f);
        QCOMPARE(right, 0.0f);
    }

    void aMonoDeviceGetsAMixOfBothChannels()
    {
        PlaybackBuffer buffer(48000, 48000, 1);
        LiveFeed feed(buffer, [](float* stereo, int frames, qint64) {
            for (int i = 0; i < frames; ++i) {
                stereo[i * 2] = 0.8f;
                stereo[i * 2 + 1] = 0.0f;
            }
        }, 1);
        const auto out = feed.play(12000);
        QVERIFY2(std::abs(peakOf(out, 4800) - 0.4f) < 0.01f, qPrintable(QString::number(peakOf(out, 4800))));
    }

    void convertsRateWithoutChangingPitch_data()
    {
        QTest::addColumn<int>("chainRate");
        QTest::addColumn<int>("deviceRate");
        QTest::newRow("48k to 44.1k") << 48000 << 44100;
        QTest::newRow("48k to 96k") << 48000 << 96000;
        QTest::newRow("48k to 32k") << 48000 << 32000;
        QTest::newRow("24k to 48k") << 24000 << 48000;
        QTest::newRow("24k to 44.1k") << 24000 << 44100;
    }

    void convertsRateWithoutChangingPitch()
    {
        QFETCH(int, chainRate);
        QFETCH(int, deviceRate);
        PlaybackBuffer buffer(chainRate, deviceRate, 2);

        // Blocks in, interleaved with renders, as in real use.
        const int blockFrames = 512;
        const int seconds = 3;
        std::vector<float> collected;
        const int renderPerBlock = int(double(blockFrames) * deviceRate / chainRate);
        int rendered = 0;
        for (int block = 0; block < seconds * chainRate / blockFrames; ++block) {
            const auto in = stereoSine(1000, chainRate, blockFrames, 0.5, double(block) * blockFrames);
            buffer.write(in.data(), blockFrames);
            const auto out = renderFrames(buffer, renderPerBlock);
            collected.insert(collected.end(), out.begin(), out.end());
            rendered += renderPerBlock;
        }
        Q_UNUSED(rendered)
        const size_t settled = size_t(deviceRate) * 2;  // the last second-ish of stereo samples
        const std::vector<float> tail(collected.begin() + qsizetype(settled), collected.end());
        const double hz = measureHz(tail, deviceRate);
        QVERIFY2(std::abs(hz - 1000.0) < 5.0, qPrintable(QString::number(hz)));
        QVERIFY2(std::abs(peakOf(tail) - 0.5f) < 0.05f, qPrintable(QString::number(peakOf(tail))));
        QCOMPARE(buffer.stats().overruns, quint64(0));
    }

    void anUnderrunIsCountedSilencedAndGrowsTheBuffer()
    {
        PlaybackBuffer buffer(48000, 48000, 2);
        buffer.setTargetLatencyMs(20);
        const auto in = stereoSine(1000, 48000, 1200);
        buffer.write(in.data(), 1200);
        const double targetBefore = buffer.stats().targetMs;

        // Render past the end of what was written.
        const auto out = renderFrames(buffer, 4800);
        const auto stats = buffer.stats();
        QCOMPARE(stats.underruns, quint64(1));
        QCOMPARE(stats.targetMs, targetBefore + PlaybackBuffer::kUnderrunBumpMs);
        QCOMPARE(out.back(), 0.0f);

        // Silent until the larger cushion is rebuilt, then resumes.
        const auto more = stereoSine(1000, 48000, 48000 / 2);
        buffer.write(more.data(), 1200);  // 25 ms, short of the new 30 ms target
        QCOMPARE(peakOf(renderFrames(buffer, kBufferFrames)), 0.0f);
        buffer.write(more.data(), 12000);
        QVERIFY(peakOf(renderFrames(buffer, 4800)) > 0.1f);
    }

    void oneStallDoesNotCountAsManyUnderruns()
    {
        PlaybackBuffer buffer(48000, 48000, 2);
        const auto in = stereoSine(1000, 48000, 4800);
        buffer.write(in.data(), 4800);
        renderFrames(buffer, 48000);  // a full second of nothing to play
        QCOMPARE(buffer.stats().underruns, quint64(1));
    }

    void floodingJumpsBackToTargetInsteadOfStayingLate()
    {
        PlaybackBuffer buffer(48000, 48000, 2);
        buffer.setTargetLatencyMs(60);
        const auto in = stereoSine(1000, 48000, 40000);
        buffer.write(in.data(), 40000);  // far beyond target + slack
        renderFrames(buffer, kBufferFrames);
        const auto stats = buffer.stats();
        QCOMPARE(stats.overruns, quint64(1));
        QVERIFY2(stats.bufferedMs < 70.0, qPrintable(QString::number(stats.bufferedMs)));
    }

    void movingTheBufferControlResetsWhatUnderrunsAdded()
    {
        PlaybackBuffer buffer(48000, 48000, 2);
        buffer.setTargetLatencyMs(20);
        const auto in = stereoSine(1000, 48000, 1200);
        buffer.write(in.data(), 1200);
        renderFrames(buffer, 4800);
        QVERIFY(buffer.stats().targetMs > 20.0);
        buffer.setTargetLatencyMs(20);
        QCOMPARE(buffer.stats().targetMs, 20.0);
    }

    void gainIsAppliedAndMuteSilences()
    {
        PlaybackBuffer buffer(48000, 48000, 2);
        LiveFeed feed(buffer, sine(1000, 48000, 0.5));
        feed.play(4800);

        buffer.setGainDb(-20.0);
        feed.play(kBufferFrames);  // the ramp
        QVERIFY(std::abs(peakOf(feed.play(2400)) - 0.05f) < 0.002f);

        buffer.setMuted(true);
        feed.play(kBufferFrames);
        QCOMPARE(peakOf(feed.play(2400)), 0.0f);

        buffer.setMuted(false);
        feed.play(kBufferFrames);
        QVERIFY(std::abs(peakOf(feed.play(2400)) - 0.05f) < 0.002f);
    }

    void gainChangesRampInsteadOfStepping()
    {
        PlaybackBuffer buffer(48000, 48000, 2);
        LiveFeed feed(buffer, [](float* stereo, int frames, qint64) {
            std::fill(stereo, stereo + frames * 2, 0.5f);
        });
        feed.play(4800);
        buffer.setGainDb(-40.0);
        const auto out = feed.play(kBufferFrames);
        float biggestStep = 0.0f;
        for (size_t i = 2; i < out.size(); i += 2) biggestStep = std::max(biggestStep, std::fabs(out[i] - out[i - 2]));
        QVERIFY2(biggestStep < 0.01f, qPrintable(QString::number(biggestStep)));
    }

    void nothingEverExceedsFullScale()
    {
        PlaybackBuffer buffer(48000, 48000, 2);
        buffer.setGainDb(30.0);
        LiveFeed feed(buffer, sine(1000, 48000, 0.9));
        const auto out = feed.play(12000);
        QCOMPARE(peakOf(out), 1.0f);  // reached the limit, and held it there
    }

    void boostLiftsQuietAudioAndStaysBelowFullScale()
    {
        PlaybackBuffer plain(48000, 48000, 2);
        PlaybackBuffer boosted(48000, 48000, 2);
        boosted.setBoost(true);
        LiveFeed plainFeed(plain, sine(1000, 48000, 0.05));
        LiveFeed boostedFeed(boosted, sine(1000, 48000, 0.05));
        plainFeed.play(4800);
        boostedFeed.play(4800);
        const float gain = peakOf(boostedFeed.play(2400)) / peakOf(plainFeed.play(2400));
        QVERIFY2(std::abs(gain - 2.0f) < 0.05f, qPrintable(QString::number(gain)));

        PlaybackBuffer hot(48000, 48000, 2);
        hot.setBoost(true);
        LiveFeed hotFeed(hot, sine(1000, 48000, 1.0));
        QVERIFY(peakOf(hotFeed.play(12000)) <= 1.0f);
    }

    // The whole thing together: a producer whose clock runs fast, in bursts, for a long
    // time. Without working drift correction this would underrun or overrun repeatedly.
    void aLongRunWithClockErrorNeverGlitches_data()
    {
        QTest::addColumn<double>("ppm");
        QTest::newRow("radio 200 ppm fast") << 200.0;
        QTest::newRow("radio 200 ppm slow") << -200.0;
    }

    void aLongRunWithClockErrorNeverGlitches()
    {
        QFETCH(double, ppm);
        PlaybackBuffer buffer(48000, 48000, 2);
        std::vector<float> block(512 * 2, 0.1f);
        std::vector<float> out(size_t(kBufferFrames) * 2);

        const double blockEvery = 512.0 / (48000.0 * (1.0 + ppm * 1e-6));
        double nextBlock = 0.0;
        double t = 0.0;
        const double step = kBufferFrames / 48000.0;
        for (t = 0.0; t < 600.0; t += step) {
            while (nextBlock <= t) {
                buffer.write(block.data(), 512);
                nextBlock += blockEvery;
            }
            buffer.render(out.data(), kBufferFrames);
        }
        const auto stats = buffer.stats();
        QCOMPARE(stats.underruns, quint64(0));
        QCOMPARE(stats.overruns, quint64(0));
        QVERIFY2(std::abs(stats.driftPpm - ppm) < 30.0, qPrintable(QString::number(stats.driftPpm)));
        QVERIFY2(std::abs(stats.bufferedMs - 60.0) < 15.0, qPrintable(QString::number(stats.bufferedMs)));
    }

    // shapeOutput() took over the per-sample code in render(), and anything that shows
    // the output level uses it too. It must be that code exactly: volume, boost, clamp,
    // in that order and in float, bit for bit, including at and past the edges.
    void theOutputShapeIsExactlyTheCodeItReplaced()
    {
        const auto before = [](float x, float gain, bool boost) {  // render() in v0.1.1
            float v = x * gain;
            if (boost) v = std::tanh(v * 2.0f);
            return std::clamp(v, -1.0f, 1.0f);
        };
        std::vector<float> inputs = {0.0f, -0.0f, 1e-40f, -1e-40f, 1e-8f, 0.25f, 0.5f, 0.999f, 1.0f, -1.0f,
                                     1.0001f, 2.0f, 10.0f, 1e30f, -1e30f,
                                     std::numeric_limits<float>::infinity(),
                                     -std::numeric_limits<float>::infinity(),
                                     std::numeric_limits<float>::quiet_NaN()};
        for (int i = -4096; i <= 4096; ++i) inputs.push_back(float(i) / 1024.0f);
        const float gains[] = {0.0f, 1e-6f, 0.001f, 0.0631f, 0.5f, 0.99f, 1.0f, 1.0116f, 2.0f, 31.62f};
        int checked = 0;
        for (float x : inputs) {
            for (float gain : gains) {
                for (bool boost : {false, true}) {
                    const float got = esa::shapeOutput(x, gain, boost);
                    const float want = before(x, gain, boost);
                    QVERIFY2(std::memcmp(&got, &want, sizeof(float)) == 0,
                             qPrintable(QStringLiteral("x %1, gain %2, boost %3: %4 against %5")
                                            .arg(x).arg(gain).arg(boost).arg(got).arg(want)));
                    ++checked;
                }
            }
        }
        QVERIFY(checked > 100000);
    }
};

QTEST_APPLESS_MAIN(TestPlayback)
#include "test_playback.moc"
