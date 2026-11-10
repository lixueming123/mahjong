// RoomActor: one table, driven by a Game plugin. No rules live here.
//
// What is here is everything that would be identical for poker, mahjong or a two-player number
// game: who sits where, where a seat's frames go, the single message entry point and its gate,
// the one-shot deadline, the delivery of a step's outbounds, the per-round report to the owner
// (§1.4.5), and the disconnect/reconnect bookkeeping (§8.5, PROTOCOL v2.3).
//
// What is NOT here — and must never appear — is a wall, a hand, a turn, a window, a point, a
// `seq`, a `phase`, a `mode`. Those are the Game's (game.h). If a change to a rule ever makes you
// want to add a member to this class, the answer is a hook on Game instead: the whole point of
// the split is that adding a game does not touch this file.
//
// Two ways in, both on RoomEndpoint: `Dispatch` (the message entry, which takes the gate) and
// `TakeSeat` (request/response, same gate). Handlers are private, so nothing can reach room state
// without the gate — async_gate.h explains why that matters.
//
// Threading: the gate is taken on every entry and held across the suspensions (delivery awaits
// each seat's mailbox), because ex_actor would otherwise start the next message the moment a
// handler suspended. That is the one thing the framework does not give us.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "async_gate.h"
#include "ex_actor/api.h"
#include "game.h"
#include "json.h"
#include "room_contract.h"
#include "room_endpoint.h"
#include "seat_endpoint.h"
#include "timer.h"

namespace mahjong {

class RoomActor : public RoomEndpoint {
 public:
  // |options_json| is the game's own configuration, opaque here (the lobby accepted it, this
  // class only carries it to Game::Setup). |game_id| is what the lobby asked for, kept for logs
  // and for the settlement; the binding to a concrete Game is the template parameter of
  // RoomActorFor, so a mismatch is a wiring bug, not a runtime choice.
  //
  // |owner| is the lobby that spawned this room: it gets every settled round (GameSettlement) and
  // never written match history by the room. Empty in the unit test, which passes
  // |finished_for_test| instead — a scheduler and a capture callback are process-local, so they
  // cannot travel in a message.
  RoomActor(std::string room_id, std::string game_id, std::string options_json, uint64_t seed,
            int action_timeout_ms, int ai_delay_ms, int inter_hand_delay_ms, std::vector<RoomSeat> seats,
            std::shared_ptr<WindowTimer> timer, ex_actor::ActorRef<RoomOwner> owner = {},
            TimeoutScheduler timeout_scheduler = {}, RoomFinishedCallback finished_for_test = {});

  stdexec::task<void> Dispatch(RoomEnvelope envelope) override;
  stdexec::task<bool> TakeSeat(RoomSeat seat) override;

  // Test seam, installed right after construction and before any traffic: production rooms
  // report through |owner| instead. Deliberately not a message — a std::function cannot be
  // serialized.
  void SetFinishedCallbackForTest(RoomFinishedCallback callback) { finished_for_test_ = std::move(callback); }

  // Introspection (logs and tests).
  const std::string& room_id() const { return room_id_; }
  const std::string& game_id() const { return game_id_; }
  bool game_started() const { return game_started_; }
  int SeatOf(const std::string& player_id) const;
  const std::vector<RoomSeat>& seats() const { return seats_; }

 protected:
  // The single concrete binding: RoomActorFor<GameT> returns its own member (covariantly, so a
  // test can reach the concrete type's introspection without a cast).
  virtual Game& game() = 0;

  // ex_actor hands every actor its own ref right after construction; RoomActorFor forwards it
  // here. The room needs it to post the frozen deadline back to itself, so the production
  // scheduler is installed here instead of arriving in a message (a std::function could never be
  // serialized).
  void BindRuntime(ex_actor::ActorRef<RoomEndpoint> self);

 private:
  // Reached only through Dispatch, i.e. only while gate_ is held.
  bool TakeSeatLocked(RoomSeat seat);

  stdexec::task<void> Ready(PlayerInfo player);
  stdexec::task<void> Sync(PlayerInfo player);
  stdexec::task<void> HandleGameFrame(PlayerInfo player, json::Value frame);
  stdexec::task<void> OnTimeout(uint64_t token);
  stdexec::task<void> OnPlayerDisconnected(PlayerInfo player, ex_actor::ActorRef<SeatEndpoint> conn);
  stdexec::task<void> OnIdleTimeout(PlayerInfo player);
  stdexec::task<void> OnReconnected(PlayerInfo player, ex_actor::ActorRef<SeatEndpoint> endpoint);

  // The one place a GameStep happens. Order is frozen by the protocol, not by taste: the frames
  // go out first (a seat must be able to see the window before its deadline can fire), then the
  // deadline is armed, then a settled round is reported to the owner.
  stdexec::task<void> Run(GameStep step);
  stdexec::task<void> Deliver(const std::vector<Outbound>& outbounds);
  stdexec::task<void> DeliverSeat(int seat, std::string frame);
  // Answer one seat with an error frame and the §7.2 quota its kind names.
  stdexec::task<void> ReplyError(int seat, protocol::ErrorCode code, std::string_view message);
  // Per-round report to the owner (ACCOUNT_DESIGN.md §4): the outcome plus the identity this
  // room has held since the deal.
  void ReportResult(const GameResult& result);

  GameContext Context() const;

  std::string room_id_;
  std::string game_id_;
  std::string options_json_;
  uint64_t seed_;
  int action_timeout_ms_;
  int ai_delay_ms_;
  int inter_hand_delay_ms_;
  std::vector<RoomSeat> seats_;
  std::shared_ptr<WindowTimer> timer_;
  TimeoutScheduler timeout_scheduler_;
  RoomFinishedCallback finished_for_test_;
  ex_actor::ActorRef<RoomOwner> owner_;
  ex_actor::ActorRef<RoomEndpoint> self_ref_;
  bool game_started_ = false;
  AsyncGate gate_;
};

}  // namespace mahjong
