#pragma once

#include "dsp/NrProcessor.h"
#include "dsp/ParamSpec.h"

#include "ClientComp.h"
#include "ClientEq.h"
#include "ClientGate.h"
#include "ClientPudu.h"
#include "ClientTube.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

namespace esa {

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

    // Discontinuity: a different receiver or a reconnect.
    void reset();

    // Ids from stageIds(). Unknown ids are dropped; missing ones keep their
    // default relative order at the end, so a stale setting cannot lose a stage.
    void setStageOrder(const QStringList& order);
    QStringList stageOrder() const { return m_order; }

    bool setStageParam(const QString& key, double value);

    // Post-NR / pre-NR RMS ratio of the latest block; 1 when NR is idle.
    float nrGain() const { return m_nrGain; }
    bool nrActive() const { return m_nrActive; }

    // Stage ids in upstream's default order.
    static QStringList stageIds();
    static QString stageLabel(const QString& id);
    // Gate, EQ, Comp, Tube and Pudu parameters.
    static const QList<ParamSpec>& stageParams();

    AetherSDR::ClientGate& gate() { return m_gate; }
    AetherSDR::ClientEq& eq() { return m_eq; }
    AetherSDR::ClientComp& comp() { return m_comp; }
    AetherSDR::ClientTube& tube() { return m_tube; }
    AetherSDR::ClientPudu& pudu() { return m_pudu; }

private:
    const int m_sampleRate;
    AetherSDR::ClientGate m_gate;
    AetherSDR::ClientEq m_eq;
    AetherSDR::ClientComp m_comp;
    AetherSDR::ClientTube m_tube;
    AetherSDR::ClientPudu m_pudu;
    std::shared_ptr<NrProcessor> m_nr;
    QStringList m_order;
    bool m_nrBypassed{false};
    bool m_txBypassed{false};
    bool m_nrActive{false};
    float m_nrGain{1.0f};
    QByteArray m_output;
};

} // namespace esa
