#include "tci/TciDiscovery.h"

#include "tci/TciProtocol.h"

#include <QNetworkInterface>
#include <QSet>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <algorithm>
#include <functional>
#include <utility>

namespace esa {

bool DiscoveredRadio::isThisComputer() const
{
    return host == QLatin1String("127.0.0.1") || host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0;
}

QString DiscoveredRadio::title() const
{
    QString text = serverName;
    if (!serverVersion.isEmpty()) text += QLatin1Char(' ') + serverVersion;
    if (!device.isEmpty()) text += QStringLiteral(" · ") + device;
    return text;
}

QString DiscoveredRadio::location() const
{
    return isThisComputer() ? QStringLiteral("This computer (port %1)").arg(port)
                            : QStringLiteral("%1:%2").arg(host).arg(port);
}

namespace discovery {

namespace {

bool isPrivate(quint32 address)
{
    return (address >> 24) == 10            // 10.0.0.0/8
           || (address >> 20) == 0xAC1      // 172.16.0.0/12
           || (address >> 16) == 0xC0A8;    // 192.168.0.0/16
}

} // namespace

QList<Network> localNetworks()
{
    QList<Network> networks;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack) || flags.testFlag(QNetworkInterface::IsPointToPoint)) {
            continue;
        }
        const auto entries = iface.addressEntries();
        for (const QNetworkAddressEntry& entry : entries) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                networks.append({entry.ip(), entry.prefixLength()});
            }
        }
    }
    return networks;
}

QStringList hostsToScan(const QList<Network>& networks)
{
    QSet<quint32> ownAddresses;
    for (const Network& network : networks) ownAddresses.insert(network.address.toIPv4Address());

    QStringList hosts{QStringLiteral("127.0.0.1")};
    QSet<quint32> seen;
    for (const Network& network : networks) {
        const quint32 address = network.address.toIPv4Address();
        // /31 and /32 are point-to-point links with nobody to discover.
        if (!isPrivate(address) || network.prefixLength > 30 || network.prefixLength < 0) continue;

        const int prefix = std::max(network.prefixLength, 24);
        const quint32 mask = 0xFFFFFFFFu << (32 - prefix);
        const quint32 base = address & mask;
        const quint32 broadcast = base | ~mask;
        for (quint32 host = base + 1; host < broadcast; ++host) {
            if (ownAddresses.contains(host) || seen.contains(host)) continue;
            seen.insert(host);
            hosts.append(QHostAddress(host).toString());
        }
    }
    return hosts;
}

QList<int> commonPorts()
{
    return {40001, 50001, 60001};
}

} // namespace discovery

// One host and port: can we connect at all, and if so, does it greet us like a TCI server?
// A plain TCP connect comes first because almost every target answers nothing, and a WebSocket
// offers no way to give up quickly on that without also giving up on a slow but real handshake.
class TciDiscovery::Probe : public QObject {
public:
    using Done = std::function<void(Probe*, bool)>;

    Probe(QString host, int port, const Options& options, Done done, QObject* parent)
        : QObject(parent), m_options(options), m_done(std::move(done))
    {
        m_radio.host = std::move(host);
        m_radio.port = port;
    }

    void start()
    {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] { finish(false); });

        m_tcp = new QTcpSocket(this);
        connect(m_tcp, &QTcpSocket::connected, this, [this] { identify(); });
        connect(m_tcp, &QTcpSocket::errorOccurred, this, [this] { finish(false); });
        m_timer.start(m_options.connectTimeoutMs);
        m_tcp->connectToHost(m_radio.host, quint16(m_radio.port));
    }

    const DiscoveredRadio& radio() const { return m_radio; }

