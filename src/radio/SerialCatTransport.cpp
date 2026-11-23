#include "radio/CatTransport.h"

#include "app/TestSandbox.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSerialPort>

#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
#include <unistd.h>
#endif
#ifdef Q_OS_MACOS
#include <libproc.h>
#include <sys/proc_info.h>

#include <vector>
#endif

Q_LOGGING_CATEGORY(lcSerial, "esa.cat.serial")

namespace esa {

namespace {

// Where a port name is in the file system: /dev/cu.usbserial-X for cu.usbserial-X.
QString deviceLocation(const QString& port)
{
    const QSerialPortInfo info(port);
    if (!info.isNull() && !info.systemLocation().isEmpty()) return info.systemLocation();
    return port.startsWith(QLatin1Char('/')) ? port : QStringLiteral("/dev/") + port;
}

class SerialCatTransport final : public CatTransport {
public:
    explicit SerialCatTransport(QObject* parent) : CatTransport(parent), m_port(this)
    {
        connect(&m_port, &QSerialPort::readyRead, this, &CatTransport::readyRead);
        connect(&m_port, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
            if (error == QSerialPort::NoError || !m_port.isOpen()) return;
            // Unplugged, or the radio switched off: what is left of the port is no use.
            if (error == QSerialPort::ResourceError || error == QSerialPort::PermissionError) {
                const QString why = m_port.errorString();
                m_port.close();
                emit lost(why);
            }
        });
    }

    bool open(const QString& port, int baud, QString* error) override
    {
        if (const QString refusal = realSerialRefusal(); !refusal.isEmpty()) {
            qCWarning(lcSerial) << "Test sandbox: refused to open a serial port" << port;
            if (error) *error = refusal;
            return false;
        }
        close();
        // Never a port another program has open: sharing would take its answers, and setting a
        // baud rate or RTS here would change them under it. Asked again at each retry, so the
        // port is taken once that program lets go.
        if (const QStringList others = otherProgramsUsing(deviceLocation(port)); !others.isEmpty()) {
            if (error) {
                *error = QStringLiteral("%1 is in use by %2. A serial port cannot be shared: quit %2 or turn its "
                                        "CAT off, and this app connects by itself.")
                             .arg(port, others.join(QStringLiteral(", ")));
            }
            return false;
        }
        m_port.setPortName(port);
        m_port.setBaudRate(baud);
        m_port.setDataBits(QSerialPort::Data8);
        m_port.setParity(QSerialPort::NoParity);
        m_port.setStopBits(QSerialPort::OneStop);
        m_port.setFlowControl(QSerialPort::NoFlowControl);
        if (!m_port.open(QIODevice::ReadWrite)) {
            if (error) {
                // Windows opens a port once: another program holding it is refused here.
                *error = m_port.error() == QSerialPort::PermissionError
                             ? QStringLiteral("%1 could not be opened: another program may be using it (%2)")
                                   .arg(port, m_port.errorString())
                             : QStringLiteral("%1: %2").arg(port, m_port.errorString());
            }
            return false;
        }
        // Before anything is sent. DTR stays low: nothing here needs it, and some radios key
        // on it. RTS goes high: Kenwood's CAT handshakes with RTS and CTS (the TS-990S's PC
        // Control Command Reference), and the radio answers nothing while the computer's RTS
        // says it cannot receive. The TS-990S has no setting that keys it from USB RTS or DTR.
        m_port.setDataTerminalReady(false);
        m_port.setRequestToSend(true);
        m_port.clear();
        qCInfo(lcSerial) << "CAT port open:" << port << baud << "baud";
        return true;
    }

    void close() override
    {
        if (!m_port.isOpen()) return;
        // A level restore written as the session ends must reach the radio before the port goes.
        if (m_port.bytesToWrite() > 0) m_port.waitForBytesWritten(200);
        m_port.close();
    }

    bool isOpen() const override { return m_port.isOpen(); }

    void write(const QByteArray& bytes) override
    {
        if (m_port.isOpen()) m_port.write(bytes);
    }

