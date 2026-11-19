// The player's own session and account: identity, the async round trip to the database, the stats
// answer and the heartbeat.
//
// This holds the whole account flow, not just the two frame shapes: who may start an auth, the claim
// of the "one account, one connection" registry entry, and the answer the lobby is asked to send. It
// knows nothing about the wire protocol — the frame table that reaches these methods lives in
// player_agent.h, so adding a frame never means touching this class.

#pragma once

#include <string>

#include <exec/task.hpp>

#include "db.h"
#include "json.h"

namespace mahjong {

class PlayerActor;

class PlayerSessionAgent {
 public:
  explicit PlayerSessionAgent(PlayerActor& player) : player_(player) {}

  // ---- frames ----
  stdexec::task<void> HandleLogin(json::Value frame);
  stdexec::task<void> HandleRegister(json::Value frame);
  stdexec::task<void> HandlePing(json::Value frame);
  stdexec::task<void> HandleStatsRequest(json::Value frame);

  // ---- database results, delivered by PlayerActor::Dispatch ----
  // Each is the only place the session's identity changes: the async answer lands on this actor's
  // mailbox, never on the DbActor pool.
  void OnLoginResult(AuthOutcome outcome);
  void OnRegisterResult(AuthOutcome outcome);
  void OnStatsResult(StatsOutcome outcome);

 private:
  // The account entry points the two auth frames reach. Each owns the "no database" refusal
  // (INTERNAL), so a handler never has to know about quotas, and the password never travels further
  // than this object.
  void BeginAccountLogin(std::string username, std::string password, bool resume);
  void LoginAsGuest(std::string name);
  void BeginRegister(std::string username, std::string password, std::string nickname);
  void RequestStats();
  // Runs the final half of a successful register/login: claims the account for this connection (or
  // answers ALREADY_ONLINE) and hands the answer to the lobby. Returns whether it took it.
  bool AcceptAccount(const AuthOutcome& outcome);
  // Answers INTERNAL for "MySQL is not usable". Deliberately not charged to any quota: the client did
  // nothing wrong.
  void AccountServiceUnavailable();

  PlayerActor& player_;

  // The account login's `resume` flag, remembered across the asynchronous database round trip so the
  // success path can tell the lobby whether to reattach the room.
  bool pending_resume_ = true;
};

}  // namespace mahjong
