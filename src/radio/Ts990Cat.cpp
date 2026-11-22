#include "radio/Ts990Cat.h"

#include <QDateTime>
#include <QFile>
#include <QLoggingCategory>

#include <algorithm>
#include <cctype>

// Every byte at debug, off unless asked for (QT_LOGGING_RULES="esa.cat.debug=true"); ESA_CAT_TRACE
// keeps them in a file.
Q_LOGGING_CATEGORY(lcCat, "esa.cat", QtInfoMsg)

namespace esa {

namespace {

// The radio's own answer to ID;.
const QByteArray kTs990Id = QByteArrayLiteral("022");
// ID; tries before saying nothing answers; then again every kRetryMs.
constexpr int kIdTries = 3;
// The longest reply this app reads, with room to spare; anything longer is not the radio.
constexpr qsizetype kMaxReply = 64;

bool allDigits(const QByteArray& text)
{
    return !text.isEmpty() && std::all_of(text.cbegin(), text.cend(), [](char c) { return c >= '0' && c <= '9'; });
}

std::unique_ptr<CatTransport> realTransport(QObject* parent)
{
    return makeSerialTransport(parent);
}

} // namespace

Ts990Cat::Ts990Cat(TransportFactory transport, QObject* parent)
    : QObject(parent),
      m_transportFactory(transport ? std::move(transport) : TransportFactory(realTransport)),
      m_poll(this),
      m_reply(this),
      m_retry(this)
{
    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, &Ts990Cat::poll);
    m_reply.setSingleShot(true);
    m_reply.setInterval(kReplyTimeoutMs);
    connect(&m_reply, &QTimer::timeout, this, &Ts990Cat::onTimeout);
    m_retry.setSingleShot(true);
    m_retry.setInterval(kRetryMs);
    connect(&m_retry, &QTimer::timeout, this, &Ts990Cat::tryOpen);
}

Ts990Cat::~Ts990Cat()
{
    close();
}

bool Ts990Cat::allowed(const QByteArray& command)
{
    // Reads. Each has a reply and changes nothing on the radio.
    static const QList<QByteArray> reads = {
        QByteArrayLiteral("ID;"),  QByteArrayLiteral("FA;"),  QByteArrayLiteral("FB;"),
        QByteArrayLiteral("OM0;"), QByteArrayLiteral("OM1;"), QByteArrayLiteral("AG0;"),
    };
    if (reads.contains(command)) return true;
    // The one write: the main receiver's AF level, AG0000; to AG0255;.
    if (command.size() == 7 && command.startsWith("AG0") && command.endsWith(';')) {
        const QByteArray level = command.mid(3, 3);
        return allDigits(level) && level.toInt() <= 255;
    }
    return false;
}

QString Ts990Cat::modeName(char code)
{
    switch (code) {
    case '1': return QStringLiteral("LSB");
    case '2': return QStringLiteral("USB");
    case '3': return QStringLiteral("CW");
    case '4': return QStringLiteral("FM");
    case '5': return QStringLiteral("AM");
    case '6': return QStringLiteral("FSK");
    case '7': return QStringLiteral("CW-R");
    case '9': return QStringLiteral("FSK-R");
    case 'A': return QStringLiteral("PSK");
    case 'B': return QStringLiteral("PSK-R");
    case 'C': return QStringLiteral("LSB-D1");
    case 'D': return QStringLiteral("USB-D1");
    case 'E': return QStringLiteral("FM-D1");
    case 'F': return QStringLiteral("AM-D1");
    case 'G': return QStringLiteral("LSB-D2");
    case 'H': return QStringLiteral("USB-D2");
    case 'I': return QStringLiteral("FM-D2");
    case 'J': return QStringLiteral("AM-D2");
    case 'K': return QStringLiteral("LSB-D3");
    case 'L': return QStringLiteral("USB-D3");
    case 'M': return QStringLiteral("FM-D3");
    case 'N': return QStringLiteral("AM-D3");
    default: return QString();
    }
}

const Ts990Cat::Band& Ts990Cat::band(int receiver) const
{
    return m_bands[receiver == 1 ? 1 : 0];
}

void Ts990Cat::open(const QString& port, int baud, bool identifyOnly)
{
    close();
    m_portName = port;
    m_baud = baud;
    m_identifyOnly = identifyOnly;
    m_wantOpen = true;
    m_lastError.clear();
    tryOpen();
}

void Ts990Cat::close()
{
    m_wantOpen = false;
    m_poll.stop();
    m_reply.stop();
    m_retry.stop();
    m_queue.clear();
    m_waiting = false;
    m_buffer.clear();
    if (m_transport) {
        m_transport->disconnect(this);
        m_transport->close();
        // Not deleted here: this can run inside the port's own readyRead, and a serial port
        // still writes to itself after emitting it.
        m_transport.release()->deleteLater();
    }
    setState(State::Closed);
}

