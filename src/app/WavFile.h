#pragma once

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <memory>
#include <optional>

class QFileDevice;
class QIODevice;

namespace esa {

// A recording on disk: 32-bit float stereo WAV, so whatever the chain produced, even past
// full scale, is kept exactly.
//
// The header is 58 bytes: RIFF, WAVE, an 18-byte fmt chunk (format 3), a fact chunk
// holding the frame count, then data. It is always rewritten whole, in one write at
// offset 0, so it is never half updated. Writes are unbuffered: what write() accepted is
// already in the operating system's hands, so a crash keeps it and the header can be
// patched to exactly what is on the disk. A buffered QFile on a full disk left the header
// at data=0 over 40 MB of samples in a test on macOS; an unbuffered one did not.
//
// An instance writes one file and lives on the recorder thread. The static functions
// read and describe files and may be used on any thread.
class WavFile {
public:
    static constexpr int kHeaderBytes = 58;
    static constexpr int kChannels = 2;
    static constexpr int kBytesPerFrame = kChannels * int(sizeof(float));
    // The largest data chunk RIFF's 32-bit sizes can describe, in whole frames.
    static constexpr quint64 kMaxDataBytes =
        (0xFFFFFFFFull - (kHeaderBytes - 8)) / kBytesPerFrame * kBytesPerFrame;

    // What parse() found.
    struct Info {
        int sampleRate{0};
        qint64 dataOffset{0};       // where the first frame starts
        quint64 frames{0};          // frames to play: the header's count, limited to what the file holds
        quint64 declaredFrames{0};  // what the data chunk's size says
        quint64 framesInFile{0};    // whole frames actually present after dataOffset
    };

    // The header for `frames` frames at `sampleRate`, as written at offset 0.
    static QByteArray header(int sampleRate, quint64 frames);
    static void encodeHeader(char* out, int sampleRate, quint64 frames);  // out: kHeaderBytes

    // Reads a 32-bit float stereo WAV header and leaves `device` at the first frame. It walks
    // the chunks, so files from other writers read too. A header that is behind its data (the
    // writer was killed between patches) reads as what it declares; one that is ahead (the file
    // was cut short) reads as the whole frames present.
    static std::optional<Info> parse(QIODevice& device, QString* error = nullptr);
    static std::optional<Info> parseFile(const QString& path, QString* error = nullptr);

    // Replaces NaN and the infinities with 0 and leaves every other value bit for bit.
    static void sanitize(float* samples, size_t count);

    // Takes a device that is not open yet, normally a QFile. It is a QFileDevice rather than
    // any QIODevice because recovering from a short write needs resize(). Tests pass a QFile
    // subclass whose writes fail, which is how the disk-full path is covered on Windows.
    WavFile(std::unique_ptr<QFileDevice> device, int sampleRate);
    ~WavFile();  // finish()es a file still open
    WavFile(const WavFile&) = delete;
    WavFile& operator=(const WavFile&) = delete;

    enum class CreateResult { Created, NameTaken, Failed };
    // Creates the file, never replacing one (QIODevice::NewOnly), and writes a header for 0
    // frames. NameTaken means something already has that name; try another. On Failed nothing
    // is left behind and errorString() says why.
    CreateResult create();

    // Appends whole frames. Non-finite samples are written as 0, by cleaning `stereo` in place
    // (on a big-endian host it is also byte-swapped in place).
    // On a short write it keeps the whole frames that reached the disk: it truncates the file to
    // them, rewrites the header and closes. It returns false then, and errorString() says why.
    bool append(float* stereo, quint64 frames);

    // Rewrites the header to match the frames written so far. False, with the file closed and
    // left as it is, if that fails.
    bool patchHeader();

    // Patches the header and closes. False if the header could not be written.
    bool finish();

    // Closes the file, if open, and deletes it: for a recording that got no frames.
    void discard();

    bool isOpen() const;
    QString path() const { return m_path; }
    int sampleRate() const { return m_sampleRate; }
    quint64 frames() const { return m_dataBytes / quint64(kBytesPerFrame); }
    quint64 dataBytes() const { return m_dataBytes; }
    QString errorString() const { return m_error; }

private:
    bool writeHeader();

    std::unique_ptr<QFileDevice> m_device;
    QString m_path;
    int m_sampleRate;
    quint64 m_dataBytes{0};
    QString m_error;
};

} // namespace esa
