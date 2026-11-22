#pragma once

#include "radio/RadioBackend.h"
#include "tci/TciClient.h"

#include <QByteArray>

#include <vector>

namespace esa {

// A radio reached over TCI: ExpertSDR3 today. Audio, state and the mute switch all share one
// WebSocket, and the stream stopping is what shows the radio has gone.
class TciBackend : public RadioBackend {
    Q_OBJECT

public:
    explicit TciBackend(QObject* parent = nullptr);

    QString kind() const override { return QStringLiteral("tci"); }
    Capabilities capabilities() const override;

    void open(const AppConfig& config) override;
    void flush() override { m_client->flush(); }
    void close() override { m_client->close(); }
    void requestAudio(int receiver, int chainRate) override;
    void stopAudio() override { m_client->stopAudio(); }
    void sendMute(bool muted) override { m_client->sendMute(muted); }

    Link state() const override { return m_client->state(); }
    const RadioState& radio() const override { return m_client->radio(); }
    QString lastError() const override { return m_client->lastError(); }
    int connectedPort() const override { return m_client->connectedPort(); }

private:
    void onClientState(TciClient::State state);
    void onAudioFrame(const tci::StreamHeader& header, const QByteArray& frame);

    TciClient* m_client;
    int m_receiver{0};
    std::vector<float> m_decoded;
    bool m_counterValid{false};
    quint32 m_lastCounter{0};
};

} // namespace esa
