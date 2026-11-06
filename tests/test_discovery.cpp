#include "MockExpert.h"

#include "tci/TciDiscovery.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTest>
#include <QWebSocketServer>
#include <QtTest>

#include <utility>

using namespace esa;
using discovery::Network;

namespace {

constexpr int kTimeout = 6000;

Network network(const char* address, int prefix)
{
    return {QHostAddress(QString::fromLatin1(address)), prefix};
}

// Never the real network: everything here targets explicit addresses on this machine.
TciDiscovery::Options onThisMachine(QList<int> ports)
{
    TciDiscovery::Options options;
    options.hosts = {QStringLiteral("127.0.0.1")};
    options.ports = std::move(ports);
    options.connectTimeoutMs = 500;
    options.identifyTimeoutMs = 700;
    return options;
}

int closedPort()
{
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost, 0);
    return probe.serverPort();  // nothing listens here once it goes out of scope
}

} // namespace

class TestDiscovery : public QObject {
    Q_OBJECT

private slots:
    void thisComputerIsAlwaysCheckedFirst()
    {
        const QStringList hosts = discovery::hostsToScan({network("192.168.1.10", 24)});
        QCOMPARE(hosts.first(), QStringLiteral("127.0.0.1"));
        QVERIFY(discovery::hostsToScan({}).contains(QStringLiteral("127.0.0.1")));
    }

    void aSlash24IsEveryOtherHostOnIt()
    {
        const QStringList hosts = discovery::hostsToScan({network("192.168.1.10", 24)});
        QCOMPARE(hosts.size(), 254);  // 253 others, plus this computer
        QVERIFY(hosts.contains(QStringLiteral("192.168.1.1")));
        QVERIFY(hosts.contains(QStringLiteral("192.168.1.254")));
        QVERIFY(!hosts.contains(QStringLiteral("192.168.1.0")));    // network address
        QVERIFY(!hosts.contains(QStringLiteral("192.168.1.255")));  // broadcast
        QVERIFY(!hosts.contains(QStringLiteral("192.168.1.10")));   // ourselves
    }

    // A /8 is 16 million addresses. Nobody asked for that.
    void aLargeNetworkIsLimitedToTheSlash24AroundUs()
    {
        for (int prefix : {8, 16, 22}) {
            const QStringList hosts = discovery::hostsToScan({network("10.20.30.40", prefix)});
            QCOMPARE(hosts.size(), 254);
            QVERIFY(hosts.contains(QStringLiteral("10.20.30.1")));
            QVERIFY(!hosts.contains(QStringLiteral("10.20.31.1")));
        }
    }

    void aSmallNetworkIsScannedAsItIs()
    {
        const QStringList hosts = discovery::hostsToScan({network("192.168.1.10", 30)});
        QCOMPARE(hosts, (QStringList{QStringLiteral("127.0.0.1"), QStringLiteral("192.168.1.9")}));
    }

    void pointToPointLinksHaveNobodyToFind()
    {
        for (int prefix : {31, 32}) {
            QCOMPARE(discovery::hostsToScan({network("192.168.1.10", prefix)}).size(), 1);
        }
    }

    // Whatever address a machine ends up with, this must never scan someone else's network.
    void publicAddressesAreNeverScanned_data()
    {
        QTest::addColumn<QString>("address");
        QTest::newRow("public") << QStringLiteral("8.8.8.8");
        QTest::newRow("carrier-grade NAT, as Tailscale uses") << QStringLiteral("100.64.1.2");
        QTest::newRow("link-local") << QStringLiteral("169.254.1.2");
        QTest::newRow("just below 172.16/12") << QStringLiteral("172.15.0.1");
        QTest::newRow("just above 172.16/12") << QStringLiteral("172.32.0.1");
    }

    void publicAddressesAreNeverScanned()
    {
        QFETCH(QString, address);
        QCOMPARE(discovery::hostsToScan({{QHostAddress(address), 24}}).size(), 1);
    }

    void privateRangesAreAllRecognised()
    {
        for (const char* address : {"10.0.0.1", "172.16.0.1", "172.31.255.1", "192.168.0.1"}) {
            QVERIFY2(discovery::hostsToScan({network(address, 24)}).size() > 1, address);
        }
    }

