#pragma once

// Fakes for a USB radio's audio, shared by the tests that need one: a codec that delivers audio
// by the clock, the config that chooses it, and a sink that counts what the engine played.
#include "app/Settings.h"
#include "audio/AudioSink.h"
#include "audio/AudioSource.h"
#include "radio/UsbBackend.h"

#include <QElapsedTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

namespace esa::testing {

// What the test controls about its fake codec, shared with whatever the backend opened.
struct Codec {
    std::atomic<bool> present{true};  // plugged in: opening it works
    std::atomic<bool> alive{true};    // the stream has not failed
    std::atomic<bool> stalled{false}; // delivers nothing, without saying so
    std::atomic<quint64> overflows{0};
    float left{0.25f};
    float right{-0.5f};
    double toneHz{0.0};  // on both channels, over the levels above; 0: none
    int rate{48000};
    int opens{0};
    QString lastKey;
};

// Delivers audio as a device would, by the clock: what is due since it opened.
class FakeSource final : public AudioSource {
public:
    explicit FakeSource(std::shared_ptr<Codec> codec) : m_codec(std::move(codec)) { m_clock.start(); }

    QString deviceName() const override { return QStringLiteral("Test codec"); }
    int sampleRate() const override { return m_codec->rate; }
    size_t available() const override
    {
        if (m_codec->stalled.load()) return 0;
        const auto due = quint64(double(m_clock.nsecsElapsed()) * 1e-9 * m_codec->rate);
        return size_t(due > m_delivered ? due - m_delivered : 0);
    }
    size_t read(float* stereo, size_t frames) override
    {
        const size_t count = std::min(frames, available());
        for (size_t i = 0; i < count; ++i) {
            const double t = double(m_delivered + i) / m_codec->rate;
            const float tone = m_codec->toneHz > 0 ? float(0.2 * std::sin(2.0 * M_PI * m_codec->toneHz * t)) : 0.0f;
            stereo[i * 2] = m_codec->left + tone;
            stereo[i * 2 + 1] = m_codec->right + tone;
        }
        m_delivered += count;
        return count;
    }
    bool alive() const override { return m_codec->alive.load(); }
    SourceStats stats() const override
    {
        SourceStats s;
        s.deviceRate = m_codec->rate;
        s.deviceChannels = 2;
        // One callback every 1024 frames, while it delivers.
        if (!m_codec->stalled.load()) m_callbacks = quint64(double(m_clock.nsecsElapsed()) * 1e-9 * m_codec->rate / 1024.0);
        s.callbacks = m_callbacks;
        s.framesCaptured = m_delivered;
        s.overflows = m_codec->overflows.load();
        return s;
    }

private:
    std::shared_ptr<Codec> m_codec;
    QElapsedTimer m_clock;
    quint64 m_delivered{0};
    mutable quint64 m_callbacks{0};
};

inline UsbBackend::SourceFactory fakeCodec(const std::shared_ptr<Codec>& codec)
{
    return [codec](const QString& key, int, QString* error) -> std::unique_ptr<AudioSource> {
        ++codec->opens;
        codec->lastKey = key;
        if (key.isEmpty() || !codec->present.load()) {
            if (error) *error = QStringLiteral("not plugged in");
            return nullptr;
        }
        codec->alive = true;
        return std::make_unique<FakeSource>(codec);
    };
}

inline AppConfig usbConfig(const QString& model = QStringLiteral("ts990s"))
{
    AppConfig c;
    c.radioKind = QStringLiteral("usb");
    c.usbModel = model;
    c.usbInput = QStringLiteral("Test|USB Audio CODEC");
    c.deviceKey = QStringLiteral("Test|Speakers");
    c.host = QStringLiteral("192.0.2.1");  // TEST-NET: never a real radio
    c.fallbackPorts = {};
    return c;
}

// The sink the engine plays into, counting what it was given.
class CountingSink final : public AudioSink {
public:
    explicit CountingSink(std::shared_ptr<std::atomic<quint64>> frames) : m_frames(std::move(frames)) {}
    QString deviceName() const override { return QStringLiteral("Test speakers"); }
    void write(const float*, int frames) override { m_frames->fetch_add(quint64(frames)); }
    bool alive() const override { return true; }
    SinkStats stats() override { return {}; }
    void setGainDb(double) override {}
    void setBoost(bool) override {}
    void setMuted(bool) override {}
    void setTargetLatencyMs(int) override {}

private:
    std::shared_ptr<std::atomic<quint64>> m_frames;
};

} // namespace esa::testing