private:
    void identify()
    {
        m_tcp->disconnect(this);
        m_tcp->disconnectFromHost();

        m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        connect(m_ws, &QWebSocket::textMessageReceived, this, [this](const QString& text) { onText(text); });
        connect(m_ws, &QWebSocket::disconnected, this, [this] { finish(false); });
        m_timer.start(m_options.identifyTimeoutMs);

        QUrl url;
        url.setScheme(QStringLiteral("ws"));
        url.setHost(m_radio.host);
        url.setPort(m_radio.port);
        m_ws->open(url);
    }

    void onText(const QString& text)
    {
        const auto commands = tci::parseCommands(text);
        for (const tci::Command& c : commands) {
            if (c.name == QLatin1String("protocol") && c.args.size() >= 2) {
                m_radio.serverName = c.args.at(0);
                m_radio.serverVersion = c.args.at(1);
                m_sawProtocol = true;
            } else if (c.name == QLatin1String("device") && !c.args.isEmpty()) {
                m_radio.device = c.args.at(0);
            } else if (c.name == QLatin1String("receive_only") && !c.args.isEmpty()) {
                m_radio.receiveOnly = tci::parseBool(c.args.at(0));
            } else if (c.name == QLatin1String("trx_count") && !c.args.isEmpty()) {
                m_radio.trxCount = c.args.at(0).toInt();
            } else if (c.name == QLatin1String("ready")) {
                finish(m_sawProtocol);
                return;
            }
        }
    }

    void finish(bool found)
    {
        if (m_finished) return;
        m_finished = true;
        m_timer.stop();
        if (m_tcp) {
            m_tcp->disconnect(this);
            m_tcp->abort();
        }
        if (m_ws) {
            m_ws->disconnect(this);
            m_ws->close();
        }
        m_done(this, found);
    }

    Options m_options;
    Done m_done;
    DiscoveredRadio m_radio;
    QTimer m_timer;
    QTcpSocket* m_tcp{nullptr};
    QWebSocket* m_ws{nullptr};
    bool m_sawProtocol{false};
    bool m_finished{false};
};

TciDiscovery::Options TciDiscovery::localNetworkOptions(int extraPort)
{
    Options options;
    options.hosts = discovery::hostsToScan(discovery::localNetworks());
    options.ports = discovery::commonPorts();
    if (extraPort > 0 && !options.ports.contains(extraPort)) options.ports.prepend(extraPort);
    return options;
}

TciDiscovery::TciDiscovery(QObject* parent)
    : QObject(parent)
{
}

TciDiscovery::~TciDiscovery() = default;

void TciDiscovery::start(const Options& options)
{
    cancel();
    m_options = options;
    m_radios.clear();
    m_targets.clear();
    for (const QString& host : options.hosts) {
        for (int port : options.ports) m_targets.append({host, port});
    }
    m_next = 0;
    m_done = 0;
    m_running = true;
    emit progressChanged(0, int(m_targets.size()));
    if (m_targets.isEmpty()) {
        m_running = false;
        emit finished();
        return;
    }
    launchMore();
}

void TciDiscovery::cancel()
{
    for (Probe* probe : std::as_const(m_active)) {
        probe->disconnect(this);
        delete probe;
    }
    m_active.clear();
    m_targets.clear();
    m_next = 0;
    m_running = false;
}

void TciDiscovery::launchMore()
{
    while (m_active.size() < m_options.maxConcurrent && m_next < m_targets.size()) {
        const Target target = m_targets.at(m_next++);
        auto* probe = new Probe(target.host, target.port, m_options,
                                [this](Probe* p, bool found) { onProbeDone(p, found); }, this);
        m_active.append(probe);
        probe->start();
    }
}

void TciDiscovery::onProbeDone(Probe* probe, bool found)
{
    m_active.removeOne(probe);
    ++m_done;
    if (found) {
        m_radios.append(probe->radio());
        emit radioFound(probe->radio());
    }
    probe->deleteLater();
    emit progressChanged(m_done, int(m_targets.size()));

    if (!m_running) return;
    launchMore();
    if (m_active.isEmpty() && m_next >= m_targets.size()) {
        m_running = false;
        emit finished();
    }
}

} // namespace esa
