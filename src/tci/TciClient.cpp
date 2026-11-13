#include "tci/TciClient.h"

#include "app/TestSandbox.h"

#include <QLoggingCategory>
#include <QUrl>

Q_LOGGING_CATEGORY(lcTci, "esa.tci")

namespace esa {

namespace {
constexpr int kRetryMs = 2000;
constexpr int kNextPortMs = 150;
// The init burst arrives in well under a second on a healthy server. Anything that
// accepts a WebSocket but never says `ready;` is not usable as a TCI server.
constexpr int kHandshakeTimeoutMs = 5000;
}

TciClient::TciClient(QObject* parent)
    : QObject(parent),
      // Parented so moveToThread() carries them along with this object.
      m_socket(QString(), QWebSocketProtocol::VersionLatest, this),
      m_retryTimer(this),
      m_handshakeTimer(this)
{
    m_retryTimer.setSingleShot(true);
    m_handshakeTimer.setSingleShot(true);
    m_handshakeTimer.setInterval(kHandshakeTimeoutMs);
    connect(&m_retryTimer, &QTimer::timeout, this, &TciClient::attemptConnection);
    connect(&m_handshakeTimer, &QTimer::timeout, this, &TciClient::onHandshakeTimeout);
    connect(&m_socket, &QWebSocket::connected, this, &TciClient::onConnected);
    connect(&m_socket, &QWebSocket::disconnected, this, &TciClient::onDisconnected);
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &TciClient::onTextMessage);
    connect(&m_socket, &QWebSocket::binaryMessageReceived, this, &TciClient::onBinaryMessage);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(&m_socket, &QWebSocket::errorOccurred, this, &TciClient::onErrorOccurred);
#else
    connect(&m_socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &TciClient::onErrorOccurred);
#endif
}

void TciClient::open(const QString& host, int port, const QList<int>& fallbackPorts)
{
    close();
    m_host = host;
    m_portCandidates = {port};
    for (int candidate : fallbackPorts) {
        if (!m_portCandidates.contains(candidate)) m_portCandidates.append(candidate);
    }
    m_portIndex = 0;
    m_wantOpen = true;
    attemptConnection();
}

void TciClient::close()
{
    m_wantOpen = false;
    m_retryTimer.stop();
    m_handshakeTimer.stop();
    m_activeAudioReceiver = -1;
    if (m_socket.state() != QAbstractSocket::UnconnectedState) m_socket.close();
    setState(State::Disconnected);
}

void TciClient::setState(State state)
{
    if (state == m_state) return;
    m_state = state;
    emit stateChanged(state);
}

void TciClient::attemptConnection()
{
    if (!m_wantOpen) return;
    m_port = m_portCandidates.value(m_portIndex);
    m_radio = RadioState{};
    m_activeAudioReceiver = -1;
    setState(State::Connecting);
    const quint64 attempt = ++m_attempt;
    if (testSandbox() && isExpertSdrPort(m_port)) {
        // Fail as a refused connection does, a moment later, so the port list moves on as usual.
        m_lastError = QStringLiteral("Port %1 is ExpertSDR3's, and tests never connect to it (ESA_TEST_SANDBOX)")
                          .arg(m_port);
        qCWarning(lcTci) << "Test sandbox: refused to connect to" << m_host << m_port;
        QTimer::singleShot(0, this, [this, attempt] {
            if (attempt == m_attempt && m_state == State::Connecting) onDisconnected();
        });
        return;
    }
    QUrl url;
    url.setScheme(QStringLiteral("ws"));
    url.setHost(m_host);
    url.setPort(m_port);
    m_socket.open(url);
}

void TciClient::scheduleRetry()
{
    if (!m_wantOpen || m_retryTimer.isActive()) return;
    if (m_portIndex + 1 < m_portCandidates.size()) {
        ++m_portIndex;
        m_retryTimer.start(kNextPortMs);
    } else {
        m_portIndex = 0;
        m_retryTimer.start(kRetryMs);
    }
}

void TciClient::onConnected()
{
    setState(State::Initializing);
    m_handshakeTimer.start();
}

void TciClient::onDisconnected()
{
    m_handshakeTimer.stop();
    m_activeAudioReceiver = -1;
    if (m_wantOpen) {
        setState(State::Connecting);
        scheduleRetry();
    } else {
        setState(State::Disconnected);
    }
}

void TciClient::onErrorOccurred(QAbstractSocket::SocketError)
{
    m_lastError = m_socket.errorString();
    // A refused connect emits an error but, depending on the Qt version, may not
    // emit disconnected, so make sure a retry is scheduled either way.
    if (m_socket.state() == QAbstractSocket::UnconnectedState) onDisconnected();
}

