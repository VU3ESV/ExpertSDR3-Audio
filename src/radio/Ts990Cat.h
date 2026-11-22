#pragma once

#include "radio/CatTransport.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

namespace esa {

// The Kenwood TS-990S's CAT, read from its USB virtual serial port, after the radio's own PC
// Control Command Reference (2013). It sends an allowlist only (allowed()): the radio's ID, the
// main and sub frequencies and modes, the main AF level, and one write, the main AF level. Nothing
// else can reach the radio, whatever a caller asks for: no transmit, no menu, no power, no tuning.
// Lives on the engine thread.
class Ts990Cat : public QObject {
    Q_OBJECT

public:
    enum class State {
        Closed,
        Identifying,  // the port is open; waiting for ID022
        Ready,
        Refused,      // another radio answered: closed, and not tried again until reopened
    };
    Q_ENUM(State)

    struct Band {
        qint64 frequencyHz{0};  // 0: not read yet, or a memory channel with nothing stored
        QString mode;           // as the radio names it: "USB", "CW-R", "USB-D1"
    };

    using TransportFactory = std::function<std::unique_ptr<CatTransport>(QObject* parent)>;

    // `transport` is a seam for tests; empty opens real serial ports.
    explicit Ts990Cat(TransportFactory transport = {}, QObject* parent = nullptr);
    ~Ts990Cat() override;

    // Opens the port and keeps trying until close(). `identifyOnly` stops at the ID: the
    // Radio page's Test, which sends ID; and nothing else.
    void open(const QString& port, int baud, bool identifyOnly = false);
    void close();

    State state() const { return m_state; }
    // Opened and not closed since: trying, talking, or refused another radio.
    bool active() const { return m_wantOpen || m_state == State::Refused; }
    QString lastError() const { return m_lastError; }
    QString port() const { return m_portName; }
    const Band& band(int receiver) const;

    // Reads the main receiver's AF level on every poll, for turning the speaker down. Set
    // before open().
    void setReadsAfLevel(bool on) { m_readAf = on; }
    // The main receiver's AF level, 0 to 255: read, and set then read back.
    void readAfLevel();
    void setAfLevel(int level);
    bool afKnown() const { return m_afLevel >= 0; }
    // Closing: a level set that is queued and not yet sent goes out now, if it gives the speaker
    // back (above 0). A pending 0 is dropped.
    void flushPendingLevel();
    int afLevel() const { return m_afLevel; }

    // Whether this exact command may be sent to the radio.
    static bool allowed(const QByteArray& command);
    // OM's mode code, as the front panel names it; empty for a code it does not use.
    static QString modeName(char code);

    static constexpr int kPollMs = 500;
    static constexpr int kReplyTimeoutMs = 500;
    static constexpr int kRetryMs = 3000;
    // Answers missed in a row before the radio counts as gone.
    static constexpr int kMissesToLose = 6;

signals:
    void stateChanged(esa::Ts990Cat::State state);
    // A receiver's frequency or mode changed: 0 main, 1 sub.
    void bandChanged(int receiver);
    void afLevelRead(int level);
    // A set's read-back: whether the radio holds what was set.
    void afLevelSet(int level, bool held);

private:
    struct Request {
        QByteArray command;
        QByteArray answer;    // what the reply starts with; empty for a set, which has none
        int readBackOf{-1};   // the AF level the set before this read asked for
    };

    void setState(State state);
    void tryOpen();
    void enqueue(const QByteArray& command, const QByteArray& answer, int readBackOf = -1);
    void idUnanswered();
    void sendNext();
    void writeCommand(const QByteArray& command);
    void onReadyRead();
    void onReply(const QByteArray& reply);
    void onTimeout();
    void poll();
    void lose(const QString& why);
    void trace(char direction, const QByteArray& bytes);

    TransportFactory m_transportFactory;
    std::unique_ptr<CatTransport> m_transport;
    QTimer m_poll;
    QTimer m_reply;
    QTimer m_retry;

    State m_state{State::Closed};
    QString m_lastError;
    QString m_portName;
    int m_baud{0};
    bool m_identifyOnly{false};
    bool m_readAf{false};
    bool m_wantOpen{false};

    QList<Request> m_queue;
    bool m_waiting{false};
    QByteArray m_buffer;
    int m_misses{0};
    int m_idTries{0};

    Band m_bands[2];
    int m_afLevel{-1};
};

} // namespace esa