    // Two adapters on one network, as on this very Mac.
    void twoAdaptersOnOneNetworkScanItOnce()
    {
        const QStringList hosts = discovery::hostsToScan({network("192.168.86.10", 24), network("192.168.86.11", 24)});
        QCOMPARE(hosts.size(), 253);
        QCOMPARE(hosts.count(QStringLiteral("192.168.86.1")), 1);
        QVERIFY(!hosts.contains(QStringLiteral("192.168.86.10")));
        QVERIFY(!hosts.contains(QStringLiteral("192.168.86.11")));
    }

    void findsARadioAndReadsItsIdentity()
    {
        MockExpert radio;
        QVERIFY(radio.listen());
        TciDiscovery scan;
        QSignalSpy done(&scan, &TciDiscovery::finished);
        scan.start(onThisMachine({radio.port()}));
        QVERIFY(done.wait(kTimeout));

        QCOMPARE(scan.radios().size(), 1);
        const DiscoveredRadio found = scan.radios().first();
        QCOMPARE(found.host, QStringLiteral("127.0.0.1"));
        QCOMPARE(found.port, radio.port());
        QCOMPARE(found.serverName, QStringLiteral("ExpertSDR3"));
        QCOMPARE(found.serverVersion, QStringLiteral("2.1"));
        QCOMPARE(found.device, QStringLiteral("ColibriDDC"));
        QVERIFY(found.receiveOnly);
        QCOMPARE(found.trxCount, 2);
        QVERIFY(found.isThisComputer());
        QCOMPARE(found.title(), QStringLiteral("ExpertSDR3 2.1 · ColibriDDC"));
    }

    // The promise that makes scanning safe on a radio somebody is using.
    void aProbeSendsTheRadioNothingAtAll()
    {
        MockExpert radio;
        radio.muted = true;
        QVERIFY(radio.listen());
        TciDiscovery scan;
        QSignalSpy done(&scan, &TciDiscovery::finished);
        scan.start(onThisMachine({radio.port()}));
        QVERIFY(done.wait(kTimeout));
        QCOMPARE(scan.radios().size(), 1);
        QTest::qWait(200);  // anything sent would have arrived by now

        QVERIFY2(radio.commands.isEmpty(), qPrintable(radio.commands.join(QLatin1Char(' '))));
        QVERIFY(radio.muted);
        QCOMPARE(radio.audioStartCount(), 0);
    }

    void aProbeLeavesTheRadioFreeForARealConnection()
    {
        MockExpert radio;
        QVERIFY(radio.listen());
        TciDiscovery scan;
        QSignalSpy done(&scan, &TciDiscovery::finished);
        scan.start(onThisMachine({radio.port()}));
        QVERIFY(done.wait(kTimeout));
        QTRY_VERIFY_WITH_TIMEOUT(!radio.hasClient(), kTimeout);
    }

    void aClosedPortIsNotARadio()
    {
        TciDiscovery scan;
        QSignalSpy done(&scan, &TciDiscovery::finished);
        scan.start(onThisMachine({closedPort()}));
        QVERIFY(done.wait(kTimeout));
        QVERIFY(scan.radios().isEmpty());
    }

    void somethingElseListeningIsNotARadio()
    {
        QTcpServer notWebSocket;  // accepts TCP and says nothing, as many services do
        QVERIFY(notWebSocket.listen(QHostAddress::LocalHost, 0));
        TciDiscovery scan;
        QSignalSpy done(&scan, &TciDiscovery::finished);
        scan.start(onThisMachine({notWebSocket.serverPort()}));
        QVERIFY(done.wait(kTimeout));
        QVERIFY(scan.radios().isEmpty());
    }

    void aWebSocketThatIsNotTciIsNotARadio()
    {
        QWebSocketServer other(QStringLiteral("other"), QWebSocketServer::NonSecureMode);
        QVERIFY(other.listen(QHostAddress::LocalHost, 0));
        connect(&other, &QWebSocketServer::newConnection, this, [&other] {
            // A `ready;` with no `protocol:` before it is not enough either.
            other.nextPendingConnection()->sendTextMessage(QStringLiteral("hello;ready;"));
        });
        TciDiscovery scan;
        QSignalSpy done(&scan, &TciDiscovery::finished);
        scan.start(onThisMachine({other.serverPort()}));
        QVERIFY(done.wait(kTimeout));
        QVERIFY(scan.radios().isEmpty());
    }

