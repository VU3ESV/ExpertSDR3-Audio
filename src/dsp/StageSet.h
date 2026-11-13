#pragma once

#include "ClientComp.h"
#include "ClientEq.h"
#include "ClientGate.h"
#include "ClientPudu.h"
#include "ClientTube.h"

namespace esa {

// The five stages of the receive chain, kept together so the parameter table can
// address any set of them: the one RxChain processes, or the window's display
// mirror (LiveTaps::view), which is never processed.
//
// The vendored stages are neither copyable nor movable, so neither is this.
struct StageSet {
    AetherSDR::ClientGate gate;
    AetherSDR::ClientEq eq;
    AetherSDR::ClientComp comp;
    AetherSDR::ClientTube tube;
    AetherSDR::ClientPudu pudu;

    // Before the first process() and never during one.
    void prepare(int sampleRate)
    {
        gate.prepare(sampleRate);
        eq.prepare(sampleRate);
        comp.prepare(sampleRate);
        tube.prepare(sampleRate);
        pudu.prepare(sampleRate);
    }

    // Upstream's ten-band layout, all bands off, which is where every chain starts.
    void initialiseBands()
    {
        eq.setActiveBandCount(AetherSDR::ClientEq::kDefaultBandCount);
        for (int band = 0; band < AetherSDR::ClientEq::kDefaultBandCount; ++band) {
            eq.setBand(band, AetherSDR::ClientEq::defaultBand(band));
        }
    }
};

} // namespace esa
