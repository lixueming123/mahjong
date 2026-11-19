// RoomActor: one table, driven by a Game plugin. No rules live here.
//
// Everything that would be identical for any game lives here: seats, where a seat's frames go, the
// single message entry and its gate, the one-shot deadline, delivery of a step's outbounds, the
// per-round report to the owner, and disconnect/reconnect bookkeeping.
//
// What must never appear here: a wall, a hand, a turn, a window, a point, a seq, a phase, a mode.
// Those are the Game's (game.h). A rule change that seems to need a new member here is a hook on
// Game instead.
//
// Two ways in, both on RoomEndpoint: `Dispatch` (message entry) and `TakeSeat` (request/response).
// Handlers are private, so nothing reaches room state without the gate (async_gate.h).
//
// Threading: the gate is held across the suspensions (delivery awaits each seat's mailbox), because
// ex_actor would otherwise start the next message the moment a handler suspended.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "async_gate.h"
#include "clock.h"
#include "ex_actor/api.h"
#include "game.h"
#include "json.h"
#include "replay.h"
#include "room_contract.h"
#include "room_endpoint.h"
#include "seat_endpoint.h"
#include "timer.h"

namespace mahjong {

class RoomActor : public RoomEndpoint {
 public:
  // |options_json| is the game's own configuration, opaque here. |owner| is the lobby that spawned
  // this room: it gets every settled round and never sees match history written. Empty in the unit
  // test, which passes |finished_for_test| instead (a scheduler and a callback are process-local
  // and cannot travel in a message).
  RoomActor(std::string room_id, std::string game_id, std::string options_json, uint64_t seed, int action_timeout_ms,
            int ai_delay_ms, int inter_hand_delay_ms, std::vector<RoomSeat> seats, std::shared_ptr<WindowTimer> timer,
            ex_actor::ActorRef<RoomOwner> owner = {}, TimeoutScheduler timeout_scheduler = {},
            RoomFinishedCallback finished_for_test = {});
  // Turns |alive_| off so a deadline already sitting in the timer stops addressing this room: the
  // timer cannot be cancelled and the lobby destroys the actor at game end, so a fired entry would
  // otherwise Send into freed memory.
  ~RoomActor() override;

  stdexec::task<void> Dispatch(RoomEnvelope envelope) override;
  stdexec::task<bool> TakeSeat(RoomSeat seat) override;

  // Live RoomActor instances, retired ones excluded. Not the lobby's room_count(): an actor spawned
  // but never registered is invisible there, so this is what can distinguish a leaked room.
  static size_t live_room_count();

  // Test seam, installed right after construction: production rooms report through |owner|.
  void SetFinishedCallbackForTest(RoomFinishedCallback callback) { finished_for_test_ = std::move(callback); }

  const std::string& room_id() const { return room_id_; }
  const std::string& game_id() const { return game_id_; }
  bool game_started() const { return game_started_; }
  int SeatOf(const std::string& player_id) const;
  const std::vector<RoomSeat>& seats() const { return seats_; }

 protected:
  // The single concrete binding: RoomActorFor<GameT> returns its own member.
  virtual Game& game() = 0;

  // ex_actor hands every actor its own ref right after construction; RoomActorFor forwards it. The
  // room needs it to post the deadline back to itself.
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
  // Stops the deadline closure and destroys the actor, which releases the recording and every seat
  // ref it holds.
  stdexec::task<void> Retire();

  // The one place a GameStep happens. Order is frozen by the protocol: frames go out first (a seat
  // must see the window before its deadline can fire), then deadlines are armed, then a settled
  // round is reported to the owner.
  stdexec::task<void> Run(GameStep step);
  stdexec::task<void> Deliver(const std::vector<Outbound>& outbounds);
  stdexec::task<void> DeliverSeat(int seat, std::string frame);
  stdexec::task<void> ReplyError(int seat, protocol::ErrorCode code, std::string_view message);
  void ReportResult(const GameResult& result);

  // One call before each delivery, so the part holds exactly the bytes that were sent.
  void RecordStep(const GameStep& step);
  // Serializes the settled hand and posts it to ReplayActor; the disk is the store's, never the
  // room's. Then the recorder is dropped so the next hand starts a fresh part.
  void WriteReplay(const GameResult& result);

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

  // ---- replay recording (replay.h) ----
  // Whether to record is whether a store exists, so nothing to copy in here.
  // |replay_started_ms_| anchors each step's `t` (pacing only); every hand part shares the origin of
  // the match's first hand.
  std::unique_ptr<ReplayRecorder> recorder_;
  int64_t replay_started_ms_ = 0;
  // Hand parts written so far, i.e. the next part's index. Counted here rather than in the recorder
  // so the name stays stable across the per-hand recorder reset.
  int replay_part_index_ = 0;
  // False once the room is being destroyed; co-owned because the timer's closure reads it after the
  // room is gone.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace mahjong