void Ts990Cat::setState(State state)
{
    if (state == m_state) return;
    const bool leftReady = m_state == State::Ready;
    m_state = state;
    if (leftReady) {
        // What was read is no longer known: a mode left standing would keep NR bypassed.
        for (int receiver = 0; receiver < 2; ++receiver) {
            if (m_bands[receiver].frequencyHz == 0 && m_bands[receiver].mode.isEmpty()) continue;
            m_bands[receiver] = Band();
            emit bandChanged(receiver);
        }
        m_afLevel = -1;
    }
    emit stateChanged(state);
}

void Ts990Cat::tryOpen()
{
    if (!m_wantOpen) return;
    if (m_portName.isEmpty()) {
        m_lastError = QStringLiteral("No CAT port chosen");
        return;
    }
    if (!m_transport) {
        m_transport = m_transportFactory(this);
        connect(m_transport.get(), &CatTransport::readyRead, this, &Ts990Cat::onReadyRead);
        connect(m_transport.get(), &CatTransport::lost, this, [this](const QString& why) { lose(why); });
    }
    if (!m_transport->isOpen()) {
        QString error;
        if (!m_transport->open(m_portName, m_baud, &error)) {
            m_lastError = error;
            m_retry.start();
            return;
        }
    }
    m_misses = 0;
    m_idTries = 0;
    m_buffer.clear();
    m_queue.clear();
    m_waiting = false;
    setState(State::Identifying);
    enqueue(QByteArrayLiteral("ID;"), QByteArrayLiteral("ID"));
}

void Ts990Cat::lose(const QString& why)
{
    qCWarning(lcCat) << "CAT lost:" << why;
    m_lastError = why;
    m_poll.stop();
    m_reply.stop();
    m_queue.clear();
    m_waiting = false;
    m_afLevel = -1;
    if (m_transport) m_transport->close();
    setState(m_wantOpen ? State::Identifying : State::Closed);
    if (m_wantOpen) m_retry.start();
}

void Ts990Cat::readAfLevel()
{
    if (m_state == State::Ready && !m_identifyOnly) enqueue(QByteArrayLiteral("AG0;"), QByteArrayLiteral("AG0"));
}

void Ts990Cat::setAfLevel(int level)
{
    if (m_state != State::Ready || m_identifyOnly || level < 0 || level > 255) return;
    enqueue(QStringLiteral("AG0%1;").arg(level, 3, 10, QLatin1Char('0')).toLatin1(), QByteArray());
    // A set has no answer: the read after it says whether the radio took it.
    enqueue(QByteArrayLiteral("AG0;"), QByteArrayLiteral("AG0"), level);
}

void Ts990Cat::enqueue(const QByteArray& command, const QByteArray& answer, int readBackOf)
{
    if (!allowed(command)) {
        // Never reached by this app's own calls; refused here whatever asks for it.
        qCCritical(lcCat) << "Refused to send a command outside the allowlist:" << command;
        return;
    }
    if (m_identifyOnly && command != "ID;") {
        // The Radio page's Test promises ID; and nothing else.
        qCCritical(lcCat) << "Refused to send" << command << "while only identifying the radio";
        return;
    }
    // A read already waiting in the queue is not asked for twice, unless a set lies between:
    // then the later read answers a different question.
    if (!answer.isEmpty() && readBackOf < 0) {
        for (auto it = m_queue.crbegin(); it != m_queue.crend(); ++it) {
            if (it->answer.isEmpty()) break;
            if (it->command == command && it->readBackOf < 0) return;
        }
    }
    m_queue.append({command, answer, readBackOf});
    if (!m_waiting) sendNext();
}

void Ts990Cat::sendNext()
{
    while (!m_waiting && !m_queue.isEmpty() && m_transport && m_transport->isOpen()) {
        const Request& next = m_queue.first();
        trace('>', next.command);
        m_transport->write(next.command);
        if (next.answer.isEmpty()) {
            m_queue.removeFirst();  // nothing comes back for a set
            continue;
        }
        m_waiting = true;
        m_reply.start();
    }
}

void Ts990Cat::onReadyRead()
{
    if (!m_transport) return;
    m_buffer += m_transport->readAll();
    qsizetype end = 0;
    while ((end = m_buffer.indexOf(';')) >= 0) {
        const QByteArray reply = m_buffer.left(end).trimmed();
        m_buffer.remove(0, end + 1);
        trace('<', reply + ';');
        onReply(reply);
    }
    // Garbage with no terminator (a wrong baud rate) is dropped rather than kept growing.
    if (m_buffer.size() > kMaxReply) m_buffer.clear();
}

