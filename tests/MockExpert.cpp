#include "MockExpert.h"

#include "app/TestSandbox.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

using esa::isExpertSdrPort;

namespace {

template <typename Server>
bool listenOff(Server& server)
{
    // Those turned down stay taken until one will do, so none is handed out again.
    std::vector<std::unique_ptr<QTcpServer>> skipped;
    for (int i = 0; i < 64; ++i) {
        if (!server.listen(QHostAddress::LocalHost, 0)) return false;
        const int port = server.serverPort();
        if (!isExpertSdrPort(port)) return true;
        server.close();
        auto hold = std::make_unique<QTcpServer>();
        hold->listen(QHostAddress::LocalHost, quint16(port));
        skipped.push_back(std::move(hold));
    }
    return false;
}

} // namespace

int closedLocalPort()
{
    // Those turned down stay taken until one will do, so none is handed out twice.
    std::vector<std::unique_ptr<QTcpServer>> skipped;
    for (int i = 0; i < 64; ++i) {
        auto probe = std::make_unique<QTcpServer>();
        if (!probe->listen(QHostAddress::LocalHost, 0)) return 0;
        const int port = probe->serverPort();
        if (!isExpertSdrPort(port)) return port;  // and closed, as `probe` goes
        skipped.push_back(std::move(probe));
    }
    return 0;
}

bool listenOffExpertPorts(QTcpServer& server)
{
    return listenOff(server);
}

bool listenOffExpertPorts(QWebSocketServer& server)
{
    return listenOff(server);
}

MockExpert::MockExpert(QObject* parent)
    : QObject(parent),
      m_server(QStringLiteral("MockExpert"), QWebSocketServer::NonSecureMode)
{
    connect(&m_server, &QWebSocketServer::newConnection, this, &MockExpert::onNewConnection);
    connect(&m_tick, &QTimer::timeout, this, &MockExpert::onTick);
    connect(&m_stream, &QTimer::timeout, this, &MockExpert::onStream);
    m_tick.start(30);  // spec 4.7: state goes out on a 30 ms tick
}

bool MockExpert::listen()
{
    // Never on one of ExpertSDR3's ports, as closedLocalPort() explains: an engine still
    // trying this port once the mock is gone would reach the real radio there.
    return listenOffExpertPorts(m_server);
}

void MockExpert::stopListening()
{
    m_server.close();
}

int MockExpert::audioStartCount() const
{
    return int(commands.filter(QStringLiteral("audio_start:")).size());
}

int MockExpert::muteCommandsSent(bool value) const
{
    return int(commands.count(value ? QStringLiteral("mute:true") : QStringLiteral("mute:false")));
}

void MockExpert::onNewConnection()
{
    QWebSocket* socket = m_server.nextPendingConnection();
    m_client = socket;
    m_receiver = -1;
    m_counter = 0;
    m_stream.stop();
    connect(socket, &QWebSocket::textMessageReceived, this, &MockExpert::onText);
    connect(socket, &QWebSocket::disconnected, this, [this, socket] {
        if (m_client == socket) m_stream.stop();
        socket->deleteLater();
    });
    sendInitBurst();
}

void MockExpert::sendInitBurst()
{
    // Order and spelling captured from the real server.
    const QStringList burst = {
        QStringLiteral("protocol:ExpertSDR3,2.1;"), QStringLiteral("device:ColibriDDC;"),
        QStringLiteral("receive_only:true;"), QStringLiteral("trx_count:2;"),
        QStringLiteral("channels_count:2;"), QStringLiteral("audio_samplerate:48000;"),
        QStringLiteral("volume:0;"), QStringLiteral("mute:%1;").arg(muted ? "true" : "false"),
        QStringLiteral("vfo:0,0,7160000;"), QStringLiteral("vfo:1,0,14017000;"),
        QStringLiteral("modulation:0,%1;").arg(modulation), QStringLiteral("modulation:1,CW;"),
        QStringLiteral("rx_enable:0,true;"), QStringLiteral("rx_enable:1,false;"),
        QStringLiteral("trx:0,false;"), QStringLiteral("trx:1,false;"),
        QStringLiteral("start;"), QStringLiteral("ready;"),
    };
    for (const QString& line : burst) m_client->sendTextMessage(line);
}

