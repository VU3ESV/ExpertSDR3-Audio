#pragma once

#include "app/Profiles.h"
#include "app/Settings.h"
#include "app/WavWriter.h"
#include "audio/AudioSink.h"
#include "dsp/RxChain.h"
#include "tci/MuteController.h"
#include "tci/TciClient.h"

#include <QElapsedTimer>
#include <QObject>
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
    explicit Engine(const QString& settingsPath = QString(), SinkFactory sinkFactory = {},
                    QObject* parent = nullptr);
    ~Engine() override;

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
    std::unique_ptr<AudioSink> m_sink;
    QTimer m_housekeeping;
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

    QString m_captureBase;
    WavWriter m_captureBefore;
    WavWriter m_captureAfter;
};

} // namespace esa

Q_DECLARE_METATYPE(esa::EngineStatus)
Q_DECLARE_METATYPE(esa::AppConfig)
Q_DECLARE_METATYPE(esa::ChainSettings)
Q_DECLARE_METATYPE(esa::AudioDeviceInfo)