void Ts990Cat::onReply(const QByteArray& reply)
{
    const bool error = reply == "?" || reply == "E" || reply == "O";
    Request answered;
    if (m_waiting && !m_queue.isEmpty() && (error || reply.startsWith(m_queue.first().answer))) {
        m_reply.stop();
        m_waiting = false;
        answered = m_queue.takeFirst();
        m_misses = 0;
    }

    if (error) {
        // Busy, or a command it could not take: nothing was read.
        qCInfo(lcCat) << "The radio answered" << reply << "to" << answered.command;
        if (answered.readBackOf >= 0) emit afLevelSet(answered.readBackOf, false);
        if (answered.command == "ID;" && m_state == State::Identifying) {
            idUnanswered();
            return;
        }
    } else if (reply.startsWith("ID")) {
        const QByteArray id = reply.mid(2);
        if (id == kTs990Id) {
            if (m_state == State::Identifying) {
                m_lastError.clear();
                setState(State::Ready);
                if (!m_identifyOnly) {
                    poll();
                    m_poll.start();
                }
            }
        } else if (allDigits(id)) {
            // Another radio. Nothing it would read here can be trusted, nor any level it holds.
            m_lastError = QStringLiteral("The radio on %1 answered ID%2, not a TS-990S (ID022)")
                              .arg(m_portName, QString::fromLatin1(id));
            qCWarning(lcCat) << m_lastError;
            const QString why = m_lastError;
            close();
            m_lastError = why;
            setState(State::Refused);
            return;
        }
    } else if (m_state == State::Ready) {
        if ((reply.startsWith("FA") || reply.startsWith("FB")) && reply.size() == 13) {
            const int receiver = reply.at(1) == 'A' ? 0 : 1;
            const QByteArray digits = reply.mid(2);
            // Spaces for a memory channel with nothing stored.
            const qint64 hz = allDigits(digits) ? digits.toLongLong() : 0;
            if (hz != m_bands[receiver].frequencyHz) {
                m_bands[receiver].frequencyHz = hz;
                emit bandChanged(receiver);
            }
        } else if (reply.startsWith("OM") && reply.size() == 4 && (reply.at(2) == '0' || reply.at(2) == '1')) {
            const int receiver = reply.at(2) - '0';
            const QString mode = modeName(reply.at(3));
            if (mode != m_bands[receiver].mode) {
                m_bands[receiver].mode = mode;
                emit bandChanged(receiver);
            }
        } else if (reply.startsWith("AG0") && reply.size() == 6 && allDigits(reply.mid(3))) {
            m_afLevel = reply.mid(3).toInt();
            emit afLevelRead(m_afLevel);
            if (answered.readBackOf >= 0) emit afLevelSet(answered.readBackOf, m_afLevel == answered.readBackOf);
        }
    }
    sendNext();
}

void Ts990Cat::onTimeout()
{
    if (!m_waiting || m_queue.isEmpty()) return;
    const Request missed = m_queue.takeFirst();
    m_waiting = false;
    ++m_misses;
    if (missed.command == "ID;" && m_state == State::Identifying) {
        idUnanswered();
        return;
    }
    if (missed.readBackOf >= 0) emit afLevelSet(missed.readBackOf, false);
    if (m_state == State::Ready && m_misses >= kMissesToLose) {
        // The radio stopped answering: switched off, or its port taken.
        m_poll.stop();
        m_queue.clear();
        m_lastError = QStringLiteral("The radio stopped answering on %1").arg(m_portName);
        m_afLevel = -1;
        setState(State::Identifying);
        m_idTries = 0;
        m_misses = 0;
        enqueue(QByteArrayLiteral("ID;"), QByteArrayLiteral("ID"));
        return;
    }
    sendNext();
}

void Ts990Cat::idUnanswered()
{
    if (++m_idTries >= kIdTries) {
        m_lastError = QStringLiteral("No answer on %1 at %2 baud. Is the radio on, and is its USB baud rate "
                                     "(menu 0-07-01) the same?")
                          .arg(m_portName)
                          .arg(m_baud);
        m_idTries = 0;
        m_retry.start();  // and ask again then
        return;
    }
    enqueue(QByteArrayLiteral("ID;"), QByteArrayLiteral("ID"));
}

void Ts990Cat::poll()
{
    if (m_state != State::Ready || !m_queue.isEmpty() || m_waiting) return;
    enqueue(QByteArrayLiteral("FA;"), QByteArrayLiteral("FA"));
    enqueue(QByteArrayLiteral("OM0;"), QByteArrayLiteral("OM0"));
    enqueue(QByteArrayLiteral("FB;"), QByteArrayLiteral("FB"));
    enqueue(QByteArrayLiteral("OM1;"), QByteArrayLiteral("OM1"));
}

void Ts990Cat::trace(char direction, const QByteArray& bytes)
{
    // ESA_CAT_TRACE=<file> keeps every byte to and from the radio, for checking by hand.
    static const QString path = qEnvironmentVariable("ESA_CAT_TRACE");
    qCDebug(lcCat) << direction << bytes;
    if (path.isEmpty()) return;
    QFile file(path);
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        file.write(QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toLatin1() + ' ' + direction + ' ' + bytes
                   + '\n');
    }
}

} // namespace esa
