#include "tci/TciProtocol.h"

#include <QtEndian>

#include <cstring>

namespace esa::tci {

QList<Command> parseCommands(const QString& text)
{
    QList<Command> commands;
    const QStringList parts = text.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& raw : parts) {
        const QString part = raw.trimmed();
        if (part.isEmpty()) continue;

        Command command;
        const qsizetype colon = part.indexOf(QLatin1Char(':'));
        if (colon < 0) {
            command.name = part.toLower();
        } else {
            command.name = part.left(colon).trimmed().toLower();
            command.args = part.mid(colon + 1).split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (QString& arg : command.args) arg = arg.trimmed();
        }
        commands.append(command);
    }
    return commands;
}

bool parseBool(const QString& value)
{
    return value.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0
        || value == QLatin1String("1");
}

std::optional<StreamHeader> parseHeader(const QByteArray& frame)
{
    if (frame.size() < kHeaderBytes) return std::nullopt;
    const auto word = [&](int index) {
        return qFromLittleEndian<quint32>(frame.constData() + index * 4);
    };
    StreamHeader h;
    h.receiver = word(0);
    h.sampleRate = word(1);
    h.format = word(2);
    h.codec = word(3);
    h.crc = word(4);
    h.length = word(5);
    h.type = word(6);
    h.channels = word(7);
    h.count = word(8);
    return h;
}

namespace {

// Read one little-endian sample of each supported format as float in [-1, 1).
float readSample(SampleType type, const char* p)
{
    switch (type) {
    case SampleType::Int16:
        return static_cast<float>(qFromLittleEndian<qint16>(p)) / 32768.0f;
    case SampleType::Int24: {
        const auto* b = reinterpret_cast<const unsigned char*>(p);
        // Assemble in the top three bytes so the arithmetic shift sign-extends.
        const auto raw = static_cast<qint32>((quint32(b[2]) << 24) | (quint32(b[1]) << 16)
                                             | (quint32(b[0]) << 8));
        return static_cast<float>(raw >> 8) / 8388608.0f;
    }
    case SampleType::Int32:
        return static_cast<float>(static_cast<double>(qFromLittleEndian<qint32>(p)) / 2147483648.0);
    case SampleType::Float32: {
        const quint32 bits = qFromLittleEndian<quint32>(p);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    }
    return 0.0f;
}

int bytesPerSample(quint32 format)
{
    switch (static_cast<SampleType>(format)) {
    case SampleType::Int16: return 2;
    case SampleType::Int24: return 3;
    case SampleType::Int32: return 4;
    case SampleType::Float32: return 4;
    }
    return 0;
}

} // namespace

int decodeToStereo(const StreamHeader& header, const char* payload, qsizetype payloadBytes,
                   std::vector<float>& out)
{
    const int width = bytesPerSample(header.format);
    if (width == 0 || header.codec != 0) return -1;
    if (header.channels != 1 && header.channels != 2) return -1;
    // Compare in 64 bits: a hostile or corrupt length must not wrap past this check.
    if (static_cast<quint64>(header.length) * static_cast<quint64>(width)
        > static_cast<quint64>(payloadBytes < 0 ? 0 : payloadBytes)) {
        return -1;
    }

    const int channels = static_cast<int>(header.channels);
    const int frames = static_cast<int>(header.length) / channels;
    out.resize(static_cast<size_t>(frames) * 2);

    const auto type = static_cast<SampleType>(header.format);
    const char* p = payload;
    for (int i = 0; i < frames; ++i) {
        const float left = readSample(type, p);
        p += width;
        float right = left;
        if (channels == 2) {
            right = readSample(type, p);
            p += width;
        }
        out[static_cast<size_t>(i) * 2] = left;
        out[static_cast<size_t>(i) * 2 + 1] = right;
    }
    return frames;
}

} // namespace esa::tci
