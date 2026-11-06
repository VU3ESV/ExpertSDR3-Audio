#pragma once

#include "dsp/ParamSpec.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <functional>
#include <memory>

namespace esa {

// A noise-reduction method. Input and output are interleaved stereo float32 at
// the rate given to the factory. A call may return fewer or more frames than it
// was given, including none while a method's first block fills, so callers must
// not assume frames in == frames out.
class NrProcessor {
public:
    virtual ~NrProcessor() = default;
    virtual void process(const QByteArray& in, QByteArray& out) = 0;
    // A new stream: forget the noise profile.
    virtual void reset() = 0;
    // Same stream resuming (end of a transmission): clear transients but keep the
    // noise estimate where the method can, so the band noise does not jump back.
    virtual void resetTransient() { reset(); }
    virtual void setParam(const QString& key, double value) = 0;
};

// To add a new noise-reduction method: write an NrProcessor adapter in
// NrRegistry.cpp and add one entry to nrMethods(). The GUI, settings, command
// line and test suite all pick it up from that entry.
struct NrMethod {
    QString id;           // "rn2": the settings value and --nr argument
    QString label;        // "RN2 (RNNoise)"
    QString description;
    QList<ParamSpec> params;  // keys are "<id>.<name>"
    // May be slow (model load, FFT planning), so it runs off the audio path.
    // Returns null on failure. Only 24000 and 48000 are supported.
    std::function<std::unique_ptr<NrProcessor>(int sampleRate)> create;
};

// Compiled-in methods in display order. "Off" is implied and not listed.
const QList<NrMethod>& nrMethods();
const NrMethod* findNrMethod(const QString& id);

// Whether upstream's policy bypasses client NR for this TCI modulation. CW and
// digital signals are not speech: a neural speech model reads a steady carrier
// as noise, and AetherSDR measured a 28 dB hole where the signal was.
bool modeBypassesNr(const QString& modulation);

// DIGL and DIGU in a 2-channel stream carry a complex I/Q signal rather than
// left/right audio (TCI spec section 3.4), which sounds wrong played as stereo.
bool modeIsComplexStereo(const QString& modulation);

} // namespace esa
