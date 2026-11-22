#pragma once

#include <QString>

#include <cstddef>

namespace esa {

struct SourceStats {
    int deviceRate{0};
    int deviceChannels{0};
    quint64 callbacks{0};      // how often the device has delivered: progress shows it is alive
    quint64 framesCaptured{0};
    quint64 overflows{0};      // times audio was lost: the device's own overflows, or a ring left full
};

// Where a radio's audio comes in through a sound device, such as a radio's own USB codec.
// The device delivers on its own thread; read() is called on the engine thread and never
// blocks.
class AudioSource {
public:
    virtual ~AudioSource() = default;
    virtual QString deviceName() const = 0;
    virtual int sampleRate() const = 0;
    // Takes up to `frames` interleaved stereo frames captured so far. A mono device's one
    // channel arrives in both.
    virtual size_t read(float* stereo, size_t frames) = 0;
    virtual size_t available() const = 0;
    // False once the stream has failed. A device that stops delivering without saying so is
    // caught by its callbacks no longer advancing (SourceStats::callbacks).
    virtual bool alive() const = 0;
    virtual SourceStats stats() const = 0;
};

} // namespace esa
