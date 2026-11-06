#pragma once

#include <QFile>
#include <QString>

namespace esa {

// A minimal float32 stereo WAV writer for `--capture`, which records before and
// after the chain so NR methods can be compared and checked offline.
class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter() { close(); }

    bool open(const QString& path, int sampleRate);
    void write(const float* stereo, int frames);
    void close();

private:
    void writeHeader(quint32 dataBytes);

    QFile m_file;
    int m_sampleRate{0};
    quint64 m_dataBytes{0};
};

} // namespace esa
