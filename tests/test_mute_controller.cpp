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
          controller({.sendMute = [this](bool muted) { sent.push_back(muted); },
                      .persist = [this](const MuteController::Ownership& o) { persisted = o; }},
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

// The same against a speaker that is a level, as the TS-990S's AF is: 0 is muted.
struct LevelHarness {
    std::vector<int> sent;
    MuteController::Ownership persisted;
    MuteController controller;
    qint64 now{1000};

    explicit LevelHarness(MuteController::Ownership restored = {})
        : persisted(restored),
          controller({.sendLevel = [this](int level) { sent.push_back(level); },
                      .persist = [this](const MuteController::Ownership& o) { persisted = o; }},
                     restored) {}

    void connectAt(int level)
    {
        controller.onConnected();
        controller.onServerLevel(level, now);
    }
    void wanted(bool value) { controller.setWanted(value, now); }
    void level(int value) { controller.onServerLevel(value, now); }
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

    // ── A speaker that is a level ────────────────────────────────────────────

    // AF goes to 0 while this app plays and back to exactly where it was, at any level.
    void aLevelIsTurnedDownAndGivenBackExactly()
    {
        for (int prior = 1; prior <= 255; ++prior) {
            LevelHarness h;
            h.connectAt(prior);
            h.wanted(true);
            QCOMPARE(h.sent, (std::vector<int>{0}));
            QCOMPARE(h.controller.phase(), Phase::Engaging);
            // Owned, with the level to give back, before anything was sent: a crash now is safe.
            QVERIFY(h.persisted.owned);
            QCOMPARE(h.persisted.priorLevel, prior);
            h.level(0);
            QCOMPARE(h.controller.phase(), Phase::Holding);
            h.wanted(false);
            QCOMPARE(h.sent, (std::vector<int>{0, prior}));
            QCOMPARE(h.controller.phase(), Phase::Restoring);
            h.level(prior);
            QCOMPARE(h.controller.phase(), Phase::Idle);
            QVERIFY(!h.persisted.owned);
            QVERIFY(h.controller.settled());
        }
    }

    // Nothing is sent before the level has been read on this connection.
    void nothingIsSentBeforeALevelIsRead()
    {
        LevelHarness h;
        h.wanted(true);
        h.controller.onConnected();
        h.advance(1000);
        QVERIFY(h.sent.empty());
        QCOMPARE(h.controller.phase(), Phase::Idle);
        h.level(120);
        QCOMPARE(h.sent, (std::vector<int>{0}));
    }

    // The AF was already down: it is the operator's, and stays down.
    void aLevelAlreadyAtZeroIsLeftThere()
    {
        LevelHarness h;
        h.connectAt(0);
        h.wanted(true);
        QVERIFY(h.sent.empty());
        QCOMPARE(h.controller.phase(), Phase::Holding);
        QVERIFY(!h.persisted.owned);
        h.wanted(false);
        QVERIFY(h.sent.empty());
        QCOMPARE(h.controller.phase(), Phase::Idle);
    }

    // The operator turns the knob up while it is held: hands off, and nothing is owed.
    void turningTheKnobUpHandsItBack()
    {
        LevelHarness h;
        h.connectAt(120);
        h.wanted(true);
        h.level(0);
        QCOMPARE(h.controller.phase(), Phase::Holding);
        h.level(40);
        QCOMPARE(h.controller.phase(), Phase::OperatorOverride);
        QVERIFY(!h.persisted.owned);
        h.advance(5000);
        h.level(0);  // and down again: still theirs
        h.wanted(false);
        QCOMPARE(h.sent, (std::vector<int>{0}));
    }

    // A crash while it was down: the next connection gives the level back, from the record.
    void aCrashWhileDownIsRestoredFromTheRecord()
    {
        LevelHarness h(MuteController::Ownership{true, false, 120});
        h.connectAt(0);
        QCOMPARE(h.sent, (std::vector<int>{120}));
        QCOMPARE(h.controller.phase(), Phase::Restoring);
        h.level(120);
        QCOMPARE(h.controller.phase(), Phase::Idle);
        QVERIFY(!h.persisted.owned);
    }

    // A crash, and the operator turned it up before this app came back: nothing is restored,
    // nor is their level touched.
    void aCrashAndTheKnobTurnedSinceIsLeftAlone()
    {
        LevelHarness h(MuteController::Ownership{true, false, 120});
        h.connectAt(60);
        QVERIFY(h.sent.empty());
        QVERIFY(!h.persisted.owned);
        QCOMPARE(h.controller.phase(), Phase::Idle);
    }

    // A crash while down, and this app plays again at once: it holds, and still owes 120.
    void aCrashWhileDownAndPlayingAgainKeepsTheRecord()
    {
        LevelHarness h(MuteController::Ownership{true, false, 120});
        h.wanted(true);
        h.connectAt(0);
        QVERIFY(h.sent.empty());
        QCOMPARE(h.controller.phase(), Phase::Holding);
        QCOMPARE(h.persisted.priorLevel, 120);
        h.wanted(false);
        QCOMPARE(h.sent, (std::vector<int>{120}));
    }

    // Played again while the restore is on its way: the stale 0 is never taken as the level to
    // give back.
    void reArmingMidRestoreKeepsThePriorLevel()
    {
        LevelHarness h;
        h.connectAt(120);
        h.wanted(true);
        h.level(0);
        h.wanted(false);
        QCOMPARE(h.controller.phase(), Phase::Restoring);
        h.wanted(true);
        QCOMPARE(h.persisted.priorLevel, 120);
        h.level(120);  // the restore landed
        h.advance(MuteController::kRetryMs);
        h.level(0);
        QCOMPARE(h.controller.phase(), Phase::Holding);
        h.wanted(false);
        QCOMPARE(h.sent.back(), 120);
    }

    // A set the radio never takes is retried a bounded number of times, then given up. The
    // record stays: the sets may have landed unseen. A later reading above 0 clears it, and
    // nothing is sent.
    void aLevelTheRadioNeverTakesIsGivenUp()
    {
        LevelHarness h;
        h.connectAt(120);
        h.wanted(true);
        for (int i = 0; i < MuteController::kMaxAttempts + 3; ++i) {
            h.level(120);
            h.advance(MuteController::kRetryMs);
        }
        QCOMPARE(h.controller.phase(), Phase::Failed);
        QCOMPARE(int(h.sent.size()), MuteController::kMaxAttempts);
        QVERIFY(h.persisted.owned);
        QCOMPARE(h.persisted.priorLevel, 120);
        h.wanted(false);
        h.level(120);
        QVERIFY(!h.persisted.owned);
        QCOMPARE(int(h.sent.size()), MuteController::kMaxAttempts);
        QCOMPARE(h.controller.phase(), Phase::Idle);
    }

    // The same, but the sets did land and were never seen: the next reading of 0 gives it back.
    void aFailedEngageThatLandedIsGivenBack()
    {
        LevelHarness h;
        h.connectAt(120);
        h.wanted(true);
        for (int i = 0; i < MuteController::kMaxAttempts + 3; ++i) h.advance(MuteController::kRetryMs);
        QCOMPARE(h.controller.phase(), Phase::Failed);
        h.wanted(false);
        h.level(0);
        QCOMPARE(h.sent.back(), 120);
        QCOMPARE(h.controller.phase(), Phase::Restoring);
        h.level(120);
        QVERIFY(!h.persisted.owned);
    }

    // The knob turned up is the operator's decision until they choose the feature again: this
    // app's audio stopping and starting again is not that.
    void anOverrideHoldsUntilRearmed()
    {
        LevelHarness h;
        h.connectAt(120);
        h.wanted(true);
        h.level(0);
        h.level(40);
        QCOMPARE(h.controller.phase(), Phase::OperatorOverride);
        h.wanted(false);
        h.wanted(true);
        h.level(40);
        h.advance(5000);
        QCOMPARE(h.controller.phase(), Phase::OperatorOverride);
        QCOMPARE(h.sent, (std::vector<int>{0}));
        h.controller.rearm(h.now);
        QCOMPARE(h.sent, (std::vector<int>{0, 0}));
        QCOMPARE(h.persisted.priorLevel, 40);  // their level now, to give back
        h.level(0);
        h.wanted(false);
        QCOMPARE(h.sent.back(), 40);
    }

    // A switch's override still ends when it is no longer wanted, as it always has.
    void aSwitchOverrideStillEndsWhenNotWanted()
    {
        Harness h;
        h.connectWithServerMuted(false);
        h.wanted(true);
        h.server(true);
        h.server(false);
        QCOMPARE(h.controller.phase(), Phase::OperatorOverride);
        h.wanted(false);
        QCOMPARE(h.controller.phase(), Phase::Idle);
        h.controller.rearm(h.now);  // nothing to re-arm
        QCOMPARE(h.controller.phase(), Phase::Idle);
    }

    // The link drops while it is down: the record stays, and the next link carries on.
    void aLostLinkKeepsWhatIsOwed()
    {
        LevelHarness h;
        h.connectAt(120);
        h.wanted(true);
        h.level(0);
        h.controller.onDisconnected();
        QCOMPARE(h.controller.phase(), Phase::Idle);
        QVERIFY(h.persisted.owned);
        h.wanted(false);
        QCOMPARE(h.sent, (std::vector<int>{0}));  // nothing can be sent without the link
        h.connectAt(0);
        QCOMPARE(h.sent, (std::vector<int>{0, 120}));
    }
};

QTEST_APPLESS_MAIN(TestMuteController)
#include "test_mute_controller.moc"
