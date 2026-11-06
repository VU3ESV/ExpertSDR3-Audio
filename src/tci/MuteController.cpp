#include "tci/MuteController.h"

namespace esa {

MuteController::MuteController(Actions actions, Ownership restored)
    : m_actions(std::move(actions)), m_ownership(restored)
{
}

bool MuteController::settled() const
{
    return m_phase != Phase::Engaging && m_phase != Phase::Restoring;
}

void MuteController::setOwnership(bool owned, bool priorMuted)
{
    if (m_ownership.owned == owned && m_ownership.priorMuted == priorMuted) return;
    m_ownership = {owned, priorMuted};
    if (m_actions.persist) m_actions.persist(m_ownership);
}

void MuteController::send(bool muted, qint64 nowMs)
{
    m_lastSendMs = nowMs;
    ++m_attempts;
    if (m_actions.sendMute) m_actions.sendMute(muted);
}

void MuteController::setWanted(bool wanted, qint64 nowMs)
{
    if (wanted == m_wanted) return;
    m_wanted = wanted;
    evaluate(nowMs);
}

void MuteController::onConnected()
{
    m_connected = true;
    m_serverKnown = false;  // unknown until the init burst reports it
}

void MuteController::onDisconnected()
{
    m_connected = false;
    m_serverKnown = false;
    // The phases that describe the live connection end with it. Ownership and an
    // operator override both survive: a network blip is not a new decision.
    if (m_phase == Phase::Engaging || m_phase == Phase::Holding
        || m_phase == Phase::Restoring || m_phase == Phase::Failed) {
        m_phase = Phase::Idle;
    }
}

void MuteController::beginEngage(qint64 nowMs)
{
    m_attempts = 0;
    // Re-arming mid-restore: our MUTE:false is in flight, so m_serverMuted is
    // about to go stale. Holding now would read that echo as the operator
    // unmuting, so assert the mute and wait for it to be confirmed instead.
    if (m_serverMuted && m_phase != Phase::Restoring) {
        // Already silent, so nothing to send. If we own it (reconnect or crash
        // recovery) the recorded prior state still stands. If we do not, it is
        // the operator's mute and we must leave it set when we finish.
        m_phase = Phase::Holding;
        return;
    }
    // Keep an existing prior: on re-engage after a reconnect the server may have
    // restarted unmuted, and the first prior is still the operator's real state.
    if (!m_ownership.owned) setOwnership(true, false);
    m_phase = Phase::Engaging;
    send(true, nowMs);
}

void MuteController::beginRelease(qint64 nowMs)
{
    // Disarming mid-engage: our MUTE:true is in flight and may still land, so it
    // must be undone even though the server has not reported it yet.
    const bool setInFlight = m_phase == Phase::Engaging;
    m_attempts = 0;
    if (m_ownership.owned && !m_ownership.priorMuted && (m_serverMuted || setInFlight)) {
        m_phase = Phase::Restoring;
        send(false, nowMs);
        return;
    }
    setOwnership(false, false);
    m_phase = Phase::Idle;
}

void MuteController::evaluate(qint64 nowMs)
{
    if (!m_connected || !m_serverKnown) return;

    switch (m_phase) {
    case Phase::Idle:
        if (m_wanted) {
            beginEngage(nowMs);
        } else if (m_ownership.owned) {
            beginRelease(nowMs);  // restore owed from a previous connection or a crash
        }
        break;
    case Phase::Engaging:
    case Phase::Holding:
        if (!m_wanted) beginRelease(nowMs);
        break;
    case Phase::Restoring:
        if (m_wanted) beginEngage(nowMs);
        break;
    case Phase::OperatorOverride:
    case Phase::Failed:
        if (!m_wanted) m_phase = Phase::Idle;
        break;
    }
}

void MuteController::onServerMute(bool muted, qint64 nowMs)
{
    m_serverKnown = true;
    m_serverMuted = muted;

    switch (m_phase) {
    case Phase::Engaging:
        if (muted) m_phase = Phase::Holding;
        // A contradicting reply here means our set was dropped; tick() retries.
        break;
    case Phase::Holding:
        if (!muted) {
            setOwnership(false, false);
            m_phase = Phase::OperatorOverride;
        }
        break;
    case Phase::Restoring:
        if (!muted) {
            setOwnership(false, false);
            m_phase = Phase::Idle;
        }
        break;
    case Phase::Idle:
    case Phase::OperatorOverride:
    case Phase::Failed:
        break;
    }
    evaluate(nowMs);
}

void MuteController::tick(qint64 nowMs)
{
    if (!m_connected || (m_phase != Phase::Engaging && m_phase != Phase::Restoring)) return;
    if (nowMs - m_lastSendMs < kRetryMs) return;

    if (m_attempts >= kMaxAttempts) {
        // A failed engage changed nothing, so nothing is owed. A failed restore
        // keeps ownership: the next session tries again rather than forgetting
        // that ExpertSDR3 was left muted on our account.
        if (m_phase == Phase::Engaging) setOwnership(false, false);
        m_phase = Phase::Failed;
        return;
    }
    send(m_phase == Phase::Engaging, nowMs);
}

} // namespace esa
