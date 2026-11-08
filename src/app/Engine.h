#pragma once

#include "app/LiveTaps.h"
#include "app/Profiles.h"
#include "app/Recorder.h"
#include "app/Settings.h"
#include "audio/AudioSink.h"
#include "dsp/RxChain.h"
#include "tci/MuteController.h"
#include "tci/TciClient.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

#include <functional>
#include <memory>
#include <vector>

namespace esa {

// A snapshot for display, emitted several times a second.
struct EngineStatus {
    TciClient::State connection{TciClient::State::Disconnected};
    QString connectionError;
    QString serverName, serverVersion, device;
    bool receiveOnly{false};
    int trxCount{0};
    int connectedPort{0};
    int receiver{0};
    qint64 frequencyHz{0};
    QString modulation;
    bool receiverEnabled{true};
    bool transmitting{false};

    MuteController::Phase mutePhase{MuteController::Phase::Idle};

    QString nrMethod;         // what the operator selected; empty when off
    bool nrPreparing{false};  // still being built off the audio path
    bool nrFailed{false};     // construction failed; running dry
    bool nrBypassedByMode{false};
    float nrGainDb{0.0f};

    QString outputDevice;
    QString outputError;
    bool outputOpen{false};
    SinkStats sink;
    float inputPeakDb{-120.0f};
    float outputPeakDb{-120.0f};
    bool audioFlowing{false};
    double blocksPerSecond{0.0};
    quint64 blocks{0};
    quint64 droppedBlocks{0};  // gaps in the stream's packet counter
    quint64 malformedBlocks{0};
    double dspLoad{0.0};  // fraction of real time spent in the chain
    int chainRate{48000};  // the rate the chain runs at: what was asked for, or what the radio sends
    bool rxBypassed{false};  // the operator's BYPASS

    // REC: the chain's output, before volume, BOOST and mute.
    struct Recording {
        bool active{false};        // accepted and not yet stopped; it may still be waiting for audio
        // The file being written, or the last one written; empty until a file exists, so a
        // REC that never got audio names no file.
        QString path;
        double seconds{0.0};       // audio recorded so far, across every part
        quint64 droppedFrames{0};  // audio the disk could not keep up with: shown in amber
        QString error;             // why REC was refused or stopped early; empty otherwise
        // `error` says why a press was turned down ("Connect to a radio first"), not why a
        // recording stopped: nothing was recorded, so there is nothing to warn about.
        bool refused{false};
    } recording;
    // PLAY of the last recording.
    struct Playback {
        bool active{false};
        double seconds{0.0};       // played so far
        double totalSeconds{0.0};  // 0 until the file has been read
    } playback;
    bool lastRecordingAvailable{false};  // PLAY has something to play
    QString lastRecordingPath;           // what it plays: the last file of the last finished recording
    // --capture: the files written so far, before and after the chain, and why it stopped if it did.
    struct Capture {
        QStringList files;
        QString error;
    } capture;
};

// Runs the whole audio path on its own thread: TCI in, chain, sink out, plus the
// mute policy. Every public slot must be invoked on that thread (queued from the
// GUI), and status is reported through signals.
class Engine : public QObject {
    Q_OBJECT

public:
    using SinkFactory = std::function<std::unique_ptr<AudioSink>(
        const AppConfig& config, QString* error)>;

    // `settingsPath` selects the settings store for mute ownership. Empty uses the
    // default store. `sinkFactory` is a seam for tests; empty uses real devices.
    // `recorderOptions` too: tests give the disk thread files that are slow or fill up.
    explicit Engine(const QString& settingsPath = QString(), SinkFactory sinkFactory = {},
                    QObject* parent = nullptr, Recorder::Options recorderOptions = Recorder::Options());
    ~Engine() override;

