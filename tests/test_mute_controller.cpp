#include "tci/MuteController.h"

#include <QtTest>

#include <vector>

using esa::MuteController;
using Phase = MuteController::Phase;

namespace {

// Drives the controller against a server whose responses the test controls.
struct Harness {
    std::vector<bool> sent;
    MuteController::Ownership persisted;
    MuteController controller;
    qint64 now{1000};

    explicit Harness(MuteController::Ownership restored = {})
        : persisted(restored),
          controller({[this](bool muted) { sent.push_back(muted); },
                      [this](const MuteController::Ownership& o) { persisted = o; }},
                     restored) {}

    void connectWithServerMuted(bool muted)
    {
        controller.onConnected();
        controller.onServerMute(muted, now);
    }
    void wanted(bool value) { controller.setWanted(value, now); }
    void server(bool muted) { controller.onServerMute(muted, now); }
    void advance(qint64 ms)
    {
        now += ms;
        controller.tick(now);
    }
};

} // namespace

class TestMuteController : public QObject {
    Q_OBJECT

private slots:
    void mutesAnUnmutedServerAndRestoresItAfterwards()
    {
        Harness h;
        h.connectWithServerMuted(false);
        h.wanted(true);
        QCOMPARE(h.sent, (std::vector<bool>{true}));
        QCOMPARE(h.controller.phase(), Phase::Engaging);
        QVERIFY(h.persisted.owned);
        QVERIFY(!h.persisted.priorMuted);

        h.server(true);
        QCOMPARE(h.controller.phase(), Phase::Holding);

        h.wanted(false);
        QCOMPARE(h.sent, (std::vector<bool>{true, false}));
        QCOMPARE(h.controller.phase(), Phase::Restoring);
        QVERIFY(!h.controller.settled());

        h.server(false);
        QCOMPARE(h.controller.phase(), Phase::Idle);
        QVERIFY(h.controller.settled());
        QVERIFY(!h.persisted.owned);
    }

    // This is how the radio on the bench was found: already muted.
    void neverUnmutesAMuteItDidNotSet()
    {
        Harness h;
        h.connectWithServerMuted(true);
        h.wanted(true);
        QVERIFY(h.sent.empty());
        QCOMPARE(h.controller.phase(), Phase::Holding);
        QVERIFY(!h.persisted.owned);

        h.wanted(false);
        QVERIFY(h.sent.empty());
        QCOMPARE(h.controller.phase(), Phase::Idle);
    }

    void doesNothingUntilTheServerStateIsKnown()
    {
        Harness h;
        h.controller.onConnected();
        h.wanted(true);
        QVERIFY(h.sent.empty());
        h.server(false);  // the init burst reports it
        QCOMPARE(h.sent, (std::vector<bool>{true}));
    }

    void operatorUnmuteWinsAndIsNotFought()
    {
        Harness h;
        h.connectWithServerMuted(false);
        h.wanted(true);
        h.server(true);

        h.server(false);  // the operator clicked unmute
        QCOMPARE(h.controller.phase(), Phase::OperatorOverride);
        QVERIFY(!h.persisted.owned);

        h.advance(5000);
        QCOMPARE(h.sent.size(), size_t(1));  // no fight

        h.wanted(false);
        QCOMPARE(h.sent.size(), size_t(1));  // and nothing to restore
        QCOMPARE(h.controller.phase(), Phase::Idle);

        h.wanted(true);  // re-arming is a new decision
        QCOMPARE(h.controller.phase(), Phase::Engaging);
        QCOMPARE(h.sent, (std::vector<bool>{true, true}));
    }

    // Spec 4.7: a set within another client's 200 ms window is dropped, and the reply
    // shows the current value. That must read as "retry", not as an operator override.
    void aDroppedSetIsRetriedNotMistakenForTheOperator()
    {
        Harness h;
        h.connectWithServerMuted(false);
        h.wanted(true);
        h.server(false);  // the set was dropped
        QCOMPARE(h.controller.phase(), Phase::Engaging);
        QCOMPARE(h.sent.size(), size_t(1));

        h.advance(MuteController::kRetryMs - 1);
        QCOMPARE(h.sent.size(), size_t(1));  // not yet
        h.advance(1);
        QCOMPARE(h.sent, (std::vector<bool>{true, true}));

        h.server(true);
        QCOMPARE(h.controller.phase(), Phase::Holding);
    }

    void aDroppedRestoreIsRetried()
    {
        Harness h;
        h.connectWithServerMuted(false);
        h.wanted(true);
        h.server(true);
        h.wanted(false);
        h.server(true);  // dropped
        QCOMPARE(h.controller.phase(), Phase::Restoring);
        h.advance(MuteController::kRetryMs);
        QCOMPARE(h.sent, (std::vector<bool>{true, false, false}));
        h.server(false);
        QCOMPARE(h.controller.phase(), Phase::Idle);
    }

    void givesUpOnEngagingAndOwesNothing()
    {
        Harness h;
        h.connectWithServerMuted(false);
        h.wanted(true);
        for (int i = 0; i < MuteController::kMaxAttempts + 2; ++i) h.advance(MuteController::kRetryMs);
        QCOMPARE(h.controller.phase(), Phase::Failed);
        QCOMPARE(int(h.sent.size()), MuteController::kMaxAttempts);
        QVERIFY(!h.persisted.owned);
    }

