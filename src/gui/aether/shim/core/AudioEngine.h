#pragma once

// Stands in for AetherSDR's core/AudioEngine.h, which the vendored AetherRX panels
// include. Upstream's class is the whole audio engine; this one declares only what those
// panels call: the RX stage objects and their save calls, mute, boost and the output
// trim, the EQ analyser's sample tap, RN2 for TX, and three scope signals. Nothing here
// processes audio. Each call forwards to a Hooks entry the host fills in; an empty hook
// reads as off and drops writes, so a bare instance is safe to hand to a panel.
//
// GUI thread only, like the panels that call it.

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>

namespace AetherSDR {

class ClientComp;
class ClientEq;
class ClientGate;
class ClientPudu;
class ClientTube;

class AudioEngine : public QObject {
    Q_OBJECT

public:
    struct Hooks {
        // What the panels draw from and write into. The host owns these, and they must
        // outlive every panel: a panel keeps the pointer it was shown with. They are a
        // display copy that no audio thread ever processes.
        ClientGate* gate{nullptr};
        ClientEq* eq{nullptr};
        ClientComp* comp{nullptr};
        ClientTube* tube{nullptr};
        ClientPudu* pudu{nullptr};

        // A panel changed one stage and asks for it to be kept. The id is RxChain's:
        // "gate", "eq", "comp", "tube" or "pudu".
        std::function<void(const QString& stage)> saveStage;

        std::function<bool()> muted;
        std::function<void(bool)> setMuted;
        std::function<bool()> boost;
        std::function<void(bool)> setBoost;
        // Upstream's RX output trim, which the Final Output knob drives.
        std::function<float()> outputDb;
        std::function<void(float)> setOutputDb;

        // Fills `out` with the latest `count` post-EQ mono samples for the analyser.
        // False when there are none yet.
        std::function<bool(float* out, int count)> copyEqSamples;
    };

    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine() override;

    // Set before the first panel is built: panels read the stage pointers once.
    void setHooks(Hooks hooks);
    const Hooks& hooks() const { return m_hooks; }

    // For the host's output feed. This app only receives, so the shared scope signal
    // always says tx=false and the TX scope signal is never emitted.
    void publishRxScope(const QByteArray& monoFloat32Pcm, int sampleRate);
    void publishRxPostChainScope(const QByteArray& monoFloat32Pcm, int sampleRate);

    // ── Upstream's surface, exactly as the vendored panels use it ──────────────
    ClientGate* clientGateRx() { return m_hooks.gate; }
    ClientGate* clientGateTx() { return nullptr; }
    ClientEq* clientEqRx() { return m_hooks.eq; }
    ClientEq* clientEqTx() { return nullptr; }
    ClientComp* clientCompRx() { return m_hooks.comp; }
    ClientComp* clientCompTx() { return nullptr; }
    ClientTube* clientTubeRx() { return m_hooks.tube; }
    ClientTube* clientTubeTx() { return nullptr; }
    ClientPudu* clientPuduRx() { return m_hooks.pudu; }
    ClientPudu* clientPuduTx() { return nullptr; }

    // The forms without "Rx" are upstream's TX saves. Panels here are only ever shown
    // for RX, but should one call them, there is only the one stage to mean.
    void saveClientGateRxSettings() const;
    void saveClientGateSettings() const;
    void saveClientEqSettings() const;
    void saveClientCompRxSettings() const;
    void saveClientCompSettings() const;
    void saveClientTubeRxSettings() const;
    void saveClientTubeSettings() const;
    void saveClientPuduRxSettings() const;
    void saveClientPuduSettings() const;

    bool isMuted() const;
    void setMuted(bool muted);
    bool rxBoost() const;
    void setRxBoost(bool on);
    float rxOutputTrimDb() const;
    void setRxOutputTrimDb(float db);

    bool copyRecentClientEqRxSamples(float* out, int count) const;
    bool copyRecentClientEqTxSamples(float* out, int count) const;

    // The Gate panel's RN2 toggle is for the TX microphone, and hidden on RX.
    bool rn2TxEnabled() const { return false; }
    void setRn2TxEnabled(bool) {}

signals:
    void scopeSamplesReady(const QByteArray& monoFloat32Pcm, int sampleRate, bool tx);
    void txPostChainScopeReady(const QByteArray& monoFloat32Pcm, int sampleRate);
    void rxPostChainScopeReady(const QByteArray& monoFloat32Pcm, int sampleRate);

private:
    void save(const char* stage) const;

    Hooks m_hooks;
};

} // namespace AetherSDR
