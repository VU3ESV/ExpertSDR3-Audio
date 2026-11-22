#pragma once

#include <QtGlobal>

#include <functional>

namespace esa {

// Decides when to send MUTE to ExpertSDR3 so that its own audio output is silent
// while this app is playing and is left exactly as it was found afterwards.
//
// Measured on ExpertSDR3 2.1: MUTE, RX_MUTE, VOLUME and RX_VOLUME do not change
// the TCI RX audio stream, so muting cannot starve this app of its input.
//
// The rules that make this more than a flag:
//  - Never undo a mute we did not set. If the operator had it muted first, it
//    stays muted when we finish.
//  - The operator outranks us (spec 3.5). If they unmute while we hold it, we
//    stop holding and do not fight them until the feature is re-armed. Note that
//    moving ExpertSDR3's volume also unmutes: any VOLUME set clears MUTE on the
//    server, which the spec does not mention. For the same reason this app must
//    never send VOLUME, or it would silently undo its own mute.
//  - A set can be dropped silently: the server ignores a set within 200 ms of
//    another client's change and replies with the current value (spec 4.7). So
//    a reply contradicting a set we just sent means "retry", not "operator
//    override". Only a change arriving once we hold the mute is an override.
//  - Ownership outlives the connection and the process. It is persisted, so
//    reconnecting does not mistake our own mute for the operator's, and a crash
//    does not leave ExpertSDR3 silently muted for good.
class MuteController {
public:
    enum class Phase {
        Idle,              // not holding anything
        Engaging,          // sent MUTE:true, waiting for it to take
        Holding,           // ExpertSDR3 is muted and we are responsible for it
        OperatorOverride,  // operator unmuted while we held it; hands off
        Restoring,         // sent MUTE:false, waiting for it to take
        Failed,            // the server never honoured our set
    };

    struct Ownership {
        bool owned{false};       // we set the mute and still owe a restore
        bool priorMuted{false};  // what the server reported before we touched it
        int priorLevel{-1};      // a level control's level before we turned it down
    };

    struct Actions {
        std::function<void(bool muted)> sendMute;
        // Instead of sendMute, for a speaker that is a level (the TS-990S's AF): 0 to silence it,
        // and the level it had to give it back. A level never read is never sent.
        std::function<void(int level)> sendLevel;
        std::function<void(const Ownership&)> persist;
    };

    static constexpr qint64 kRetryMs = 300;  // longer than the 200 ms monopoly window
    static constexpr int kMaxAttempts = 10;

    MuteController(Actions actions, Ownership restored);

    // True while ExpertSDR3 should be silent. Safe to call repeatedly.
    void setWanted(bool wanted, qint64 nowMs);

    void onConnected();
    void onDisconnected();
    // Every MUTE state report from the server: init burst, echoes and changes
    // made by the operator or another client.
    void onServerMute(bool muted, qint64 nowMs);
    // A level control's every reading: 0 counts as muted. The first on a connection is what
    // makes the speaker known, so nothing is ever sent before one.
    void onServerLevel(int level, qint64 nowMs);
    // A level control's operator override lasts until this: the operator choosing the feature
    // again, or a new session. A quiet spell that stops this app's audio is no such choice.
    void rearm(qint64 nowMs);
    // Drives retries; call a few times a second.
    void tick(qint64 nowMs);

    Phase phase() const { return m_phase; }
    const Ownership& ownership() const { return m_ownership; }
    bool wanted() const { return m_wanted; }
    // Nothing outstanding: safe to close the connection without losing a restore.
    bool settled() const;

private:
    void evaluate(qint64 nowMs);
    void beginEngage(qint64 nowMs);
    void beginRelease(qint64 nowMs);
    void send(bool muted, qint64 nowMs);
    void setOwnership(bool owned, bool priorMuted, int priorLevel = -1);

    Actions m_actions;
    Ownership m_ownership;
    Phase m_phase{Phase::Idle};
    bool m_wanted{false};
    bool m_connected{false};
    bool m_serverKnown{false};
    bool m_serverMuted{false};
    int m_serverLevel{-1};
    qint64 m_lastSendMs{0};
    int m_attempts{0};
};

} // namespace esa
