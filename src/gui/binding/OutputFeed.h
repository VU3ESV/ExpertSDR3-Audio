#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QtGlobal>

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

class QTimer;

namespace AetherSDR {
class AudioEngine;
}

namespace esa {

struct LiveTaps;

// Feeds Final Output's meter and scope from what the engine hands to the speakers
// (LiveTaps::outTap), shaped as the speaker path shapes it: Volume, then BOOST, then the
// clamp. MUTE is left out, as upstream's meter leaves it out (upstream mutes at the sink's
// volume, after its meter): under MUTE they still show what would play, so the operator sees
// the radio's audio arriving while the speakers are quiet. Otherwise they show what leaves
// this app, never a level the speakers would not get.
//
// It runs only while started, which the page does while it is on screen. When nothing has
// reached the output for a while (no radio, or a radio sending no audio) it feeds silence,
// so the meter falls and the scope draws a flat line, where upstream's scope would say
// "Enable PC Audio", which means nothing here.
//
// GUI thread only. The engine is the ring's only producer and this its only consumer.
class OutputFeed : public QObject {
    Q_OBJECT

public:
    // What the speaker path does to a sample, as the window holds it now, MUTE aside.
    struct Shape {
        double volumeDb{0.0};
        bool boost{false};
    };

    static constexpr int kTickMs = 16;
    // Nothing handed to the output for this long, and the feed turns to silence.
    static constexpr int kStaleMs = 250;
    // Silence goes out in 40 ms pieces, 25 times a second, as upstream's scope tap runs.
    static constexpr int kSilenceEveryMs = 40;

    OutputFeed(std::shared_ptr<LiveTaps> taps, AetherSDR::AudioEngine* engine, std::function<Shape()> shape,
               QObject* parent = nullptr);
    ~OutputFeed() override;

    // Starts from what arrives from now on: what queued while stopped is skipped.
    void start();
    void stop();
    bool isRunning() const;

    // Whether it is feeding silence because nothing has reached the output for a while.
    bool feedingSilence() const { return m_silent; }

    // Drains the ring and publishes it, or silence. The timer calls this; tests may too.
    void tick();

    // The mono samples the meter and the scope get for `frames` stereo frames: each channel
    // as the speaker path shapes it, folded to one and held within full scale.
    static void toMono(const float* stereo, size_t frames, const Shape& shape, float* mono);

signals:
    void silenceChanged(bool silent);

private:
    void publish(size_t samples, int rate);
    void setSilent(bool silent);

    std::shared_ptr<LiveTaps> m_taps;
    AetherSDR::AudioEngine* m_engine;
    std::function<Shape()> m_shape;
    QTimer* m_timer;
    QElapsedTimer m_clock;
    std::vector<float> m_stereo;  // sized once, to the whole ring
    std::vector<float> m_mono;
    quint64 m_lastBlocks{0};
    qint64 m_lastAdvanceMs{0};
    qint64 m_lastSilenceMs{0};
    bool m_silent{true};  // until something is seen reaching the output, nothing has
};

} // namespace esa
