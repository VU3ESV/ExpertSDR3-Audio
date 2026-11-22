#pragma once

#include <QHash>
#include <QObject>
#include <QString>

namespace esa {

struct AppConfig;

// What the radio has told us. Later reports overwrite earlier ones; a backend keeps this
// current, by the radio pushing changes (TCI) or by asking for them (CAT).
struct RadioState {
    QString serverName;     // "ExpertSDR3"; for a USB radio, its make
    QString serverVersion;  // "2.1"
    QString device;         // "ColibriDDC"; for a USB radio, its model
    bool receiveOnly{false};
    int trxCount{0};
    struct Receiver {
        qint64 frequencyHz{0};  // VFO A
        QString modulation;
        bool transmitting{false};
        bool enabled{true};
    };
    QHash<int, Receiver> receivers;
};

// One way of reaching a radio: where its receive audio comes from, what it reports, and how
// its own speaker can be silenced. The engine holds one, on the engine thread; every signal
// comes from that thread, and nothing is emitted once close() has returned Disconnected.
class RadioBackend : public QObject {
    Q_OBJECT

public:
    enum class Link { Disconnected, Connecting, Initializing, Ready };
    Q_ENUM(Link)

    // How the radio's own speaker can be silenced while this app plays.
    enum class SpeakerControl {
        None,    // it cannot: the operator turns the radio down
        Switch,  // a mute switch, which the radio may silently refuse for a while (TCI)
        Level,   // the AF level, set to 0 and back (CAT)
    };

    struct Capabilities {
        SpeakerControl speaker{SpeakerControl::None};
        // Audio arriving shows the radio is reaching us and producing sound. True for TCI, whose
        // stream stops when the radio does; not for a USB codec, which streams silence regardless.
        bool audioProvesRadio{false};
        bool reportsMode{false};
        bool reportsFrequency{false};
    };

    explicit RadioBackend(QObject* parent = nullptr) : QObject(parent) {}

    // "tci" or "usb": which kind of radio this reaches, as AppConfig::radioKind names it.
    virtual QString kind() const = 0;
    virtual Capabilities capabilities() const = 0;

    // Reach the radio that `config` describes, and keep trying until close().
    virtual void open(const AppConfig& config) = 0;
    // Push out what is pending (a mute restore), then close. Nothing is emitted afterwards.
    virtual void flush() {}
    virtual void close() = 0;

    // Applied once the link is up, and again after every reconnect.
    virtual void requestAudio(int receiver, int chainRate) = 0;
    virtual void stopAudio() = 0;

    // SpeakerControl::Switch only.
    virtual void sendMute(bool muted) { Q_UNUSED(muted); }

    virtual Link state() const = 0;
    virtual const RadioState& radio() const = 0;
    virtual QString lastError() const = 0;
    // The TCI port that answered; 0 where there is none.
    virtual int connectedPort() const { return 0; }

signals:
    void stateChanged(esa::RadioBackend::Link state);
    void radioChanged();
    void muteReported(bool muted);
    void modulationChanged(int receiver, const QString& modulation);
    void transmitChanged(int receiver, bool transmitting);
    // Interleaved stereo float at `sampleRate`, delivered directly on the engine thread; the
    // pointer is valid only during the call. Only 24000 and 48000 Hz reach the engine: a
    // backend counts any other rate as malformed.
    void audioBlock(int receiver, const float* stereo, int frames, int sampleRate);
    void malformedBlock();
    // Blocks the radio sent that never arrived, as its own counter shows.
    void blocksLost(quint64 count);
    // A TCI port that answered where the configured one did not; the window remembers it.
    void portResolved(int port);
};

} // namespace esa