    QByteArray readAll() override { return m_port.readAll(); }

private:
    QSerialPort m_port;
};

} // namespace

QStringList otherProgramsUsing(const QString& devicePath)
{
    QStringList names;
#ifdef Q_OS_MACOS
    // The kernel reports resolved paths (/private/var for /var). A port's cu. and tty. nodes are
    // one port.
    const QString canonical = QFileInfo(devicePath).canonicalFilePath();
    const QString path = canonical.isEmpty() ? devicePath : canonical;
    QList<QByteArray> paths{QFile::encodeName(path)};
    if (path.startsWith(QLatin1String("/dev/cu."))) paths << QFile::encodeName(QStringLiteral("/dev/tty.") + path.mid(8));
    if (path.startsWith(QLatin1String("/dev/tty."))) paths << QFile::encodeName(QStringLiteral("/dev/cu.") + path.mid(9));
    const int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytes <= 0) return names;
    std::vector<pid_t> pids(size_t(bytes) / sizeof(pid_t) + 64);
    const int listed = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), int(pids.size() * sizeof(pid_t)));
    const pid_t self = getpid();
    for (int i = 0; i < listed / int(sizeof(pid_t)); ++i) {
        const pid_t pid = pids[size_t(i)];
        if (pid <= 0 || pid == self) continue;
        const int size = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
        if (size <= 0) continue;  // gone, or not this user's to look at
        std::vector<proc_fdinfo> fds(size_t(size) / sizeof(proc_fdinfo) + 16);
        const int got = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(), int(fds.size() * sizeof(proc_fdinfo)));
        for (int j = 0; j < got / int(sizeof(proc_fdinfo)); ++j) {
            if (fds[size_t(j)].proc_fdtype != PROX_FDTYPE_VNODE) continue;
            vnode_fdinfowithpath info{};
            if (proc_pidfdinfo(pid, fds[size_t(j)].proc_fd, PROC_PIDFDVNODEPATHINFO, &info, sizeof(info))
                != int(sizeof(info))) {
                continue;
            }
            if (!paths.contains(QByteArray(info.pvip.vip_path))) continue;
            char name[2 * MAXCOMLEN + 1] = {};
            proc_name(pid, name, sizeof(name));
            const QString who = QString::fromUtf8(name).isEmpty() ? QStringLiteral("process %1").arg(pid)
                                                                   : QString::fromUtf8(name);
            if (!names.contains(who)) names << who;
            break;
        }
    }
#elif defined(Q_OS_LINUX)
    const QString target = QFileInfo(devicePath).canonicalFilePath();
    if (target.isEmpty()) return names;
    const QString self = QString::number(getpid());
    const QStringList pids = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& pid : pids) {
        bool number = false;
        pid.toInt(&number);
        if (!number || pid == self) continue;
        const QDir fdDir(QStringLiteral("/proc/%1/fd").arg(pid));
        const QStringList fds = fdDir.entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot);
        for (const QString& fd : fds) {
            if (QFile::symLinkTarget(fdDir.filePath(fd)) != target) continue;
            QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
            QString who = comm.open(QIODevice::ReadOnly) ? QString::fromUtf8(comm.readAll()).trimmed() : QString();
            if (who.isEmpty()) who = QStringLiteral("process %1").arg(pid);
            if (!names.contains(who)) names << who;
            break;
        }
    }
#else
    Q_UNUSED(devicePath);
#endif
    return names;
}

QString realSerialRefusal()
{
    if (!testSandbox()) return QString();
    return QStringLiteral("Real serial ports are off in tests (ESA_TEST_SANDBOX): use a mock radio");
}

QList<QSerialPortInfo> listSerialPorts()
{
    QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
#ifdef Q_OS_MACOS
    ports.removeIf([](const QSerialPortInfo& port) { return port.portName().startsWith(QLatin1String("tty.")); });
#endif
    return ports;
}

std::unique_ptr<CatTransport> makeSerialTransport(QObject* parent)
{
    return std::make_unique<SerialCatTransport>(parent);
}

} // namespace esa
