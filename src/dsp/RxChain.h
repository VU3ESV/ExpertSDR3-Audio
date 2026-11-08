#pragma once

#include "dsp/NrProcessor.h"
#include "dsp/ParamSpec.h"
#include "dsp/StageSet.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>

namespace esa {

class SpscStereoRing;

// The AetherRX receive chain: at most one noise-reduction method, then Gate, EQ,
// Comp, Tube and Pudu in an operator-chosen order. This mirrors
// AudioEngine::processMixedRxAudioData upstream, without the Flex/Kiwi routing.
//
// Not thread-safe: owned and called by one thread.
class RxChain {
public:
    explicit RxChain(int sampleRate);

    int sampleRate() const { return m_sampleRate; }

    // Runs the chain. A method may buffer, so the output can differ in length from
    // the input and can be empty. The returned buffer is valid until the next call.
    const QByteArray& process(const QByteArray& stereoIn);

    // Null turns NR off.
    void setNrProcessor(std::shared_ptr<NrProcessor> processor);
    bool hasNrProcessor() const { return m_nr != nullptr; }
    NrProcessor* nrProcessor() { return m_nr.get(); }

    // Bypass NR only (a CW or digital mode). Leaving the bypass resets the method,
    // because its noise profile describes a stream that is no longer playing.
    void setNrBypassed(bool bypassed);
    bool nrBypassed() const { return m_nrBypassed; }

    // Bypass everything while transmitting, so stateful stages do not adapt to the
    // TX gap. Leaving it clears transients but keeps the noise estimate.
    void setTransmitBypass(bool bypassed);

    // The operator's BYPASS: the input passes through untouched, NR included, exactly
    // as while transmitting, and leaving it clears transients the same way.
    void setOperatorBypass(bool bypassed);
    bool operatorBypassed() const { return m_operatorBypassed; }

    // Post-EQ samples for the EQ's analyser, written straight after the EQ's place in
    // the order whether or not the EQ is enabled, as upstream does. Only while
    // `*wanted` is true, and never for an empty or bypassed block. A full ring drops
    // the excess. Both must outlive the chain, or be replaced first; null detaches.
    void setPostEqTap(SpscStereoRing* ring, const std::atomic<bool>* wanted);

    // Discontinuity: a different receiver or a reconnect.
    void reset();

    // Ids from stageIds(). Unknown ids are dropped; missing ones keep their
    // default relative order at the end, so a stale setting cannot lose a stage.
    void setStageOrder(const QStringList& order);
    QStringList stageOrder() const { return m_order; }

    bool setStageParam(const QString& key, double value);

    // The parameter table on any set of stages, such as a display mirror that is never
    // processed. False for a key that is not a stage parameter. read returns what the
    // stages hold, which can differ from what was applied by float precision.
    static bool applyStageParam(StageSet& stages, const QString& key, double value);
    static bool readStageParam(const StageSet& stages, const QString& key, double* value);

    // Post-NR / pre-NR RMS ratio of the latest block; 1 when NR is idle.
    float nrGain() const { return m_nrGain; }
    bool nrActive() const { return m_nrActive; }

    // Bits of lastRun(), one per stage.
    enum StageBit : unsigned {
        GateBit = 1u << 0,
        EqBit = 1u << 1,
        CompBit = 1u << 2,
        TubeBit = 1u << 3,
        PuduBit = 1u << 4,
    };
    // The stages that processed the latest block. 0 when it passed through untouched:
    // every stage off, bypassed, or empty while a method's first block fills.
    unsigned lastRun() const { return m_lastRun; }

    // Stage ids in upstream's default order.
    static QStringList stageIds();
    // Unknown ids dropped, duplicates removed, and missing stages added at the end.
    static QStringList normalizedOrder(const QStringList& order);
    static QString stageLabel(const QString& id);
    // Gate, EQ, Comp, Tube and Pudu parameters.
    static const QList<ParamSpec>& stageParams();

    StageSet& stages() { return m_stages; }
    const StageSet& stages() const { return m_stages; }
    AetherSDR::ClientGate& gate() { return m_stages.gate; }
    AetherSDR::ClientEq& eq() { return m_stages.eq; }
    AetherSDR::ClientComp& comp() { return m_stages.comp; }
    AetherSDR::ClientTube& tube() { return m_stages.tube; }
    AetherSDR::ClientPudu& pudu() { return m_stages.pudu; }

private:
    const int m_sampleRate;
    StageSet m_stages;
    std::shared_ptr<NrProcessor> m_nr;
    QStringList m_order;
    bool m_nrBypassed{false};
    bool m_txBypassed{false};
    bool m_operatorBypassed{false};
    bool m_nrActive{false};
    float m_nrGain{1.0f};
    unsigned m_lastRun{0};
    SpscStereoRing* m_eqTap{nullptr};
    const std::atomic<bool>* m_eqTapWanted{nullptr};
    QByteArray m_output;
};

} // namespace esa
