#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QSerialPortInfo>
#include <QString>

#include <memory>

namespace esa {

// The wire to a radio's CAT port. A serial port in the app, a mock radio in tests.
class CatTransport : public QObject {
    Q_OBJECT

public:
    explicit CatTransport(QObject* parent = nullptr) : QObject(parent) {}

    virtual bool open(const QString& port, int baud, QString* error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual void write(const QByteArray& bytes) = 0;
    virtual QByteArray readAll() = 0;

signals:
    void readyRead();
    // The port went away (unplugged, the radio switched off).
    void lost(const QString& why);
};

// Why no real serial port may open here, or empty if one may. Under the test sandbox none may:
// a test never reaches a radio, an amplifier or anything else on this computer's ports.
QString realSerialRefusal();

// The serial ports a radio's CAT may be on: listed, never opened. On macOS each port appears
// twice, and only the cu. one is offered: opening its tty. twin waits for a carrier.
QList<QSerialPortInfo> listSerialPorts();

// A real serial port, 8N1 with no flow control. DTR and RTS are dropped at once on opening,
// because a radio can map either to transmit.
std::unique_ptr<CatTransport> makeSerialTransport(QObject* parent = nullptr);

} // namespace esa