    void findsSeveralRadiosAmongOtherPorts()
    {
        MockExpert first;
        MockExpert second;
        QVERIFY(first.listen());
        QVERIFY(second.listen());
        TciDiscovery scan;
        QSignalSpy done(&scan, &TciDiscovery::finished);
        scan.start(onThisMachine({closedPort(), first.port(), closedPort(), second.port()}));
        QVERIFY(done.wait(kTimeout));
        QCOMPARE(scan.radios().size(), 2);
    }

    void aHostThatDoesNotAnswerDoesNotStallTheScan()
    {
        MockExpert radio;
        QVERIFY(radio.listen());
        TciDiscovery::Options options = onThisMachine({radio.port()});
        // TEST-NET-1: reserved for documentation, so nothing is ever there to disturb.
        options.hosts.prepend(QStringLiteral("192.0.2.1"));
        options.connectTimeoutMs = 400;
        TciDiscovery scan;
        QSignalSpy done(&scan, &TciDiscovery::finished);
        scan.start(options);
        QVERIFY(done.wait(kTimeout));
        QCOMPARE(scan.radios().size(), 1);
    }

    void nothingToScanFinishesAtOnce()
    {
        TciDiscovery scan;
        QSignalSpy done(&scan, &TciDiscovery::finished);
        scan.start({});
        QCOMPARE(done.size(), 1);
        QVERIFY(!scan.isRunning());
    }

    void progressCountsEveryTarget()
    {
        TciDiscovery::Options options = onThisMachine({closedPort(), closedPort(), closedPort()});
        TciDiscovery scan;
        QSignalSpy done(&scan, &TciDiscovery::finished);
        QSignalSpy progress(&scan, &TciDiscovery::progressChanged);
        scan.start(options);
        QVERIFY(done.wait(kTimeout));
        const auto last = progress.last();
        QCOMPARE(last.at(0).toInt(), 3);
        QCOMPARE(last.at(1).toInt(), 3);
    }

    void neverRunsMoreThanTheLimitAtOnce()
    {
        // Many listeners answering at once would exhaust a process's sockets on macOS otherwise.
        QList<QTcpServer*> servers;
        QList<int> ports;
        for (int i = 0; i < 12; ++i) {
            auto* server = new QTcpServer(this);
            QVERIFY(server->listen(QHostAddress::LocalHost, 0));
            servers.append(server);
            ports.append(server->serverPort());
        }
        TciDiscovery::Options options = onThisMachine(ports);
        options.maxConcurrent = 3;
        options.identifyTimeoutMs = 150;
        TciDiscovery scan;
        QSignalSpy done(&scan, &TciDiscovery::finished);
        QElapsedTimer timer;
        timer.start();
        scan.start(options);
        QVERIFY(done.wait(kTimeout));
        // 12 at 3 at a time, each held for its 150 ms identify timeout, cannot be under 4 rounds.
        QVERIFY2(timer.elapsed() >= 550, qPrintable(QString::number(timer.elapsed())));
        qDeleteAll(servers);
    }

    void cancellingStopsTheScan()
    {
        TciDiscovery::Options options;
        options.hosts = {QStringLiteral("192.0.2.1"), QStringLiteral("192.0.2.2")};
        options.ports = {40001};
        TciDiscovery scan;
        QSignalSpy done(&scan, &TciDiscovery::finished);
        scan.start(options);
        QVERIFY(scan.isRunning());
        scan.cancel();
        QVERIFY(!scan.isRunning());
        QTest::qWait(100);
        QCOMPARE(done.size(), 0);
    }

    void aScanCanBeRepeated()
    {
        MockExpert radio;
        QVERIFY(radio.listen());
        TciDiscovery scan;
        QSignalSpy done(&scan, &TciDiscovery::finished);
        for (int i = 0; i < 2; ++i) {
            scan.start(onThisMachine({radio.port()}));
            QVERIFY(done.wait(kTimeout));
            QCOMPARE(scan.radios().size(), 1);  // not 2: the second replaces the first
        }
    }

    void describesWhereARadioIs()
    {
        DiscoveredRadio local;
        local.host = QStringLiteral("127.0.0.1");
        local.port = 60001;
        QCOMPARE(local.location(), QStringLiteral("This computer (port 60001)"));
        DiscoveredRadio remote;
        remote.host = QStringLiteral("192.168.1.20");
        remote.port = 40001;
        QCOMPARE(remote.location(), QStringLiteral("192.168.1.20:40001"));
        QVERIFY(!remote.isThisComputer());
    }
};

QTEST_GUILESS_MAIN(TestDiscovery)
#include "test_discovery.moc"