    // The same object for the engine's whole life, so any thread may call this. Holding
    // it keeps the data valid after the engine is gone.
    std::shared_ptr<LiveTaps> taps() const { return m_taps; }

public slots:
    void configure(const esa::AppConfig& config);
    void connectRadio();
    // Returns at once. The socket closes once ExpertSDR3's mute has been restored. A connectRadio()
    // meanwhile takes effect straight after that, so switching radios cannot lose its way.
    void disconnectRadio();
    void setReceiver(int receiver);
    void setOutputDevice(const QString& deviceKey);
    void setBufferMs(int ms);
    void setOutputGainDb(double db);
    void setBoost(bool on);
    void setOutputMuted(bool muted);
    void setMuteExpert(bool on);
    void setNrAutoBypass(bool on);
    void setNrMethod(const QString& id);
    void setStageOrder(const QStringList& order);
    void setParam(const QString& key, double value);
    // Replaces the whole audio chain at once, as when a profile is loaded.
    void applyChainSettings(const esa::ChainSettings& chain);
    // The operator's BYPASS: the radio's audio plays with no NR and no stages. Kept in
    // memory only, never in the settings, so a crash cannot leave everything off.
    void setRxBypassed(bool on);
    // REC. Records the chain's output into `folder`, named from the time and the radio's
    // frequency and mode when the link is up. Accepted only while connected, even if the link is
    // still coming up; otherwise the status says "Connect to a radio first". Sends nothing to the
    // radio.
    void startRecording(const QString& folder);
    // The same into the file `path` (or the first free name after it): headless --record x.wav.
    void startRecordingToFile(const QString& path);
    void stopRecording();
    // PLAY: the last finished recording, through the selected output with its volume, BOOST and
    // mute, in place of the live audio, which goes on being processed (and recorded). With no
    // radio connected it opens the output for the playback alone. Sends nothing to the radio.
    void startPlayback();
    void stopPlayback();
    // Diagnostics: records the radio's audio to <base>.before.wav and the chain's output to
    // <base>.after.wav, from now until disconnect or quit, as the next free names if those exist.
    // A rate change continues each in a _part2 file. Empty stops it.
    void setCapture(const QString& basePath);
    // Rescans audio hardware. The output reopens briefly around it, because
    // PortAudio only sees new devices across a re-init.
    void refreshDevices();
    // Restore ExpertSDR3's mute, stop audio and close. Unlike disconnectRadio(), this returns only
    // once the restore is confirmed or has timed out. Call before deleting the engine.
    void shutdown();

signals:
    void statusChanged(const esa::EngineStatus& status);
    void devicesChanged(const QList<esa::AudioDeviceInfo>& devices);
    void connectedPortChanged(int port);
    // Once each time REC is refused or a recording stops early (disk full, folder not writable).
    // The same text is in EngineStatus::recording.error. Whatever reached the disk is kept, and
    // the live audio carries on.
    void recordingFailed(const QString& reason);
    // PLAY was refused (nothing recorded, another sample rate, no output) or stopped early.
    void playbackFailed(const QString& reason);

private slots:
    void onAudioBlock(const esa::tci::StreamHeader& header, const QByteArray& frame);
    void onConnectionState(esa::TciClient::State state);
    void onMuteReported(bool muted);
    void onModulationChanged(int receiver, const QString& modulation);
    void onTransmitChanged(int receiver, bool transmitting);
    void onHousekeeping();
    void continueClosing();

private:
    void finishClosing();
    void rebuildChain(int sampleRate);
    // Everything done with a processed block: meters, taps, recording, capture and the speakers.
    void publishBlock(const float* stereo, int frames);

    void beginRecording(const QString& folder, const QString& file);
    void refuseRecording(const QString& reason);
    void openRecordingSession();
    void rollRecording(int sampleRate);
    // Ends the recording in progress, if any; a non-empty reason reports it as a failure.
    void endRecording(const QString& failure);
    void onRecorderFileOpened(quint64 id, const QString& path);
    void onRecorderFileClosed(quint64 id, const QString& path, quint64 frames, int sampleRate);
    void onRecorderFailed(quint64 id, const QString& reason);
    void onRecorderFinished(quint64 id);
    void pushCapture(std::shared_ptr<RecordSession>& session, const float* stereo, int frames, int sampleRate);
    void finishCapture();
    // Disconnect and quit: every session ends, and this returns once every file is closed.
    void finishRecordings();

    void refusePlayback(const QString& reason);
    void pumpPlayback();
    // Stops PLAY, if it is on; a non-empty reason reports it as a failure. `playOut` lets an
    // output opened for the playback alone finish what it holds before it closes.
    void endPlayback(const QString& failure, bool playOut = false);
    // Closes an output opened for a playback alone, now, if there is one.
    void releasePlaybackSink();

    void copyStageMeters();
    void idleStageMeters();
    void startNrBuild();
    // Mute ownership belongs to one radio, so a different radio gets its own controller.
    void prepareMuteFor(const QString& host);
    void openSink();
    void closeSink();
    void updateMuteWanted();
    void updateNrBypass();
    void emitStatus();
    void requestAudio();
    qint64 nowMs() const { return m_clock.elapsed(); }

    SinkFactory m_sinkFactory;
    std::unique_ptr<Settings> m_settings;
    AppConfig m_config;

