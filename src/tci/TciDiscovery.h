#pragma once

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace esa {

struct DiscoveredRadio {
    QString host;
    int port{0};
    QString serverName;     // "ExpertSDR3"
    QString serverVersion;  // "2.1"
    QString device;         // "ColibriDDC"
    bool receiveOnly{false};
    int trxCount{0};

    bool isThisComputer() const;
    // "ExpertSDR3 2.1 · ColibriDDC"
    QString title() const;
    // "192.168.1.20:40001", or "This computer (port 40001)"
    QString location() const;
};

namespace discovery {

struct Network {
    QHostAddress address;
    int prefixLength{0};
};

// The IPv4 networks of this machine's active adapters, without loopback and VPN links.
QList<Network> localNetworks();

// Who to try, this computer first. Deliberately modest, because this knocks on doors:
//  - Only private addresses (10/8, 172.16/12, 192.168/16), so a public or carrier-assigned
//    address can never turn into a scan of someone else's network.
//  - At most the /24 around each address, however large the network really is.
//  - Never this machine's own addresses: 127.0.0.1 already covers them.
QStringList hostsToScan(const QList<Network>& networks);

// The ports ExpertSDR3 installs commonly use.
QList<int> commonPorts();

} // namespace discovery

// Finds TCI servers. TCI has no discovery mechanism and ExpertSDR3 advertises nothing, so this
// tries each host and port in turn, and accepts one only when it greets us as a TCI server does.
//
// Strictly read-only: a TCI server reports its state to anyone who connects, so a probe only
// listens to that greeting and leaves. It sends no commands at all, so it cannot change anything
// on a radio that is in use.
class TciDiscovery : public QObject {
    Q_OBJECT

public:
    struct Options {
        QStringList hosts;
        QList<int> ports;
        int connectTimeoutMs{800};
        int identifyTimeoutMs{2500};
        int maxConcurrent{64};  // sockets at once; macOS allows a process only 256 by default
    };

    // Everything on this machine's networks, plus `extraPort` if it is an unusual one.
    static Options localNetworkOptions(int extraPort = 0);

    explicit TciDiscovery(QObject* parent = nullptr);
    ~TciDiscovery() override;

    void start(const Options& options);
    void cancel();

    bool isRunning() const { return m_running; }
    QList<DiscoveredRadio> radios() const { return m_radios; }

signals:
    void radioFound(const esa::DiscoveredRadio& radio);
    void progressChanged(int done, int total);
    void finished();

private:
    class Probe;
    struct Target {
        QString host;
        int port;
    };

    void launchMore();
    void onProbeDone(Probe* probe, bool found);

    Options m_options;
    QList<Target> m_targets;
    qsizetype m_next{0};
    QList<Probe*> m_active;
    QList<DiscoveredRadio> m_radios;
    int m_done{0};
    bool m_running{false};
};

} // namespace esa
