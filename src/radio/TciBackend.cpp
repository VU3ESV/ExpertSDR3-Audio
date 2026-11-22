#include "radio/TciBackend.h"

#include "app/Settings.h"

namespace esa {

TciBackend::TciBackend(QObject* parent)
    : RadioBackend(parent), m_client(new TciClient(this))
{
    connect(m_client, &TciClient::stateChanged, this, &TciBackend::onClientState);
    connect(m_client, &TciClient::muteReported, this, &RadioBackend::muteReported);
    connect(m_client, &TciClient::modulationChanged, this, &RadioBackend::modulationChanged);
    connect(m_client, &TciClient::transmitChanged, this, &RadioBackend::transmitChanged);
    connect(m_client, &TciClient::radioChanged, this, &RadioBackend::radioChanged);
    connect(m_client, &TciClient::audioBlock, this, &TciBackend::onAudioFrame);
    connect(m_client, &TciClient::malformedBlock, this, &RadioBackend::malformedBlock);
}

RadioBackend::Capabilities TciBackend::capabilities() const
{
    Capabilities c;
    c.speaker = SpeakerControl::Switch;
    c.audioProvesRadio = true;
    c.reportsMode = true;
    c.reportsFrequency = true;
    return c;
}

void TciBackend::open(const AppConfig& config)
{
    m_counterValid = false;
    m_client->open(config.host, config.port, config.fallbackPorts);
}

void TciBackend::requestAudio(int receiver, int chainRate)
{
    m_receiver = receiver;
    m_counterValid = false;  // a new stream
    TciClient::AudioRequest request;
    request.receiver = receiver;
    request.sampleRate = chainRate;
    // Real samples per block, targeting about 10.7 ms in stereo at either rate.
    request.samplesPerBlock = chainRate == 24000 ? 512 : 1024;
    m_client->requestAudio(request);
}

void TciBackend::onClientState(TciClient::State state)
{
    if (state == Link::Connecting || state == Link::Disconnected) {
        m_counterValid = false;
    } else if (state == Link::Ready) {
        emit portResolved(m_client->connectedPort());
    }
    emit stateChanged(state);
}

void TciBackend::onAudioFrame(const tci::StreamHeader& header, const QByteArray& frame)
{
    if (int(header.receiver) != m_receiver) return;
    // Blocks still in flight while a closed socket shuts are the ended session's. Taken as
    // audio they would show it flowing while disconnected, and reach an output a PLAY opened.
    if (m_client->state() != Link::Ready) return;

    const int frames = tci::decodeToStereo(header, frame.constData() + tci::kHeaderBytes,
                                           frame.size() - tci::kHeaderBytes, m_decoded);
    if (frames < 0) {
        emit malformedBlock();
        return;
    }
    if (frames == 0) return;
    // The server could ignore our rate request. The engine follows what it actually sends if
    // the chain supports that rate; anything else is dropped rather than played at the wrong
    // speed.
    if (header.sampleRate != 24000 && header.sampleRate != 48000) {
        emit malformedBlock();
        return;
    }

    // The packet counter restarts on TX/RX switches, so a backwards jump is not a
    // loss; only a forward gap is.
    if (m_counterValid && header.count > m_lastCounter + 1) emit blocksLost(header.count - m_lastCounter - 1);
    m_lastCounter = header.count;
    m_counterValid = true;

    emit audioBlock(int(header.receiver), m_decoded.data(), frames, int(header.sampleRate));
}

} // namespace esa
