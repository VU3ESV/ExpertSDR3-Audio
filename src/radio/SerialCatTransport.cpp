#include "radio/CatTransport.h"

#include "app/TestSandbox.h"

#include <QLoggingCategory>
#include <QSerialPort>

Q_LOGGING_CATEGORY(lcSerial, "esa.cat.serial")

namespace esa {

namespace {

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
        m_port.setPortName(port);
        m_port.setBaudRate(baud);
        m_port.setDataBits(QSerialPort::Data8);
        m_port.setParity(QSerialPort::NoParity);
        m_port.setStopBits(QSerialPort::OneStop);
        m_port.setFlowControl(QSerialPort::NoFlowControl);
        if (!m_port.open(QIODevice::ReadWrite)) {
            if (error) *error = QStringLiteral("%1: %2").arg(port, m_port.errorString());
            return false;
        }
        // Before anything is sent: a radio can map DTR or RTS to transmit.
        m_port.setDataTerminalReady(false);
        m_port.setRequestToSend(false);
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
