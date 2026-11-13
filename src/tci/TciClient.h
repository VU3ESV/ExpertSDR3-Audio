#pragma once

#include "tci/TciProtocol.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QWebSocket>

namespace esa {

// What the server has told us. Later commands overwrite earlier ones, and the
// server pushes every change, so this is always current without polling.
struct RadioState {
    QString serverName;     // "ExpertSDR3"
    QString serverVersion;  // "2.1"
    QString device;         // "ColibriDDC"
    bool receiveOnly{false};
    int trxCount{0};
    struct Receiver {
        qint64 frequencyHz{0};  // VFO A
        QString modulation;
        bool transmitting{false};
        bool enabled{true};
    };
    QHash<int, Receiver> receivers;
};

// The TCI connection: WebSocket lifecycle, the init handshake, reconnects and the
// RX audio request. Lives on the engine thread. Knows nothing about DSP or muting.
class TciClient : public QObject {
    Q_OBJECT

public:
    enum class State { Disconnected, Connecting, Initializing, Ready };
    Q_ENUM(State)

    struct AudioRequest {
        int receiver{0};
        int sampleRate{48000};
        // Real samples per block. 1024 in stereo is 512 frames, 10.7 ms at 48 kHz.
        // The spec's minimum for 48 kHz is 512 "samples", and warns against blocks
        // under 10 ms, but ExpertSDR3 counts this value in real samples: 512 in
        // stereo is only 5.3 ms.
        int samplesPerBlock{1024};
    };

    explicit TciClient(QObject* parent = nullptr);

    // Connect and keep trying until close(). While nothing has ever answered on
    // `port`, `fallbackPorts` are tried in turn, so a first run needs no configuring.
    void open(const QString& host, int port, const QList<int>& fallbackPorts = {});
    void close();

    // Applied once connected, and re-applied on every reconnect and DSP restart.
    void requestAudio(const AudioRequest& request);
    void stopAudio();

    void sendMute(bool muted);
    // Queue what is pending and push it to the socket before closing.
    void flush();

    State state() const { return m_state; }
    const RadioState& radio() const { return m_radio; }
    int connectedPort() const { return m_port; }
    QString lastError() const { return m_lastError; }

signals:
    void stateChanged(esa::TciClient::State state);
    void muteReported(bool muted);
    void modulationChanged(int receiver, const QString& modulation);
    void transmitChanged(int receiver, bool transmitting);
    void radioChanged();
    // Delivered directly on this thread. `frame` still includes the 64-byte header.
    void audioBlock(const esa::tci::StreamHeader& header, const QByteArray& frame);
    void malformedBlock();

private slots:
    void onConnected();
    void onDisconnected();
    void onErrorOccurred(QAbstractSocket::SocketError error);
    void onTextMessage(const QString& message);
    void onBinaryMessage(const QByteArray& message);
    void attemptConnection();
    void onHandshakeTimeout();

private:
    void setState(State state);
    void handleCommand(const tci::Command& command);
    void applyAudioRequest();
    void scheduleRetry();
    void send(const QString& command);

    QWebSocket m_socket;
    QTimer m_retryTimer;
    QTimer m_handshakeTimer;
    State m_state{State::Disconnected};
    RadioState m_radio;
    QString m_host;
    int m_port{0};
    QList<int> m_portCandidates;
    int m_portIndex{0};
    bool m_wantOpen{false};
    bool m_audioRequested{false};
    AudioRequest m_audio;
    int m_activeAudioReceiver{-1};
    QString m_lastError;
    quint64 m_attempt{0};  // counts connection attempts, so a refusal posted for one ends only that one
};

} // namespace esa
