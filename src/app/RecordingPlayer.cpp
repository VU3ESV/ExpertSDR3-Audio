#include "app/RecordingPlayer.h"

#include <QtEndian>

#include <bit>
#include <cstring>

namespace esa {

namespace {

// Up to 0.34 s at 48 kHz per read: few syscalls, and a small fixed buffer.
constexpr size_t kChunkFrames = 16384;

void fromLittleEndianInPlace(float* samples, size_t count)
{
    if constexpr (std::endian::native == std::endian::big) {
        for (size_t i = 0; i < count; ++i) {
            quint32 bits;
            std::memcpy(&bits, &samples[i], 4);
            bits = qFromLittleEndian(bits);
            std::memcpy(&samples[i], &bits, 4);
        }
    } else {
        (void)samples;
        (void)count;
    }
}

} // namespace

RecordingPlayer::RecordingPlayer(std::shared_ptr<PlaySession> session, int expectedRate)
    : m_session(std::move(session)), m_expectedRate(expectedRate)
{
}

bool RecordingPlayer::fail(const QString& reason)
{
    m_file.close();
    if (!m_session->failed()) {
        m_session->m_failureReason = reason;
        m_session->m_failed.store(true, std::memory_order_release);
    }
    return false;
}

bool RecordingPlayer::open()
{
    m_file.setFileName(m_session->path());
    if (!m_file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Could not open %1: %2").arg(m_session->path(), m_file.errorString()));
    QString error;
    const std::optional<WavFile::Info> info = WavFile::parse(m_file, &error);
    if (!info) return fail(QStringLiteral("Could not play %1: %2").arg(m_session->path(), error));
    if (m_expectedRate > 0 && info->sampleRate != m_expectedRate) {
        return fail(QStringLiteral("This recording is at %1 Hz, but the output runs at %2 Hz")
                        .arg(info->sampleRate)
                        .arg(m_expectedRate));
    }

    m_total = info->frames;
    m_scratch.assign(kChunkFrames * WavFile::kChannels, 0.0f);
    m_session->m_sampleRate = info->sampleRate;
    m_session->m_totalFrames = m_total;
    fill();
    if (m_session->failed()) return false;
    m_session->m_ready.store(true, std::memory_order_release);
    return true;
}

bool RecordingPlayer::fill()
{
    PlaySession& s = *m_session;
    if (s.failed() || s.atEnd()) return false;
    if (s.cancelled()) {
        m_file.close();
        return false;
    }

    SpscStereoRing& ring = s.ring();
    while (m_queued < m_total && !s.cancelled()) {
        // Only the engine empties the ring, so the free space can only grow meanwhile.
        const size_t space = ring.capacity() - ring.size();
        const size_t want = size_t(std::min<quint64>({m_total - m_queued, quint64(space), quint64(kChunkFrames)}));
        if (want == 0) break;

        const qint64 bytes = qint64(want) * WavFile::kBytesPerFrame;
        const qint64 got = m_file.read(reinterpret_cast<char*>(m_scratch.data()), bytes);
        if (got < 0) return fail(QStringLiteral("Could not read %1: %2").arg(s.path(), m_file.errorString()));
        const size_t frames = size_t(got) / WavFile::kBytesPerFrame;
        if (frames == 0) return fail(QStringLiteral("%1 ended before its last frame").arg(s.path()));
        // A short read that splits a frame leaves the rest for the next read.
        if (qint64(frames) * WavFile::kBytesPerFrame != got)
            m_file.seek(m_file.pos() - (got - qint64(frames) * WavFile::kBytesPerFrame));

        fromLittleEndianInPlace(m_scratch.data(), frames * WavFile::kChannels);
        ring.write(m_scratch.data(), frames);
        m_queued += frames;
        s.m_queued.store(m_queued, std::memory_order_release);
    }

    if (m_queued >= m_total) {
        m_file.close();
        s.m_atEnd.store(true, std::memory_order_release);
        return false;
    }
    if (s.cancelled()) {
        m_file.close();
        return false;
    }
    return true;
}

} // namespace esa
