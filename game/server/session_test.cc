// Unit tests for the session state machine (session.h).
//
// These replace what used to be implicit: with the old `logged_in_`/`auth_pending_`/
// `session_claimed_` bools, the legal states and the legal transitions were only recorded in
// the comments of the functions that happened to touch them, and two asynchronous paths — a
// socket close and an account result — could interleave in ways nothing checked.

#include "session.h"

#include <gtest/gtest.h>

#include "player.h"

namespace mahjong {
namespace {

TEST(Session, StartsAnonymousAndAcceptsAuth) {
  Session session;
  EXPECT_EQ(session.phase(), Session::Phase::kAnonymous);
  EXPECT_TRUE(session.CanStartAuth());
  EXPECT_FALSE(session.logged_in());
  EXPECT_FALSE(session.closed());
  EXPECT_FALSE(session.holds_account());
  EXPECT_EQ(session.account_id(), 0u);
}

TEST(Session, GuestLoginCarriesTheAllocatedIdentity) {
  Session session;
  ASSERT_TRUE(session.LoginAsGuest("p-0f3a1c", "Alice"));
  EXPECT_EQ(session.phase(), Session::Phase::kGuest);
  EXPECT_TRUE(session.logged_in());
  EXPECT_FALSE(session.holds_account()) << "a guest must never look like a claimed account";
  EXPECT_FALSE(session.CanStartAuth()) << "an identity is already established";
  EXPECT_EQ(session.player_id(), "p-0f3a1c");
  EXPECT_EQ(session.name(), "Alice");
  EXPECT_EQ(session.account_id(), 0u);
}

TEST(Session, AccountLoginGoesThroughThePendingPhase) {
  Session session;
  ASSERT_TRUE(session.BeginAuth());
  EXPECT_EQ(session.phase(), Session::Phase::kAuthPending);
  EXPECT_FALSE(session.logged_in()) << "no identity exists until the database answers";
  EXPECT_FALSE(session.CanStartAuth()) << "a second login must wait for the first";

  ASSERT_TRUE(session.LoginAsAccount(42, "u-000042", "Alice"));
  EXPECT_EQ(session.phase(), Session::Phase::kAccount);
  EXPECT_TRUE(session.logged_in());
  EXPECT_TRUE(session.holds_account());
  EXPECT_EQ(session.account_id(), 42u);
  EXPECT_EQ(session.player_id(), "u-000042");

  const PlayerInfo info = session.Info();
  EXPECT_EQ(info.player_id, "u-000042");
  EXPECT_EQ(info.name, "Alice");
  EXPECT_EQ(info.account_id, 42u);
}

// §3.1: a mistyped password must leave the connection able to try again.
TEST(Session, FailedAuthReturnsToAnonymous) {
  Session session;
  ASSERT_TRUE(session.BeginAuth());
  ASSERT_TRUE(session.FailAuth());
  EXPECT_EQ(session.phase(), Session::Phase::kAnonymous);
  EXPECT_TRUE(session.CanStartAuth());
  EXPECT_FALSE(session.logged_in());
}

TEST(Session, AccountLoginCannotSkipThePendingPhase) {
  Session session;
  EXPECT_FALSE(session.LoginAsAccount(42, "u-000042", "Alice"));
  EXPECT_EQ(session.phase(), Session::Phase::kAnonymous);

  // Nor may a guest slide into an account mid-session: only kAuthPending may become kAccount.
  ASSERT_TRUE(session.LoginAsGuest("p-0f3a1c", "Alice"));
  EXPECT_FALSE(session.LoginAsAccount(42, "u-000042", "Alice"));
  EXPECT_EQ(session.phase(), Session::Phase::kGuest);
}

TEST(Session, GuestLoginIsRefusedWhileAnAccountLoginIsInFlight) {
  Session session;
  ASSERT_TRUE(session.BeginAuth());
  EXPECT_FALSE(session.LoginAsGuest("p-0f3a1c", "Alice"));
  EXPECT_EQ(session.phase(), Session::Phase::kAuthPending);
}

TEST(Session, RepeatedTransitionsAreNoOps) {
  Session session;
  ASSERT_TRUE(session.BeginAuth());
  EXPECT_FALSE(session.BeginAuth());
  EXPECT_EQ(session.phase(), Session::Phase::kAuthPending);

  EXPECT_FALSE(session.LoginAsGuest("p-0f3a1c", "Alice")) << "a guest login must not win over the one in flight";
  EXPECT_EQ(session.phase(), Session::Phase::kAuthPending);
}

// The race the old bools could not express: the poll thread closes the socket while the database
// query is still running, so the result arrives afterwards. The close must win — a late
// LoginAsAccount would otherwise resurrect a session whose account nobody will ever release.
TEST(Session, CloseOutranksALateAccountResult) {
  Session session;
  ASSERT_TRUE(session.BeginAuth());
  ASSERT_TRUE(session.Close());
  EXPECT_EQ(session.phase(), Session::Phase::kClosed);

  EXPECT_FALSE(session.LoginAsAccount(42, "u-000042", "Alice"));
  EXPECT_FALSE(session.LoginAsGuest("p-0f3a1c", "Alice"));
  EXPECT_FALSE(session.BeginAuth());
  EXPECT_EQ(session.phase(), Session::Phase::kClosed);
  EXPECT_FALSE(session.logged_in());
  EXPECT_FALSE(session.holds_account()) << "nothing was claimed, so nothing may be released";
}

TEST(Session, CloseIsAbsorbingFromEveryPhase) {
  for (const Session::Phase phase : {Session::Phase::kAnonymous, Session::Phase::kAuthPending,
                                     Session::Phase::kGuest, Session::Phase::kAccount}) {
    Session session;
    switch (phase) {
      case Session::Phase::kAnonymous:
        break;
      case Session::Phase::kAuthPending:
        ASSERT_TRUE(session.BeginAuth());
        break;
      case Session::Phase::kGuest:
        ASSERT_TRUE(session.LoginAsGuest("p-0f3a1c", "Alice"));
        break;
      case Session::Phase::kAccount:
        ASSERT_TRUE(session.BeginAuth());
        ASSERT_TRUE(session.LoginAsAccount(42, "u-000042", "Alice"));
        break;
      case Session::Phase::kClosed:
        FAIL() << "not a starting phase";
        break;
    }
    EXPECT_TRUE(session.Close());
    EXPECT_TRUE(session.closed());
    EXPECT_FALSE(session.Close()) << "a second close must be a no-op";
  }
}

// The close path reads the identity to log it, so the fields must survive the transition.
TEST(Session, CloseKeepsTheIdentityForLogging) {
  Session session;
  ASSERT_TRUE(session.LoginAsGuest("p-0f3a1c", "Alice"));
  ASSERT_TRUE(session.Close());
  EXPECT_EQ(session.player_id(), "p-0f3a1c");
  EXPECT_EQ(session.name(), "Alice");
}

}  // namespace
}  // namespace mahjong
