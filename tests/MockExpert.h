#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>

class QTcpServer;

// A stand-in for ExpertSDR3's TCI server, modelled on behaviour measured from the
// real one (ExpertSDR3 2.1, ColibriDDC): the same init burst in the same order,
// state echoes on a 30 ms tick (including for sets that change nothing), the 200 ms
// monopoly window dropping sets, per-client AUDIO_* settings, a stream that ignores
// MUTE, and VOLUME clearing MUTE, which the spec does not mention.
class MockExpert : public QObject {
    Q_OBJECT

public:
    explicit MockExpert(QObject* parent = nullptr);

    bool listen();
    int port() const { return m_server.serverPort(); }

    // Radio state, settable before a client connects or at any time after.
    bool muted{false};
    QString modulation{QStringLiteral("LSB")};
    int blockSamples{1024};
    bool sendAudio{true};
    // Simulates spec 4.7's monopoly window: the next N mute sets are dropped, and the
    // reply reports the unchanged value.
    int dropMuteSets{0};

    // Everything clients have sent, in order, lower-cased.
    QStringList commands;
    int audioStartCount() const;
    int muteCommandsSent(bool value) const;
    bool hasClient() const { return !m_client.isNull(); }
    qint64 blocksSent{0};

    // The operator acts at the radio: state changes and is pushed to clients.
    void operatorSetMute(bool value);
    void operatorSetVolume(int db);
    void operatorSetModulation(const QString& mode);
    void operatorSetTransmit(bool transmitting);
    void operatorRestartDsp();
    // The stream changes rate on its own, whatever the client asked for: a server that ignores
    // the request. The block period stays as it was.
    void operatorSetStreamRate(int hz);

    void sendBinary(const QByteArray& frame);
    void dropClient();
    void stopListening();

private slots:
    void onNewConnection();
    void onText(const QString& text);
    void onTick();
    void onStream();

private:
    void sendInitBurst();
    void push(const QString& text);

    QPointer<QWebSocket> m_client;
    QTimer m_tick;
    QTimer m_stream;
    QStringList m_pending;
    int m_sampleRate{48000};
    int m_receiver{-1};
    quint32 m_counter{0};
    qint64 m_phase{0};
    // Last, so it goes first: it deletes any socket still open, whose disconnected handler
    // (onNewConnection) still uses the timers above. macOS's Qt 6.4.2 emits there and crashed.
    QWebSocketServer m_server;
};

// A port on this machine that nothing listens on, for a radio that is not there; 0 if none
// could be found. Never one of ExpertSDR3's own ports (40001, 50001, 60001): the system can
// hand those out as free here while ExpertSDR3 listens on every address, and a test pointed
// at one would then reach the real radio.
int closedLocalPort();

// Listens on this machine, on a port the system picks that is not one of ExpertSDR3's: a test
// pointed there could reach the real radio once the server is gone, and the test sandbox
// refuses those ports on any host anyway, so a client would never reach this server. Every
// server a test listens with goes through one of these. False if none could be had.
bool listenOffExpertPorts(QTcpServer& server);
bool listenOffExpertPorts(QWebSocketServer& server);