void TciClient::onHandshakeTimeout()
{
    m_lastError = QStringLiteral("Connected, but the server never reported ready");
    m_socket.abort();
}

void TciClient::send(const QString& command)
{
    if (m_socket.state() == QAbstractSocket::ConnectedState) m_socket.sendTextMessage(command);
}

void TciClient::flush()
{
    m_socket.flush();
}

void TciClient::sendMute(bool muted)
{
    send(muted ? QStringLiteral("mute:true;") : QStringLiteral("mute:false;"));
}

void TciClient::requestAudio(const AudioRequest& request)
{
    m_audio = request;
    m_audioRequested = true;
    if (m_state == State::Ready) applyAudioRequest();
}

void TciClient::stopAudio()
{
    m_audioRequested = false;
    if (m_activeAudioReceiver >= 0) {
        send(QStringLiteral("audio_stop:%1;").arg(m_activeAudioReceiver));
        m_activeAudioReceiver = -1;
    }
}

void TciClient::applyAudioRequest()
{
    if (m_activeAudioReceiver >= 0 && m_activeAudioReceiver != m_audio.receiver) {
        send(QStringLiteral("audio_stop:%1;").arg(m_activeAudioReceiver));
    }
    // AUDIO_* are per-client ("unidirectional"), so none of this disturbs other
    // TCI clients of the same server.
    send(QStringLiteral("audio_samplerate:%1;").arg(m_audio.sampleRate));
    send(QStringLiteral("audio_stream_sample_type:float32;"));
    send(QStringLiteral("audio_stream_channels:2;"));
    send(QStringLiteral("audio_stream_samples:%1;").arg(m_audio.samplesPerBlock));
    send(QStringLiteral("audio_start:%1;").arg(m_audio.receiver));
    m_activeAudioReceiver = m_audio.receiver;
}

void TciClient::onTextMessage(const QString& message)
{
    const auto commands = tci::parseCommands(message);
    for (const tci::Command& command : commands) handleCommand(command);
}

void TciClient::handleCommand(const tci::Command& c)
{
    const QString& name = c.name;
    if (name == QLatin1String("ready")) {
        m_handshakeTimer.stop();
        m_portCandidates = {m_port};  // this port works: stay on it across reconnects
        m_portIndex = 0;
        setState(State::Ready);
        if (m_audioRequested) applyAudioRequest();
        emit radioChanged();
    } else if (name == QLatin1String("start")) {
        // Also part of the init burst, before `ready`. After it, the operator has
        // restarted the DSP, and the stream must be asked for again.
        if (m_state == State::Ready && m_audioRequested) applyAudioRequest();
    } else if (name == QLatin1String("mute") && !c.args.isEmpty()) {
        emit muteReported(tci::parseBool(c.args.at(0)));
    } else if (name == QLatin1String("protocol") && c.args.size() >= 2) {
        m_radio.serverName = c.args.at(0);
        m_radio.serverVersion = c.args.at(1);
    } else if (name == QLatin1String("device") && !c.args.isEmpty()) {
        m_radio.device = c.args.at(0);
    } else if (name == QLatin1String("receive_only") && !c.args.isEmpty()) {
        m_radio.receiveOnly = tci::parseBool(c.args.at(0));
    } else if (name == QLatin1String("trx_count") && !c.args.isEmpty()) {
        m_radio.trxCount = c.args.at(0).toInt();
        emit radioChanged();
    } else if (name == QLatin1String("modulation") && c.args.size() >= 2) {
        const int rx = c.args.at(0).toInt();
        m_radio.receivers[rx].modulation = c.args.at(1);
        emit modulationChanged(rx, c.args.at(1));
        emit radioChanged();
    } else if (name == QLatin1String("vfo") && c.args.size() >= 3) {
        if (c.args.at(1).toInt() == 0) {  // channel A
            m_radio.receivers[c.args.at(0).toInt()].frequencyHz = c.args.at(2).toLongLong();
            emit radioChanged();
        }
    } else if (name == QLatin1String("trx") && c.args.size() >= 2) {
        const int rx = c.args.at(0).toInt();
        const bool tx = tci::parseBool(c.args.at(1));
        m_radio.receivers[rx].transmitting = tx;
        emit transmitChanged(rx, tx);
    } else if (name == QLatin1String("rx_enable") && c.args.size() >= 2) {
        m_radio.receivers[c.args.at(0).toInt()].enabled = tci::parseBool(c.args.at(1));
        emit radioChanged();
    }
}

void TciClient::onBinaryMessage(const QByteArray& message)
{
    const auto header = tci::parseHeader(message);
    if (!header) {
        emit malformedBlock();
        return;
    }
    if (static_cast<tci::StreamType>(header->type) == tci::StreamType::RxAudio) {
        emit audioBlock(*header, message);
    }
}

} // namespace esa
