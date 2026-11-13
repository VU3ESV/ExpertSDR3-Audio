#include "app/WavFile.h"

#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <climits>
#include <cstring>

namespace esa {

namespace {

constexpr quint16 kFormatFloat = 3;          // WAVE_FORMAT_IEEE_FLOAT
constexpr quint16 kFormatExtensible = 0xFFFE;
constexpr quint32 kFmtChunkBytes = 18;
constexpr quint32 kStreamingSize = 0xFFFFFFFFu;  // what writers that cannot seek leave behind

// WAV is little-endian. Every platform this ships on is too, so this compiles to nothing.
void toLittleEndianInPlace(float* samples, size_t count)
{
    if constexpr (std::endian::native == std::endian::big) {
        for (size_t i = 0; i < count; ++i) {
            quint32 bits;
            std::memcpy(&bits, &samples[i], 4);
            bits = qToLittleEndian(bits);
            std::memcpy(&samples[i], &bits, 4);
        }
    } else {
        (void)samples;
        (void)count;
    }
}

// Writes all of `bytes`, offering the rest again after a short count. A short count is not yet
// an error, and Qt reports a write that stopped partway without saying why; the retry fails
// with the reason (disk full, file too large, device gone). Returns the bytes written.
qint64 writeAll(QIODevice& device, const char* data, qint64 bytes)
{
    qint64 done = 0;
    while (done < bytes) {
        const qint64 n = device.write(data + done, bytes - done);
        if (n <= 0) break;
        done += n;
    }
    return done;
}

} // namespace

QByteArray WavFile::header(int sampleRate, quint64 frames)
{
    QByteArray out(kHeaderBytes, '\0');
    encodeHeader(out.data(), sampleRate, frames);
    return out;
}

void WavFile::encodeHeader(char* b, int sampleRate, quint64 frames)
{
    const auto dataBytes = quint32(std::min(frames, kMaxDataBytes / kBytesPerFrame) * kBytesPerFrame);
    const auto rate = quint32(std::max(sampleRate, 0));
    std::memcpy(b, "RIFF", 4);
    qToLittleEndian<quint32>(quint32(kHeaderBytes - 8) + dataBytes, b + 4);
    std::memcpy(b + 8, "WAVEfmt ", 8);
    qToLittleEndian<quint32>(kFmtChunkBytes, b + 16);
    qToLittleEndian<quint16>(kFormatFloat, b + 20);
    qToLittleEndian<quint16>(kChannels, b + 22);
    qToLittleEndian<quint32>(rate, b + 24);
    qToLittleEndian<quint32>(rate * kBytesPerFrame, b + 28);  // bytes per second
    qToLittleEndian<quint16>(kBytesPerFrame, b + 32);         // block align
    qToLittleEndian<quint16>(32, b + 34);                     // bits per sample
    qToLittleEndian<quint16>(0, b + 36);                      // cbSize: no extension
    // Non-PCM formats should carry a fact chunk with the length in frames.
    std::memcpy(b + 38, "fact", 4);
    qToLittleEndian<quint32>(4, b + 42);
    qToLittleEndian<quint32>(dataBytes / kBytesPerFrame, b + 46);
    std::memcpy(b + 50, "data", 4);
    qToLittleEndian<quint32>(dataBytes, b + 54);
}

std::optional<WavFile::Info> WavFile::parse(QIODevice& device, QString* error)
{
    const auto refuse = [error](const QString& why) -> std::optional<Info> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (!device.isOpen() || !device.isReadable()) return refuse(QStringLiteral("The file is not open for reading"));
    if (device.isSequential()) return refuse(QStringLiteral("The file cannot be read at random"));

    const qint64 size = device.size();
    char riff[12];
    if (!device.seek(0) || device.read(riff, 12) != 12) return refuse(QStringLiteral("Too short to be a WAV file"));
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0)
        return refuse(QStringLiteral("Not a WAV file"));

    Info info;
    bool haveFormat = false;
    qint64 pos = 12;
    // The RIFF size is not trusted: a crash can leave it behind the data, and only the data
    // chunk's own size, from the same header write, matters.
    while (pos + 8 <= size) {
        char chunk[8];
        if (!device.seek(pos) || device.read(chunk, 8) != 8) break;
        const quint32 chunkBytes = qFromLittleEndian<quint32>(chunk + 4);
        pos += 8;

        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (chunkBytes < 16) return refuse(QStringLiteral("The format chunk is too short"));
            char fmt[40] = {};
            const qint64 want = std::min<qint64>(chunkBytes, 40);
            if (device.read(fmt, want) != want) return refuse(QStringLiteral("The format chunk is cut short"));
            quint16 format = qFromLittleEndian<quint16>(fmt);
            const quint16 channels = qFromLittleEndian<quint16>(fmt + 2);
            const quint32 rate = qFromLittleEndian<quint32>(fmt + 4);
            const quint16 blockAlign = qFromLittleEndian<quint16>(fmt + 12);
            const quint16 bits = qFromLittleEndian<quint16>(fmt + 14);
            // WAVE_FORMAT_EXTENSIBLE: the sub-format GUID starts with the real format code.
            if (format == kFormatExtensible && chunkBytes >= 40) format = qFromLittleEndian<quint16>(fmt + 24);
            if (format != kFormatFloat)
                return refuse(QStringLiteral("Not a 32-bit float recording (format %1)").arg(format));
            if (channels != kChannels) return refuse(QStringLiteral("Not stereo (%1 channels)").arg(channels));
            if (bits != 32 || blockAlign != kBytesPerFrame)
                return refuse(QStringLiteral("Not 32-bit samples (%1 bits)").arg(bits));
            if (rate == 0 || rate > quint32(INT_MAX)) return refuse(QStringLiteral("Bad sample rate %1").arg(rate));
            info.sampleRate = int(rate);
            haveFormat = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            if (!haveFormat) return refuse(QStringLiteral("The data comes before the format chunk"));
            info.dataOffset = pos;
            info.framesInFile = size > pos ? quint64(size - pos) / kBytesPerFrame : 0;
            info.declaredFrames = chunkBytes / kBytesPerFrame;
            info.frames = chunkBytes == kStreamingSize ? info.framesInFile
                                                       : std::min(info.declaredFrames, info.framesInFile);
            if (!device.seek(pos)) return refuse(device.errorString());
            return info;
        }
        pos += qint64(chunkBytes) + (chunkBytes & 1);  // chunks are padded to even sizes
    }
    return refuse(haveFormat ? QStringLiteral("No data chunk") : QStringLiteral("No format chunk"));
}

