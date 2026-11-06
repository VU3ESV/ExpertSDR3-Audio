#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

// Pure TCI 2.1 wire format: no sockets and no state, so it is testable alone.
namespace esa::tci {

// A command as received. `name` is lower-cased (the protocol is case-insensitive).
struct Command {
    QString name;
    QStringList args;
};

// One WebSocket text frame can carry several `name:a,b;` commands.
QList<Command> parseCommands(const QString& text);

// Spec section 4.7: true if "true" or "1" in any case; anything else is false.
bool parseBool(const QString& value);

enum class StreamType : quint32 {
    Iq = 0,
    RxAudio = 1,
    TxAudio = 2,
    TxChrono = 3,
    LineOut = 4,
};

enum class SampleType : quint32 {
    Int16 = 0,
    Int24 = 1,
    Int32 = 2,
    Float32 = 3,
};

inline constexpr int kHeaderBytes = 64;

struct StreamHeader {
    quint32 receiver{0};
    quint32 sampleRate{0};
    quint32 format{0};
    quint32 codec{0};
    quint32 crc{0};
    // Real samples in the payload, NOT frames. ExpertSDR3 reports 4096 for its
    // default stereo block (2048 frames) and exactly N after
    // AUDIO_STREAM_SAMPLES:N, so frames are always length / channels.
    quint32 length{0};
    quint32 type{0};
    quint32 channels{0};
    quint32 count{0};
};

// nullopt if the frame is too short to hold a header.
std::optional<StreamHeader> parseHeader(const QByteArray& frame);

// Decodes a block to interleaved stereo float32. Mono is duplicated to both
// channels. Returns the frame count, or -1 if the block is malformed: unknown
// format, a compressed codec, a channel count other than 1 or 2, or a length
// claiming more samples than the payload holds.
int decodeToStereo(const StreamHeader& header, const char* payload, qsizetype payloadBytes,
                   std::vector<float>& out);

} // namespace esa::tci
