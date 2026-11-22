#pragma once

#include "audio/AudioSource.h"
#include "radio/RadioBackend.h"
#include "radio/Ts990Cat.h"
#include "radio/UsbModels.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QTimer>

#include <functional>
#include <memory>
#include <vector>

namespace AetherSDR {
class Resampler;
}

namespace esa {

// A radio whose receive audio comes from its own USB codec, such as a Kenwood TS-990S. The
// codec streams whether or not the radio is on, so audio arriving proves nothing about the
// radio. Where the model has CAT and a port is chosen, its frequency and mode are read beside
// the audio; the audio never waits on CAT, and works with none. Lives on the engine thread.
class UsbBackend : public RadioBackend {
    Q_OBJECT

public:
    using SourceFactory =
        std::function<std::unique_ptr<AudioSource>(const QString& deviceKey, int preferredRate, QString* error)>;

    // `sourceFactory` and `catTransport` are seams for tests; empty opens real devices.
    explicit UsbBackend(SourceFactory sourceFactory = {}, QObject* parent = nullptr,
                        Ts990Cat::TransportFactory catTransport = {});
    ~UsbBackend() override;

    QString kind() const override { return QStringLiteral("usb"); }
    Capabilities capabilities() const override;

    void open(const AppConfig& config) override;
    void flush() override;
    void close() override;
    void requestAudio(int receiver, int chainRate) override;
    void stopAudio() override {}

    Link state() const override { return m_state; }
    const RadioState& radio() const override { return m_radio; }
    QString lastError() const override { return m_lastError; }

    Input input() const override;
    void setInputTrimDb(double db) override;
    Cat cat() const override;
    void setSpeakerLevel(int level) override;
    bool speakerLinkUp() const override;
    // The CAT link, while a port is chosen; null for audio only.
    Ts990Cat* catLink() const { return m_cat; }
    void releaseAudioDevices() override;
    void reacquireAudioDevices() override;

    // Frames per block handed on, at the chain's rate: about 10.7 ms, as TCI's blocks are.
    static int blockFrames(int chainRate) { return chainRate == 24000 ? 256 : 512; }

private:
    void setState(Link state);
    void tryOpen();
    void lose(const QString& why);
    void drain();
    void deliver(const float* mono, size_t frames);
    void onCatBand(int receiver);
    void updateSpeakerLink();

    SourceFactory m_sourceFactory;
    Ts990Cat::TransportFactory m_catTransport;
    Ts990Cat* m_cat{nullptr};
    bool m_speakerUp{false};
    std::unique_ptr<AudioSource> m_source;
    QTimer m_retry;
    QTimer m_drain;
    QElapsedTimer m_clock;

    Link m_state{Link::Disconnected};
    RadioState m_radio;
    QString m_lastError;
    bool m_wantOpen{false};
    QString m_inputKey;
    UsbModel m_model;
    float m_trim{1.0f};
    int m_receiver{0};
    int m_chainRate{48000};

    std::unique_ptr<AetherSDR::Resampler> m_resampler;  // device rate to chain rate, when they differ
    QByteArray m_converted;
    std::vector<float> m_captured;  // stereo, as read from the device
    std::vector<float> m_mono;      // the receiver's channel
    std::vector<float> m_pending;   // at the chain's rate, until a whole block is ready
    std::vector<float> m_block;     // stereo, as handed on

    quint64 m_lastCallbacks{0};
    qint64 m_lastProgressMs{0};
};

} // namespace esa