std::optional<WavFile::Info> WavFile::parseFile(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return std::nullopt;
    }
    return parse(file, error);
}

void WavFile::sanitize(float* samples, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        quint32 bits;
        std::memcpy(&bits, &samples[i], 4);
        // All exponent bits set is NaN or an infinity. Checked on the bits, so it holds under
        // any floating-point compiler flags.
        if ((bits & 0x7F800000u) == 0x7F800000u) samples[i] = 0.0f;
    }
}

WavFile::WavFile(std::unique_ptr<QFileDevice> device, int sampleRate)
    : m_device(std::move(device)),
      m_path(m_device ? m_device->fileName() : QString()),
      m_sampleRate(sampleRate)
{
}

WavFile::~WavFile()
{
    if (isOpen()) finish();
}

bool WavFile::isOpen() const
{
    return m_device && m_device->isOpen();
}

WavFile::CreateResult WavFile::create()
{
    m_error.clear();
    m_dataBytes = 0;
    if (!m_device || m_sampleRate <= 0) {
        m_error = QStringLiteral("No file to write to");
        return CreateResult::Failed;
    }
    if (!m_device->open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Unbuffered)) {
        const QFileInfo existing(m_path);
        if (existing.exists() || existing.isSymLink()) return CreateResult::NameTaken;
        m_error = m_device->errorString();
        return CreateResult::Failed;
    }
    if (!writeHeader()) {
        // Not even the header fits. Leave nothing behind.
        const QString why = m_error;
        m_device->close();
        QFile::remove(m_path);
        m_error = why;
        return CreateResult::Failed;
    }
    return CreateResult::Created;
}

bool WavFile::writeHeader()
{
    char h[kHeaderBytes];
    encodeHeader(h, m_sampleRate, frames());
    // One write, so the header on disk is either the old one or the new one. Rewriting bytes
    // the file already has normally needs no room, but on a copy-on-write filesystem (APFS
    // with snapshots, Btrfs, ZFS) it can still fail when the disk is full. The file then
    // declares the frames of its last patch, about a second behind what it holds.
    if (!m_device->seek(0) || writeAll(*m_device, h, kHeaderBytes) != kHeaderBytes
        || !m_device->seek(kHeaderBytes + qint64(m_dataBytes))) {
        m_error = m_device->errorString();
        return false;
    }
    return true;
}

bool WavFile::append(float* stereo, quint64 frames)
{
    if (!isOpen()) {
        if (m_error.isEmpty()) m_error = QStringLiteral("The file is not open");
        return false;
    }
    if (frames == 0) return true;

    // The recorder rolls to a new part long before this; it is only a backstop.
    const quint64 room = (kMaxDataBytes - m_dataBytes) / kBytesPerFrame;
    const quint64 take = std::min(frames, room);
    sanitize(stereo, size_t(take) * kChannels);
    toLittleEndianInPlace(stereo, size_t(take) * kChannels);

    const qint64 bytes = qint64(take) * kBytesPerFrame;
    const qint64 written = writeAll(*m_device, reinterpret_cast<const char*>(stereo), bytes);
    m_dataBytes += quint64(written);

    if (written == bytes) {
        if (take == frames) return true;
        finish();
        m_error = QStringLiteral("The file reached the 4 GiB limit of WAV");
        return false;
    }

    // Keep the whole frames that reached the disk and make the header say exactly that, so the
    // file stays valid and playable.
    QString why = m_device->errorString();
    if (why.isEmpty()) why = QStringLiteral("Write failed");
    m_dataBytes -= m_dataBytes % kBytesPerFrame;
    const bool truncated = m_device->resize(kHeaderBytes + qint64(m_dataBytes));
    const bool patched = writeHeader();
    m_device->close();
    m_error = why;
    if (!truncated || !patched) m_error += QStringLiteral("; the file could not be tidied up afterwards");
    return false;
}

bool WavFile::patchHeader()
{
    if (!isOpen()) return false;
    if (writeHeader()) return true;
    const QString why = m_error;
    m_device->close();
    m_error = why;
    return false;
}

bool WavFile::finish()
{
    if (!isOpen()) return true;
    const bool ok = writeHeader();
    m_device->close();
    return ok;
}

void WavFile::discard()
{
    if (isOpen()) m_device->close();
    if (!m_path.isEmpty()) QFile::remove(m_path);
    m_dataBytes = 0;
}

} // namespace esa
