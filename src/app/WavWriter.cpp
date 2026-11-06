#include "app/WavWriter.h"

#include <QtEndian>

namespace esa {

namespace {
void put16(QFile& f, quint16 v) { const quint16 le = qToLittleEndian(v); f.write(reinterpret_cast<const char*>(&le), 2); }
void put32(QFile& f, quint32 v) { const quint32 le = qToLittleEndian(v); f.write(reinterpret_cast<const char*>(&le), 4); }
}

bool WavWriter::open(const QString& path, int sampleRate)
{
    close();
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    m_sampleRate = sampleRate;
    m_dataBytes = 0;
    writeHeader(0);  // sizes are patched in close()
    return true;
}

void WavWriter::writeHeader(quint32 dataBytes)
{
    constexpr quint16 kFormatFloat = 3;
    constexpr quint16 kChannels = 2;
    constexpr quint16 kBits = 32;
    const quint32 byteRate = quint32(m_sampleRate) * kChannels * (kBits / 8);

    m_file.seek(0);
    m_file.write("RIFF", 4);
    put32(m_file, 36 + dataBytes);
    m_file.write("WAVEfmt ", 8);
    put32(m_file, 16);
    put16(m_file, kFormatFloat);
    put16(m_file, kChannels);
    put32(m_file, quint32(m_sampleRate));
    put32(m_file, byteRate);
    put16(m_file, kChannels * (kBits / 8));
    put16(m_file, kBits);
    m_file.write("data", 4);
    put32(m_file, dataBytes);
}

void WavWriter::write(const float* stereo, int frames)
{
    if (!m_file.isOpen() || frames <= 0) return;
    const qint64 bytes = qint64(frames) * 2 * qint64(sizeof(float));
    m_file.write(reinterpret_cast<const char*>(stereo), bytes);
    m_dataBytes += quint64(bytes);
}

void WavWriter::close()
{
    if (!m_file.isOpen()) return;
    writeHeader(quint32(qMin<quint64>(m_dataBytes, 0xFFFFFFFFu - 36)));
    m_file.close();
}

} // namespace esa