void MockExpert::push(const QString& text)
{
    m_pending.append(text);
}

void MockExpert::onTick()
{
    if (m_client) {
        for (const QString& text : std::as_const(m_pending)) m_client->sendTextMessage(text);
    }
    m_pending.clear();
}

void MockExpert::onText(const QString& text)
{
    const QStringList parts = text.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& raw : parts) {
        const QString command = raw.trimmed().toLower();
        commands.append(command);
        const QString name = command.section(QLatin1Char(':'), 0, 0);
        const QString args = command.section(QLatin1Char(':'), 1);

        if (name == QLatin1String("mute") && !args.isEmpty()) {
            if (dropMuteSets > 0) {
                --dropMuteSets;  // spec 4.7: dropped, so the reply below is unchanged
            } else {
                muted = args == QLatin1String("true");
            }
            push(QStringLiteral("mute:%1;").arg(muted ? "true" : "false"));
        } else if (name == QLatin1String("volume") && !args.isEmpty()) {
            operatorSetVolume(args.toInt());  // the real server treats it the same way
        } else if (name == QLatin1String("audio_samplerate")) {
            m_sampleRate = args.toInt();
        } else if (name == QLatin1String("audio_stream_samples")) {
            blockSamples = args.toInt();
        } else if (name == QLatin1String("audio_start")) {
            m_receiver = args.toInt();
            if (sendAudio) {
                // Block period from the real samples per block, in stereo.
                const int intervalMs = std::max(1, (blockSamples / 2) * 1000 / m_sampleRate);
                m_stream.start(intervalMs);
            }
            push(QStringLiteral("audio_start:%1;").arg(m_receiver));
        } else if (name == QLatin1String("audio_stop")) {
            m_receiver = -1;
            m_stream.stop();
        }
    }
}

void MockExpert::onStream()
{
    if (!m_client || m_receiver < 0) return;
    const int samples = blockSamples;
    QByteArray frame(64 + samples * int(sizeof(float)), '\0');
    const auto put = [&](int index, quint32 value) { qToLittleEndian(value, frame.data() + index * 4); };
    put(0, quint32(m_receiver));
    put(1, quint32(m_sampleRate));
    put(2, 3);  // float32
    put(5, quint32(samples));
    put(6, 1);  // RX audio
    put(7, 2);  // channels
    put(8, m_counter++);
    auto* data = reinterpret_cast<float*>(frame.data() + 64);
    for (int i = 0; i < samples / 2; ++i) {
        const double t = double(m_phase++) / m_sampleRate;
        data[i * 2] = float(0.2 * std::sin(2.0 * 3.14159265358979 * 600.0 * t));   // I / left
        data[i * 2 + 1] = float(0.2 * std::cos(2.0 * 3.14159265358979 * 600.0 * t));  // Q / right
    }
    m_client->sendBinaryMessage(frame);
    ++blocksSent;
}

void MockExpert::sendBinary(const QByteArray& frame)
{
    if (m_client) m_client->sendBinaryMessage(frame);
}

void MockExpert::operatorSetMute(bool value)
{
    muted = value;
    push(QStringLiteral("mute:%1;").arg(value ? "true" : "false"));
}

void MockExpert::operatorSetVolume(int db)
{
    push(QStringLiteral("volume:%1;").arg(db));
    operatorSetMute(false);
}

void MockExpert::operatorSetModulation(const QString& mode)
{
    modulation = mode;
    push(QStringLiteral("modulation:0,%1;").arg(mode));
}

void MockExpert::operatorSetTransmit(bool transmitting)
{
    push(QStringLiteral("trx:0,%1;").arg(transmitting ? "true" : "false"));
}

void MockExpert::operatorRestartDsp()
{
    m_stream.stop();
    m_receiver = -1;
    push(QStringLiteral("stop;"));
    push(QStringLiteral("start;"));
}

void MockExpert::operatorSetStreamRate(int hz)
{
    m_sampleRate = hz;
}

void MockExpert::dropClient()
{
    if (m_client) m_client->abort();
}