    TciClient* m_tci;
    std::unique_ptr<MuteController> m_mute;
    QString m_muteHost;
    std::unique_ptr<RxChain> m_chain;
    const std::shared_ptr<LiveTaps> m_taps;
    bool m_rxBypassed{false};
    std::unique_ptr<AudioSink> m_sink;
    QTimer m_housekeeping;
    // Puts the stage meters to rest exactly kMeterIdleMs after the last block, which the
    // housekeeping tick alone would only notice up to a tick late. Armed only near then.
    QTimer m_meterRest;
    QElapsedTimer m_clock;

    // Disconnecting is a step that takes time, not a call that waits: ExpertSDR3's mute has to be
    // restored before the socket closes. Waiting inside a nested event loop would let queued calls
    // run in the middle of it, so a connect could be undone by the disconnect it interrupted.
    QTimer m_closeTimer;
    bool m_closing{false};
    bool m_connectAfterClosing{false};
    qint64 m_closeDeadlineMs{0};

    // NR objects are slow to build (model load, FFT planning), so they are built on
    // a pool and adopted on this thread. The generation discards a build the
    // operator has since superseded.
    QThreadPool m_nrPool;
    quint64 m_nrGeneration{0};
    bool m_nrPreparing{false};
    bool m_nrFailed{false};

    bool m_sessionActive{false};   // the operator asked for a connection
    bool m_audioConfirmed{false};  // audio has actually arrived in this session
    bool m_outputMuted{false};
    QString m_outputError;
    QString m_outputDeviceName;
    qint64 m_lastBlockMs{-1};
    qint64 m_nextSinkRetryMs{0};

    QString m_modulation;
    bool m_complexStereo{false};

    std::vector<float> m_decoded;
    QByteArray m_input;

    bool m_counterValid{false};
    quint32 m_lastCounter{0};
    quint64 m_blocks{0};
    quint64 m_droppedBlocks{0};
    quint64 m_malformedBlocks{0};
    quint64 m_blocksAtLastRate{0};
    qint64 m_lastRateMs{0};
    double m_blocksPerSecond{0.0};
    float m_inputPeak{0.0f};
    float m_outputPeak{0.0f};
    qint64 m_dspNanos{0};
    qint64 m_dspWindowMs{0};
    double m_dspLoad{0.0};

    // The recorder's signals are delivered to this, not to the engine itself, so that shutdown()
    // can take exactly those before its final status and no other queued call.
    QObject m_recorderInbox;
    // The disk thread. The engine thread only ever hands it frames through a session's ring.
    Recorder m_recorder;

    // REC, from the press to the stop, across the parts a rate change makes.
    bool m_recording{false};
    QString m_recordFolder;  // REC named from the radio, into this folder
    QString m_recordFile;    // or this file, for headless --record x.wav
    std::shared_ptr<RecordSession> m_record;  // the part being written; null while waiting for the link
    QSet<quint64> m_recordParts;              // every part's session id, to match the recorder's signals
    quint64 m_recordFramesPushed{0};          // into the current part
    double m_recordSecondsBefore{0.0};        // in earlier parts
    quint64 m_recordDroppedBefore{0};
    QString m_recordPath;
    QString m_recordError;
    bool m_recordRefused{false};  // m_recordError is a refusal, which connecting answers
    // Recordings that have ended, by their last part's session id, until the disk thread is done
    // with them; true once a failure of theirs has been reported.
    QHash<quint64, bool> m_endingRecordings;
    QSet<quint64> m_recordSessions;  // every REC session ever, as opposed to --capture's
    QString m_closedRecordPath;      // the last REC file the disk thread closed
    int m_closedRecordRate{0};
    QString m_lastRecordingPath;     // what PLAY plays
    int m_lastRecordingRate{0};

    QString m_captureBase;
    std::shared_ptr<RecordSession> m_captureBefore;
    std::shared_ptr<RecordSession> m_captureAfter;
    QSet<quint64> m_captureSessions;
    QStringList m_captureFiles;
    QString m_captureError;

    // PLAY: a 10 ms precise timer moves the frames due by elapsed time from the disk thread's
    // ring to the output.
    QTimer m_playTimer;
    std::shared_ptr<PlaySession> m_play;
    PlayPacer m_playPacer;
    bool m_playStarted{false};    // the file is open and the pacer running
    // The output was opened for the playback alone. It stays set after a playback that ran to
    // its end, until m_playDrain has let the output's buffer play out.
    bool m_playOwnsSink{false};
    QTimer m_playDrain;
    int m_playRate{0};
    int m_sinkRate{0};            // the rate the open output takes
    std::vector<float> m_playScratch;
};

} // namespace esa

Q_DECLARE_METATYPE(esa::EngineStatus)
Q_DECLARE_METATYPE(esa::AppConfig)
Q_DECLARE_METATYPE(esa::ChainSettings)
Q_DECLARE_METATYPE(esa::AudioDeviceInfo)