    void aFailedRestoreIsRememberedForNextSession()
    {
        Harness h;
        h.connectWithServerMuted(false);
        h.wanted(true);
        h.server(true);
        h.wanted(false);
        for (int i = 0; i < MuteController::kMaxAttempts + 2; ++i) h.advance(MuteController::kRetryMs);
        QCOMPARE(h.controller.phase(), Phase::Failed);
        QVERIFY(h.persisted.owned);
    }

    // The trap: after a reconnect the server reports mute:true. That is OUR mute, and
    // must not become the remembered prior state, or we never unmute.
    void reconnectDoesNotMistakeOurMuteForTheOperators()
    {
        Harness h;
        h.connectWithServerMuted(false);
        h.wanted(true);
        h.server(true);

        h.controller.onDisconnected();
        h.controller.onConnected();
        h.server(true);
        QCOMPARE(h.controller.phase(), Phase::Holding);
        QVERIFY(h.persisted.owned);
        QVERIFY(!h.persisted.priorMuted);

        h.wanted(false);
        QCOMPARE(h.sent.back(), false);  // still restored
    }

    void reconnectToARestartedServerMutesAgain()
    {
        Harness h;
        h.connectWithServerMuted(false);
        h.wanted(true);
        h.server(true);
        h.controller.onDisconnected();
        h.controller.onConnected();
        h.server(false);  // ExpertSDR3 restarted: fresh state
        QCOMPARE(h.controller.phase(), Phase::Engaging);
        QCOMPARE(h.sent, (std::vector<bool>{true, true}));
        QVERIFY(!h.persisted.priorMuted);
    }

    void operatorOverrideSurvivesANetworkBlip()
    {
        Harness h;
        h.connectWithServerMuted(false);
        h.wanted(true);
        h.server(true);
        h.server(false);
        h.controller.onDisconnected();
        h.controller.onConnected();
        h.server(false);
        QCOMPARE(h.controller.phase(), Phase::OperatorOverride);
        QCOMPARE(h.sent.size(), size_t(1));
    }

    void crashRecoveryRestoresOnTheNextRun()
    {
        Harness h(MuteController::Ownership{true, false});
        h.connectWithServerMuted(true);  // still muted from before the crash
        QCOMPARE(h.sent, (std::vector<bool>{false}));
        QCOMPARE(h.controller.phase(), Phase::Restoring);
        h.server(false);
        QVERIFY(!h.persisted.owned);
    }

    void crashRecoveryAdoptsTheMuteIfStillWanted()
    {
        Harness h(MuteController::Ownership{true, false});
        h.wanted(true);
        h.connectWithServerMuted(true);
        QCOMPARE(h.controller.phase(), Phase::Holding);
        QVERIFY(h.sent.empty());
        h.wanted(false);
        QCOMPARE(h.sent, (std::vector<bool>{false}));  // the true prior state is honoured
    }

    void crashRecoveryWithAnAlreadyUnmutedServerJustForgets()
    {
        Harness h(MuteController::Ownership{true, false});
        h.connectWithServerMuted(false);
        QVERIFY(h.sent.empty());
        QVERIFY(!h.persisted.owned);
    }

    // Race: disarm while MUTE:true is in flight and the server has not reported it. It
    // may still land, so it must be undone, or ExpertSDR3 ends up muted with no owner.
    void disarmingMidEngageUndoesTheInFlightSet()
    {
        Harness h;
        h.connectWithServerMuted(false);
        h.wanted(true);
        h.wanted(false);
        QCOMPARE(h.sent, (std::vector<bool>{true, false}));
        QCOMPARE(h.controller.phase(), Phase::Restoring);
        QVERIFY(h.persisted.owned);

        h.server(true);  // the first set landed
        QCOMPARE(h.controller.phase(), Phase::Restoring);
        h.server(false);  // and so did the undo
        QCOMPARE(h.controller.phase(), Phase::Idle);
        QVERIFY(!h.persisted.owned);
    }

    // Race: re-arm while MUTE:false is in flight. Its echo must not read as the
    // operator unmuting.
    void rearmingMidRestoreIsNotAnOperatorOverride()
    {
        Harness h;
        h.connectWithServerMuted(false);
        h.wanted(true);
        h.server(true);
        h.wanted(false);
        h.wanted(true);
        QCOMPARE(h.sent, (std::vector<bool>{true, false, true}));
        QCOMPARE(h.controller.phase(), Phase::Engaging);

        h.server(false);  // the echo of the restore
        QCOMPARE(h.controller.phase(), Phase::Engaging);
        h.server(true);
        QCOMPARE(h.controller.phase(), Phase::Holding);
        QVERIFY(h.persisted.owned);
        QVERIFY(!h.persisted.priorMuted);
    }

    void repeatedSetWantedDoesNotResend()
    {
        Harness h;
        h.connectWithServerMuted(false);
        h.wanted(true);
        h.wanted(true);
        QCOMPARE(h.sent.size(), size_t(1));
    }

    void retriesStopWhileDisconnected()
    {
        Harness h;
        h.connectWithServerMuted(false);
        h.wanted(true);
        h.controller.onDisconnected();
        h.advance(10 * MuteController::kRetryMs);
        QCOMPARE(h.sent.size(), size_t(1));
    }
};

QTEST_APPLESS_MAIN(TestMuteController)
#include "test_mute_controller.moc"
