#pragma once

#include "audio/SpscRing.h"
#include "dsp/StageSet.h"

#include <QtGlobal>

#include <atomic>

namespace esa {

// Live data the engine shares with the window, without locks. The Engine creates one
// and keeps it for life; whoever displays it holds a shared_ptr, because the Engine
// can be deleted while the window still exists.
//
// One writer per piece of data: the window writes view's parameters and the wanted
// flags; the engine thread writes view's meters (copyMeteringFrom only), produces into
// the rings and stores the rest. The window is each ring's only consumer, and before
// setting a wanted flag it skips what is already queued, so it starts from fresh data.
// Nothing here is needed by a headless run, which simply never sets a wanted flag.
struct LiveTaps {
    LiveTaps()
    {
        view.prepare(48000);
        view.initialiseBands();
    }

    // What the vendored panels draw from and write into. Never processed.
    StageSet view;

    // Post-EQ samples for the EQ's analyser, written by RxChain::process.
    SpscStereoRing eqTap{8192};
    std::atomic<bool> eqWanted{false};

    // Post-chain samples before volume, boost and mute, for the output meter and scope.
    SpscStereoRing outTap{24000};
    std::atomic<bool> outWanted{false};

    std::atomic<float> inputPeak{0.0f};  // engine: max per block; window: exchange(0)
    std::atomic<float> nrGain{1.0f};     // RxChain::nrGain() of the latest block
    std::atomic<bool> nrActive{false};
    std::atomic<int> chainRate{48000};   // the rate the chain was last built at
    std::atomic<quint64> blocks{0};      // published blocks; stops advancing when audio stops
    // Blocks handed to the speakers and outTap: the chain's, or PLAY's while it plays, with or
    // without a radio. Stops advancing when nothing plays, so the output meter and scope go by
    // this one: `blocks` stands still through a PLAY with no radio.
    std::atomic<quint64> outBlocks{0};
};

// These must be plain loads and stores on the audio path. Where a 64-bit atomic would
// take a lock instead, as it can on 32-bit ARM, the build stops here rather than
// letting the lock in quietly.
static_assert(std::atomic<float>::is_always_lock_free && std::atomic<quint64>::is_always_lock_free);

} // namespace esa
