#include "core/AudioEngine.h"

#include <utility>

namespace AetherSDR {

AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent)
{
}

AudioEngine::~AudioEngine() = default;

void AudioEngine::setHooks(Hooks hooks)
{
    m_hooks = std::move(hooks);
}

void AudioEngine::publishRxScope(const QByteArray& monoFloat32Pcm, int sampleRate)
{
    emit scopeSamplesReady(monoFloat32Pcm, sampleRate, /*tx=*/false);
}

void AudioEngine::publishRxPostChainScope(const QByteArray& monoFloat32Pcm, int sampleRate)
{
    emit rxPostChainScopeReady(monoFloat32Pcm, sampleRate);
}

void AudioEngine::save(const char* stage) const
{
    if (m_hooks.saveStage) m_hooks.saveStage(QString::fromLatin1(stage));
}

void AudioEngine::saveClientGateRxSettings() const { save("gate"); }
void AudioEngine::saveClientGateSettings() const { save("gate"); }
void AudioEngine::saveClientEqSettings() const { save("eq"); }
void AudioEngine::saveClientCompRxSettings() const { save("comp"); }
void AudioEngine::saveClientCompSettings() const { save("comp"); }
void AudioEngine::saveClientTubeRxSettings() const { save("tube"); }
void AudioEngine::saveClientTubeSettings() const { save("tube"); }
void AudioEngine::saveClientPuduRxSettings() const { save("pudu"); }
void AudioEngine::saveClientPuduSettings() const { save("pudu"); }

bool AudioEngine::isMuted() const
{
    return m_hooks.muted ? m_hooks.muted() : false;
}

void AudioEngine::setMuted(bool muted)
{
    if (m_hooks.setMuted) m_hooks.setMuted(muted);
}

bool AudioEngine::rxBoost() const
{
    return m_hooks.boost ? m_hooks.boost() : false;
}

void AudioEngine::setRxBoost(bool on)
{
    if (m_hooks.setBoost) m_hooks.setBoost(on);
}

float AudioEngine::rxOutputTrimDb() const
{
    return m_hooks.outputDb ? m_hooks.outputDb() : 0.0f;
}

void AudioEngine::setRxOutputTrimDb(float db)
{
    if (m_hooks.setOutputDb) m_hooks.setOutputDb(db);
}

bool AudioEngine::copyRecentClientEqRxSamples(float* out, int count) const
{
    return m_hooks.copyEqSamples ? m_hooks.copyEqSamples(out, count) : false;
}

bool AudioEngine::copyRecentClientEqTxSamples(float*, int) const
{
    return false;
}

} // namespace AetherSDR
