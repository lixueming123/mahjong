// Who this connection currently speaks for.
//
// One TCP connection is at most one player (player_actor.h), so the session lives inside
// PlayerActor rather than in an actor of its own: a separate SessionActor would turn every
// "who sent this frame" into a cross-actor query on the hot path, or force PlayerActor to
// keep a mirror of the answer. What was actually missing was not a thread — it was a type.
// Before this, the five reachable states were spread across three bools plus `account_id_`,
// which admits eleven combinations that can never happen and leaves every transition implicit
// in a different handler.
//
// The states, and nothing else:
//
//   kAnonymous --BeginAuth--> kAuthPending --LoginAsAccount--> kAccount
//        |                          |
//        |                          +--FailAuth--> kAnonymous
//        +--LoginAsGuest--> kGuest
//
//   * --Close--> kClosed            (absorbing: nothing leaves it)
//
// Every transition reports whether it happened, and one that does not apply to the current phase
// is a silent no-op rather than an assert. That is deliberate: a socket close (gateway thread)
// and an account result (DbActor pool) genuinely race, and the loser must not undo the winner.
// A close that arrives while a login is in flight therefore sticks, and the late result is
// dropped by AcceptAccount instead of resurrecting the session.

#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "player.h"

namespace mahjong {

class Session {
 public:
  enum class Phase {
    kAnonymous,    // no identity yet; login/register are the only frames accepted
    kAuthPending,  // an account login or register is in flight; still no identity
    kGuest,        // logged in under a per-process id (ACCOUNT_DESIGN.md §1.2)
    kAccount,      // logged in behind a claimed account
    kClosed,       // OnClosed ran; only the release path remains
  };

  Phase phase() const { return phase_; }
  bool logged_in() const { return phase_ == Phase::kGuest || phase_ == Phase::kAccount; }
  bool closed() const { return phase_ == Phase::kClosed; }
  // True while this session owns an entry in the SessionRegistry, i.e. while the account must
  // be released on the way out (§1.3).
  bool holds_account() const { return phase_ == Phase::kAccount; }

  // True while the connection may still start an account operation: reachable for a guest that
  // has not logged in yet, refused once this session already has an identity (or one in flight).
  bool CanStartAuth() const { return phase_ == Phase::kAnonymous; }

  uint64_t account_id() const { return account_id_; }  // 0 for a guest or a bot
  const std::string& player_id() const { return player_id_; }
  const std::string& name() const { return name_; }

  // What the lobby and the rooms accept: a snapshot, never a reference into this object.
  PlayerInfo Info() const { return PlayerInfo {.player_id = player_id_, .name = name_, .account_id = account_id_}; }

  bool BeginAuth() {
    if (phase_ != Phase::kAnonymous) {
      return false;
    }
    phase_ = Phase::kAuthPending;
    return true;
  }

  bool FailAuth() {
    if (phase_ != Phase::kAuthPending) {
      return false;
    }
    phase_ = Phase::kAnonymous;
    return true;
  }

  bool LoginAsGuest(std::string player_id, std::string name) {
    if (phase_ != Phase::kAnonymous) {
      return false;
    }
    phase_ = Phase::kGuest;
    account_id_ = 0;
    player_id_ = std::move(player_id);
    name_ = std::move(name);
    return true;
  }

  // Only reachable from kAuthPending: a guest never becomes an account mid-session, and the
  // claim of the registry entry happens in the caller, next to the registry itself.
  bool LoginAsAccount(uint64_t account_id, std::string player_id, std::string name) {
    if (phase_ != Phase::kAuthPending) {
      return false;
    }
    phase_ = Phase::kAccount;
    account_id_ = account_id;
    player_id_ = std::move(player_id);
    name_ = std::move(name);
    return true;
  }

  // The identity fields survive the close on purpose: the close path still logs who it was, and
  // nothing can start an operation from kClosed anyway.
  bool Close() {
    if (phase_ == Phase::kClosed) {
      return false;
    }
    phase_ = Phase::kClosed;
    return true;
  }

 private:
  Phase phase_ = Phase::kAnonymous;
  uint64_t account_id_ = 0;
  std::string player_id_;
  std::string name_;
};

}  // namespace mahjong
