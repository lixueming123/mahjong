// Unit tests for the pool's policy layer (db_policy.h): MySQL error classification, the circuit
// breaker that turns a dead MySQL into cheap failures, and the slot ownership table.
//
// Deliberately not linked against mahjong_db: the pool itself needs libmysqlclient, while all of
// the above is pure state, so this target stays green on a machine without MySQL. The end-to-end
// path (real MySQL) is tests/account_e2e_test.py.

#include "db_policy.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace mahjong {
namespace {

FailureBudgetOptions BudgetOptions(uint32_t failure_threshold = 2, int64_t cooldown_base_ms = 500,
                                   int64_t cooldown_max_ms = 30000, int64_t cooldown_floor_ms = 0) {
  FailureBudgetOptions options;
  options.failure_threshold = failure_threshold;
  options.cooldown_base_ms = cooldown_base_ms;
  options.cooldown_max_ms = cooldown_max_ms;
  options.cooldown_floor_ms = cooldown_floor_ms;
  return options;
}

// ---- error classification ----------------------------------------------------------------

TEST(DbErrorClassification, ConnectionLossBreaksTheConnection) {
  // GONE, LOST, COMMANDS_OUT_OF_SYNC (session state unknown), KILLED, SERVER_SHUTDOWN.
  for (unsigned int code : {1053u, 1927u, 2006u, 2013u, 2014u}) {
    EXPECT_EQ(ClassifyMysqlError(code), DbFailureClass::kConnectionLost) << "code " << code;
    EXPECT_TRUE(BreaksConnection(ClassifyMysqlError(code))) << "code " << code;
  }
}

TEST(DbErrorClassification, ConnectErrorsLeaveNoConnectionBehind) {
  for (unsigned int code : {2002u, 2003u, 2005u}) {
    EXPECT_EQ(ClassifyMysqlError(code), DbFailureClass::kConnectFailed) << "code " << code;
    // There is no socket to close, so this must not ask the pool to break one.
    EXPECT_FALSE(BreaksConnection(ClassifyMysqlError(code))) << "code " << code;
  }
}

TEST(DbErrorClassification, WrongCredentialsOrDatabaseAreConfigErrors) {
  for (unsigned int code : {1044u, 1045u, 1049u}) {
    EXPECT_EQ(ClassifyMysqlError(code), DbFailureClass::kConfig) << "code " << code;
    EXPECT_FALSE(BreaksConnection(ClassifyMysqlError(code))) << "code " << code;
  }
}

TEST(DbErrorClassification, DuplicateEntryIsANormalAnswer) {
  EXPECT_EQ(ClassifyMysqlError(1062), DbFailureClass::kDuplicate);
  EXPECT_FALSE(BreaksConnection(DbFailureClass::kDuplicate));
}

TEST(DbErrorClassification, EverythingElseIsAPerStatementError) {
  // Unknown column, syntax error, data too long.
  for (unsigned int code : {1054u, 1064u, 1406u}) {
    EXPECT_EQ(ClassifyMysqlError(code), DbFailureClass::kQueryError) << "code " << code;
    EXPECT_FALSE(BreaksConnection(ClassifyMysqlError(code))) << "code " << code;
  }
}

TEST(DbErrorClassification, ZeroMeansNoError) { EXPECT_EQ(ClassifyMysqlError(0), DbFailureClass::kNone); }

TEST(DbErrorClassification, OnlyUnavailableClassesCountTowardsTheCircuit) {
  EXPECT_TRUE(CountsAsUnavailable(DbFailureClass::kConnectionLost));
  EXPECT_TRUE(CountsAsUnavailable(DbFailureClass::kConnectFailed));
  EXPECT_TRUE(CountsAsUnavailable(DbFailureClass::kConfig));

  // A duplicate or a bad statement says nothing about the server's health.
  EXPECT_FALSE(CountsAsUnavailable(DbFailureClass::kDuplicate));
  EXPECT_FALSE(CountsAsUnavailable(DbFailureClass::kQueryError));
  EXPECT_FALSE(CountsAsUnavailable(DbFailureClass::kNone));
}

// ---- circuit breaker ---------------------------------------------------------------------

TEST(FailureBudgetTest, AllowsWhileClosed) {
  FailureBudget budget {BudgetOptions()};

  EXPECT_EQ(budget.BeginAttempt(0), FailureBudget::Verdict::kAllow);
  EXPECT_FALSE(budget.circuit_open(0));
  EXPECT_EQ(budget.consecutive_failures(), 0u);
}

TEST(FailureBudgetTest, StaysClosedBelowThreshold) {
  FailureBudget budget(BudgetOptions(/*failure_threshold=*/2));

  budget.BeginAttempt(0);
  budget.OnFailure(0);
  EXPECT_FALSE(budget.circuit_open(0));
  EXPECT_EQ(budget.consecutive_failures(), 1u);
  // One failure is not enough to deny the next request.
  EXPECT_EQ(budget.BeginAttempt(1), FailureBudget::Verdict::kAllow);
}

TEST(FailureBudgetTest, OpensAtThresholdAndRejectsDuringCooldown) {
  FailureBudget budget(BudgetOptions(/*failure_threshold=*/2, /*cooldown_base_ms=*/500));

  budget.BeginAttempt(0);
  budget.OnFailure(0);
  budget.BeginAttempt(10);
  budget.OnFailure(10);

  EXPECT_TRUE(budget.circuit_open(10));
  EXPECT_EQ(budget.cooldown_until_ms(), 510);
  // One millisecond before the deadline the request must still be rejected without a socket.
  EXPECT_EQ(budget.BeginAttempt(509), FailureBudget::Verdict::kReject);
  EXPECT_TRUE(budget.circuit_open(509));
}

TEST(FailureBudgetTest, HalfOpenProbeAllowedAfterCooldown) {
  FailureBudget budget(BudgetOptions(/*failure_threshold=*/1, /*cooldown_base_ms=*/500));

  budget.OnFailure(0);
  ASSERT_EQ(budget.cooldown_until_ms(), 500);
  EXPECT_EQ(budget.BeginAttempt(500), FailureBudget::Verdict::kAllowProbe);
}

TEST(FailureBudgetTest, SingleProbeInFlight) {
  FailureBudget budget(BudgetOptions(/*failure_threshold=*/1, /*cooldown_base_ms=*/500));

  budget.OnFailure(0);
  EXPECT_EQ(budget.BeginAttempt(500), FailureBudget::Verdict::kAllowProbe);
  // The probe has not reported back yet: nobody else may try, or a slow probe turns into a herd.
  EXPECT_EQ(budget.BeginAttempt(501), FailureBudget::Verdict::kReject);
}

TEST(FailureBudgetTest, ProbeSuccessClosesAndResets) {
  FailureBudget budget(BudgetOptions(/*failure_threshold=*/2, /*cooldown_base_ms=*/500));

  budget.OnFailure(0);
  budget.OnFailure(1);
  ASSERT_EQ(budget.BeginAttempt(501), FailureBudget::Verdict::kAllowProbe);

  budget.OnSuccess(502);
  EXPECT_FALSE(budget.circuit_open(502));
  EXPECT_EQ(budget.consecutive_failures(), 0u);
  EXPECT_EQ(budget.cooldown_until_ms(), 0);
  EXPECT_EQ(budget.BeginAttempt(502), FailureBudget::Verdict::kAllow);
}

TEST(FailureBudgetTest, ProbeFailureDoublesTheCooldown) {
  FailureBudget budget(BudgetOptions(/*failure_threshold=*/2, /*cooldown_base_ms=*/500));

  budget.OnFailure(0);
  budget.OnFailure(10);
  ASSERT_EQ(budget.cooldown_until_ms(), 510);  // 500 * 2^0

  ASSERT_EQ(budget.BeginAttempt(510), FailureBudget::Verdict::kAllowProbe);
  budget.OnFailure(510);
  EXPECT_EQ(budget.cooldown_until_ms(), 1510);  // 500 * 2^1

  ASSERT_EQ(budget.BeginAttempt(1510), FailureBudget::Verdict::kAllowProbe);
  budget.OnFailure(1510);
  EXPECT_EQ(budget.cooldown_until_ms(), 3510);  // 500 * 2^2
}

TEST(FailureBudgetTest, CooldownIsCappedAtMax) {
  FailureBudget budget(BudgetOptions(/*failure_threshold=*/1, /*cooldown_base_ms=*/500,
                                     /*cooldown_max_ms=*/3000));

  int64_t last_interval = 0;
  int64_t now = 0;
  for (int attempt = 0; attempt < 10; ++attempt) {
    budget.OnFailure(now);
    last_interval = budget.cooldown_until_ms() - now;
    now = budget.cooldown_until_ms();
    ASSERT_EQ(budget.BeginAttempt(now), FailureBudget::Verdict::kAllowProbe);
  }
  // The last interval is the cap, not 500 * 2^9.
  EXPECT_EQ(last_interval, 3000);
}

TEST(FailureBudgetTest, CooldownFloorOutlastsOneAttempt) {
  // The pool sets the floor to the connect timeout, so an open circuit always lasts longer than
  // the attempt that opened it. Without this, a hung connect (3s) with a 500ms base cooldown let
  // every request pay the full 3s again — measured, see DbPool::DbPool.
  FailureBudget budget(BudgetOptions(/*failure_threshold=*/2, /*cooldown_base_ms=*/500,
                                     /*cooldown_max_ms=*/30000, /*cooldown_floor_ms=*/3000));

  budget.OnFailure(0);
  budget.OnFailure(0);
  EXPECT_EQ(budget.cooldown_until_ms(), 3000);

  // The doubling starts at the floor: 3000 -> 6000, not 3000 -> 1000 -> 2000 -> 3000.
  budget.OnFailure(3000);
  EXPECT_EQ(budget.cooldown_until_ms(), 9000);
}

TEST(FailureBudgetTest, CooldownFloorNeverExceedsTheCap) {
  FailureBudget budget(BudgetOptions(/*failure_threshold=*/1, /*cooldown_base_ms=*/500,
                                     /*cooldown_max_ms=*/1000, /*cooldown_floor_ms=*/5000));

  budget.OnFailure(0);
  // Capped, not floored: a floor above the cap would mean an unbounded outage.
  EXPECT_EQ(budget.cooldown_until_ms(), 1000);
}

TEST(FailureBudgetTest, ThresholdZeroIsClampedToOne) {
  FailureBudget budget(BudgetOptions(/*failure_threshold=*/0));

  budget.OnFailure(0);
  EXPECT_TRUE(budget.circuit_open(0));
  EXPECT_EQ(budget.consecutive_failures(), 1u);
}

TEST(FailureBudgetTest, ZeroCooldownBaseIsClampedUp) {
  FailureBudget budget(BudgetOptions(/*failure_threshold=*/1, /*cooldown_base_ms=*/0));

  budget.OnFailure(0);
  // A zero base would make the breaker flap on every request.
  EXPECT_GT(budget.cooldown_until_ms(), 0);
}

TEST(FailureBudgetTest, HugeFailureCountDoesNotOverflowTheShift) {
  FailureBudget budget(BudgetOptions(/*failure_threshold=*/1, /*cooldown_base_ms=*/500,
                                     /*cooldown_max_ms=*/30000));

  int64_t now = 0;
  for (int failure = 0; failure < 100; ++failure) {
    budget.OnFailure(now);
    const int64_t interval = budget.cooldown_until_ms() - now;
    // Always positive and never above the cap, even when the shift is clamped at 20.
    EXPECT_GT(interval, 0);
    EXPECT_LE(interval, 30000);
    now = budget.cooldown_until_ms();
  }
  EXPECT_EQ(budget.cooldown_until_ms() - now, 0);
  EXPECT_EQ(budget.consecutive_failures(), 100u);
}

TEST(FailureBudgetTest, ConsecutiveFailuresAccumulate) {
  FailureBudget budget {BudgetOptions()};

  for (uint32_t failure = 0; failure < 10; ++failure) {
    budget.OnFailure(0);
  }
  EXPECT_EQ(budget.consecutive_failures(), 10u);
}

TEST(FailureBudgetTest, OpenCountIncrementsPerTrip) {
  FailureBudget budget(BudgetOptions(/*failure_threshold=*/1, /*cooldown_base_ms=*/100));

  EXPECT_EQ(budget.open_count(), 0u);
  budget.OnFailure(0);  // trip 1
  EXPECT_EQ(budget.open_count(), 1u);

  ASSERT_EQ(budget.BeginAttempt(100), FailureBudget::Verdict::kAllowProbe);
  budget.OnFailure(100);  // the failed probe is trip 2
  EXPECT_EQ(budget.open_count(), 2u);
}

// ---- slot ownership ----------------------------------------------------------------------

TEST(SlotOwnershipTest, AcceptsEachSlotOnce) {
  SlotOwnership ownership(2);

  EXPECT_TRUE(ownership.TryBind(0, 7));
  // Re-binding the same pair is idempotent: Initialize can be retried without leaking the slot.
  EXPECT_TRUE(ownership.TryBind(0, 7));
  // A different thread must not steal a bound slot.
  EXPECT_FALSE(ownership.TryBind(0, 8));
  EXPECT_TRUE(ownership.TryBind(1, 8));

  EXPECT_EQ(ownership.bound_count(), 2u);
}

TEST(SlotOwnershipTest, RejectsOutOfRangeSlot) {
  SlotOwnership ownership(2);

  EXPECT_FALSE(ownership.TryBind(2, 7));
  EXPECT_FALSE(ownership.TryUnbind(2, 7));
  EXPECT_FALSE(ownership.bound(2));
}

TEST(SlotOwnershipTest, RejectsForeignUnbind) {
  SlotOwnership ownership(1);

  ASSERT_TRUE(ownership.TryBind(0, 7));
  EXPECT_FALSE(ownership.TryUnbind(0, 8));
  EXPECT_TRUE(ownership.bound(0));
  EXPECT_EQ(ownership.bound_count(), 1u);
}

TEST(SlotOwnershipTest, AllowsRebindAfterUnbind) {
  SlotOwnership ownership(1);

  ASSERT_TRUE(ownership.TryBind(0, 7));
  EXPECT_TRUE(ownership.TryUnbind(0, 7));
  EXPECT_FALSE(ownership.bound(0));
  EXPECT_EQ(ownership.bound_count(), 0u);
  // The next thread (a restarted shard) can take the slot.
  EXPECT_TRUE(ownership.TryBind(0, 8));
}

TEST(SlotOwnershipTest, CountsBoundSlots) {
  SlotOwnership ownership(3);

  EXPECT_EQ(ownership.slot_count(), 3u);
  EXPECT_EQ(ownership.bound_count(), 0u);
  EXPECT_TRUE(ownership.TryBind(0, 1));
  EXPECT_TRUE(ownership.TryBind(2, 1));
  EXPECT_EQ(ownership.bound_count(), 2u);
  EXPECT_TRUE(ownership.TryUnbind(0, 1));
  EXPECT_EQ(ownership.bound_count(), 1u);
}

}  // namespace
}  // namespace mahjong
