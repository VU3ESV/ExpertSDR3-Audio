#pragma once

// PortAudio helpers shared by the output (PortAudioSink) and the input (PortAudioSource). For
// their .cpp files only: it includes PortAudio's own header.

#include <portaudio.h>

#include <QString>

namespace esa::audio::pa {

// PortAudio's library state is process-wide and reference counted, and it snapshots
// the device list when first initialised, so a rescan needs every holder to let go.
class Library {
public:
    Library() : m_error(Pa_Initialize()) {}
    ~Library()
    {
        if (m_error == paNoError) Pa_Terminate();
    }
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    bool ok() const { return m_error == paNoError; }
    QString errorText() const { return QString::fromUtf8(Pa_GetErrorText(m_error)); }

private:
    PaError m_error;
};

// "<host api>|<name>": stable across runs and reorderings.
inline QString deviceKeyFor(const PaDeviceInfo* info)
{
    const PaHostApiInfo* host = Pa_GetHostApiInfo(info->hostApi);
    return QStringLiteral("%1|%2").arg(host ? QString::fromUtf8(host->name) : QString(),
                                       QString::fromUtf8(info->name));
}

} // namespace esa::audio::pa
