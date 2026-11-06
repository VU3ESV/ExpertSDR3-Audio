#include "audio/DriftController.h"
#include "audio/SpscRing.h"

#include <QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

using namespace esa;

namespace {

struct DriftResult {
    double finalErrorMs{0.0};
    double worstLateErrorMs{0.0};
    double finalPpm{0.0};
    double minFill{1e12};
};

// Simulates a TCI producer in bursty blocks against a device consuming fixed buffers,
// with the two clocks disagreeing by `clockErrorPpm`.
DriftResult simulate(double clockErrorPpm, double seconds, int blockFrames, double jitterMs)
{
    constexpr double kRate = 48000.0;
    constexpr int kDeviceBuffer = 256;
    const double target = kRate * 0.06;

    DriftController drift(kRate, target);
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> jitter(-jitterMs, jitterMs);

    double fill = target;
    double t = 0.0;
    const double blockPeriod = blockFrames / (kRate * (1.0 + clockErrorPpm * 1e-6));
    // Each block arrives at its ideal time plus jitter. Jitter must not accumulate:
    // the sender runs off a clock, so it wobbles around that clock, not away from it.
    double idealBlock = blockPeriod;
    double nextBlock = idealBlock;
    DriftResult result;

    const double devicePeriod = kDeviceBuffer / kRate;
    while (t < seconds) {
        t += devicePeriod;
        while (nextBlock <= t) {
            fill += blockFrames;
            idealBlock += blockPeriod;
            nextBlock = idealBlock + jitter(rng) * 1e-3;
        }
        const double ratio = drift.update(fill, kDeviceBuffer);
        fill -= kDeviceBuffer * ratio;
        result.minFill = std::min(result.minFill, fill);
        // Judge only after a settling time well beyond the controller's time constants.
        if (t > seconds * 0.75) {
            result.worstLateErrorMs = std::max(result.worstLateErrorMs,
                                               std::abs(drift.filteredFill() - target) / kRate * 1000.0);
        }
    }
    result.finalErrorMs = (drift.filteredFill() - target) / kRate * 1000.0;
    result.finalPpm = drift.correctionPpm();
    return result;
}

} // namespace

class TestDrift : public QObject {
    Q_OBJECT

private slots:
    void holdsTheBufferDespiteClockError_data()
    {
        QTest::addColumn<double>("ppm");
        QTest::newRow("-500ppm") << -500.0;
        QTest::newRow("-100ppm") << -100.0;
        QTest::newRow("none") << 0.0;
        QTest::newRow("+100ppm") << 100.0;
        QTest::newRow("+500ppm") << 500.0;
    }

    void holdsTheBufferDespiteClockError()
    {
        QFETCH(double, ppm);
        const DriftResult r = simulate(ppm, 900.0, 512, 3.0);
        QVERIFY2(r.worstLateErrorMs < 3.0, qPrintable(QString::number(r.worstLateErrorMs)));
        // Settling on the real clock error shows it learned it rather than happening
        // to sit near target.
        QVERIFY2(std::abs(r.finalPpm - ppm) < 30.0, qPrintable(QString::number(r.finalPpm)));
        QVERIFY2(r.minFill > 0.0, "the buffer ran dry");
    }

    // Without correction a 50 ppm error empties a 60 ms buffer in 20 minutes. This is
    // the failure the controller exists to prevent, so check it does not happen.
    void noUnderrunOverHoursOfRealisticDrift()
    {
        const DriftResult r = simulate(50.0, 4 * 3600.0, 512, 3.0);
        QVERIFY(r.minFill > 0.0);
        QVERIFY(std::abs(r.finalErrorMs) < 3.0);
    }

    void survivesTheDefaultLargeBlocks()
    {
        const DriftResult r = simulate(100.0, 900.0, 2048, 5.0);
        QVERIFY(r.minFill > 0.0);
    }

    void correctionStaysInaudiblySmall()
    {
        DriftController drift(48000.0, 2880.0);
        double ratio = 1.0;
        for (int i = 0; i < 100000; ++i) ratio = drift.update(48000.0, 256);  // a second too much
        QVERIFY(std::abs(ratio - 1.0) <= DriftController::kMaxCorrection + 1e-12);
    }

    void resetKeepsTheLearnedClockErrorUnlessTold()
    {
        DriftController drift(48000.0, 2880.0);
        for (int i = 0; i < 40000; ++i) drift.update(2880.0 + 500.0, 256);
        const double learned = drift.correctionPpm();
        QVERIFY(learned > 0.0);

        drift.reset(2880.0);
        drift.update(2880.0, 256);
        QVERIFY(drift.correctionPpm() > 0.0);

        drift.reset(2880.0, false);
        drift.update(2880.0, 256);
        QCOMPARE(drift.correctionPpm(), 0.0);
    }

    void ringIsFifoAcrossTheWrap()
    {
        SpscStereoRing ring(4);
        float out[8];
        const float a[6] = {1, 2, 3, 4, 5, 6};
        QCOMPARE(ring.write(a, 3), size_t(3));
        QCOMPARE(ring.read(out, 2), size_t(2));
        const float b[6] = {7, 8, 9, 10, 11, 12};
        QCOMPARE(ring.write(b, 3), size_t(3));  // wraps
        QCOMPARE(ring.size(), size_t(4));
        QCOMPARE(ring.read(out, 4), size_t(4));
        const float expected[8] = {5, 6, 7, 8, 9, 10, 11, 12};
        for (int i = 0; i < 8; ++i) QCOMPARE(out[i], expected[i]);
    }

    void ringDropsTheExcessWhenFull()
    {
        SpscStereoRing ring(2);
        const float a[6] = {1, 2, 3, 4, 5, 6};
        QCOMPARE(ring.write(a, 3), size_t(2));
        QCOMPARE(ring.size(), size_t(2));
    }

    void ringSkipDiscardsTheOldest()
    {
        SpscStereoRing ring(4);
        const float a[6] = {1, 2, 3, 4, 5, 6};
        ring.write(a, 3);
        QCOMPARE(ring.skip(2), size_t(2));
        float out[2];
        QCOMPARE(ring.read(out, 1), size_t(1));
        QCOMPARE(out[0], 5.0f);
    }

    // Real threads, as in production. Order must hold, with no lost or torn frames.
    void ringSurvivesRealConcurrency()
    {
        SpscStereoRing ring(257);
        constexpr int kFrames = 500000;
        std::atomic<bool> failed{false};

        std::thread producer([&] {
            std::vector<float> block(64 * 2);
            int next = 0;
            while (next < kFrames) {
                const int count = std::min(64, kFrames - next);
                for (int i = 0; i < count; ++i) {
                    block[size_t(i) * 2] = float(next + i);
                    block[size_t(i) * 2 + 1] = -float(next + i);
                }
                int written = 0;
                while (written < count) {
                    written += int(ring.write(block.data() + written * 2, size_t(count - written)));
                    if (written < count) std::this_thread::yield();
                }
                next += count;
            }
        });

        int expected = 0;
        float frame[2];
        while (expected < kFrames) {
            if (ring.read(frame, 1) == 0) {
                std::this_thread::yield();
                continue;
            }
            if (frame[0] != float(expected) || frame[1] != -float(expected)) {
                failed = true;
                break;
            }
            ++expected;
        }
        producer.join();
        QVERIFY(!failed.load());
    }
};

QTEST_APPLESS_MAIN(TestDrift)
#include "test_drift.moc"
