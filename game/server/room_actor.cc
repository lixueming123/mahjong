#include "room_actor.h"

#include <algorithm>
#include <cassert>
#include <exception>
#include <utility>

#include <spdlog/spdlog.h>

#include "actor_scope.h"
#include "overloaded.h"
#include "protocol.h"

namespace mahjong {

RoomActor::RoomActor(std::string room_id, std::string game_id, std::string options_json, uint64_t seed,
                     int action_timeout_ms, int inter_hand_delay_ms, std::vector<RoomSeat> seats,
                     std::shared_ptr<WindowTimer> timer, ex_actor::ActorRef<RoomOwner> owner,
                     TimeoutScheduler timeout_scheduler, RoomFinishedCallback finished_for_test)
    : room_id_(std::move(room_id)),
      game_id_(std::move(game_id)),
      options_json_(std::move(options_json)),
      seed_(seed),
      action_timeout_ms_(action_timeout_ms),
      inter_hand_delay_ms_(inter_hand_delay_ms),
      seats_(std::move(seats)),
      timer_(std::move(timer)),
      timeout_scheduler_(std::move(timeout_scheduler)),
      finished_for_test_(std::move(finished_for_test)),
      owner_(std::move(owner)) {
  // Seat number == index everywhere in this class; make that true even if the caller handed the
  // seats over in another order.
  std::sort(seats_.begin(), seats_.end(), [](const RoomSeat& lhs, const RoomSeat& rhs) { return lhs.seat < rhs.seat; });
  // The lobby holds this actor as `ActorRef<RoomEndpoint>` and the framework invokes the method
  // pointer through a static_cast to the method's class (include/ex_actor/internal/actor.h),
  // which is only correct while RoomEndpoint is the first base — it does not check. Fail loudly
  // here rather than corrupt memory if someone ever adds a base in front of it.
  assert(static_cast<void*>(static_cast<RoomEndpoint*>(this)) == static_cast<void*>(this));
}

void RoomActor::BindRuntime(ex_actor::ActorRef<RoomEndpoint> self) {
  self_ref_ = std::move(self);
  // PROTOCOL.md §0.5: the production scheduler is the window timer plus the room's own ref. A
  // room without a timer (the unit test) keeps whatever scheduler it was constructed with.
  if (timer_ == nullptr || timeout_scheduler_) {
    return;
  }
  std::shared_ptr<WindowTimer> timer = timer_;
  ex_actor::ActorRef<RoomEndpoint> ref = self_ref_;
  timeout_scheduler_ = [timer, ref](int64_t delay_ms, uint64_t token) {
    timer->Schedule(delay_ms, [ref, token]() {
      ActorSendScope().spawn(ref.Send<&RoomEndpoint::Dispatch>(RoomEnvelope::WindowTimeout(token)));
    });
  };
}

int RoomActor::SeatOf(const std::string& player_id) const {
  for (const RoomSeat& seat : seats_) {
    if (!seat.is_bot && seat.player.player_id == player_id) {
      return seat.seat;
    }
  }
  return -1;
}

GameContext RoomActor::Context() const {
  GameContext context;
  context.room_id = room_id_;
  context.seed = seed_;
  context.action_timeout_ms = action_timeout_ms_;
  context.inter_hand_delay_ms = inter_hand_delay_ms_;
  context.options_json = options_json_;
  context.seats.reserve(seats_.size());
  for (const RoomSeat& seat : seats_) {
    context.seats.push_back(GameSeat {.seat = seat.seat, .is_bot = seat.is_bot, .player_id = seat.player.player_id,
                                      .name = seat.player.name});
  }
  return context;
}

// The room's message entry point (room_endpoint.h; `TakeSeat` takes the same gate for its own
// entry). Every handler below runs with the room to itself — including across a suspension, which
// delivery has: the framework would otherwise hand this actor the next message the moment a
// handler awaited something.
//
// The visitor returns the task Dispatch awaits rather than being a coroutine itself: a coroutine
// per arm would allocate a frame for every message.
stdexec::task<void> RoomActor::Dispatch(RoomEnvelope envelope) {
  AsyncGate::Guard guard = co_await gate_.Acquire();
  co_await std::visit(
      overloaded {
          [this](RoomEnvelope::ReadyMsg&& m) { return Ready(std::move(m.player)); },
          [this](RoomEnvelope::SyncMsg&& m) { return Sync(std::move(m.player)); },
          [this](RoomEnvelope::GameFrameMsg&& m) { return HandleGameFrame(std::move(m.player), std::move(m.frame)); },
          [this](RoomEnvelope::WindowTimeoutMsg&& m) { return OnTimeout(m.token); },
          [this](RoomEnvelope::PlayerDisconnectedMsg&& m) {
            return OnPlayerDisconnected(std::move(m.player), std::move(m.conn));
          },
          [this](RoomEnvelope::IdleTimeoutMsg&& m) { return OnIdleTimeout(std::move(m.player)); },
          [this](RoomEnvelope::ReconnectedMsg&& m) {
            return OnReconnected(std::move(m.player), std::move(m.endpoint));
          },
      },
      std::move(envelope.msg));
}

// The second public entry (the request/response counterpart of a TakeSeat message): it takes the
// same gate, so a seat change can never interleave with a deal.
stdexec::task<bool> RoomActor::TakeSeat(RoomSeat seat) {
  AsyncGate::Guard guard = co_await gate_.Acquire();
  co_return TakeSeatLocked(std::move(seat));
}

bool RoomActor::TakeSeatLocked(RoomSeat seat) {
  if (game_started_ || seat.seat < 0 || seat.seat >= static_cast<int>(seats_.size())) {
    return false;
  }
  RoomSeat& slot = seats_[static_cast<size_t>(seat.seat)];
  // A human seat can only be replaced by its own player, or turned back into a bot.
  if (!seat.is_bot && !slot.is_bot && slot.player.player_id != seat.player.player_id) {
    return false;
  }
  slot = std::move(seat);
  return true;
}

// ---------------------------------------------------------------------------------------
// The one place a step happens
// ---------------------------------------------------------------------------------------

stdexec::task<void> RoomActor::Run(GameStep step) {
  co_await Deliver(step.outbounds);
  if (step.deadline_ms > 0) {
    if (timeout_scheduler_) {
      timeout_scheduler_(step.deadline_ms, step.token);
    } else {
      // A room without a scheduler cannot enforce a deadline; say so once rather than silently
      // freezing the table. Production installs one in BindRuntime, and the unit test injects a
      // capturing one.
      spdlog::error("room {}: no deadline scheduler; the armed deadline is dropped", room_id_);
    }
  }
  if (step.result.has_value()) {
    ReportResult(*step.result);
  }
}

stdexec::task<void> RoomActor::Deliver(const std::vector<Outbound>& outbounds) {
  for (const Outbound& outbound : outbounds) {
    if (outbound.recipients.empty()) {
      // "everyone with a wire". Sequential on purpose: a step is one state transition and the
      // frames of a step are independent, so there is nothing to gain from fanning out here —
      // and a sequential loop keeps the delivery order (and therefore a golden transcript)
      // deterministic.
      for (int seat = 0; seat < static_cast<int>(seats_.size()); ++seat) {
        co_await DeliverSeat(seat, outbound.frame);
      }
      continue;
    }
    for (const int seat : outbound.recipients) {
      co_await DeliverSeat(seat, outbound.frame);
    }
  }
}

stdexec::task<void> RoomActor::DeliverSeat(int seat, std::string frame) {
  if (seat < 0 || seat >= static_cast<int>(seats_.size())) {
    co_return;
  }
  const RoomSeat& room_seat = seats_[static_cast<size_t>(seat)];
  if (frame.empty() || room_seat.endpoint.IsEmpty()) {
    co_return;  // a bot, or a seat with no wire
  }
  co_await room_seat.endpoint.Send<&SeatEndpoint::Dispatch>(PlayerEnvelope::SeatFrame(std::move(frame)));
}

stdexec::task<void> RoomActor::ReplyError(int seat, protocol::ErrorCode code, std::string_view message) {
  // PROTOCOL §7.2: the disconnect quota counts every error answered to a connection, so a
  // rejected room-level action is accounted too. The frame and the quota travel in ONE message,
  // which is what keeps "the frame is already queued" true before a quota can ask for a close.
  // ROOM_NOT_FOUND and INTERNAL are in neither bucket.
  QuotaKind quota = QuotaKind::kNone;
  if (code == protocol::ErrorCode::kBadRequest) {
    quota = QuotaKind::kBadFrame;
  } else if (code == protocol::ErrorCode::kNotYourTurn || code == protocol::ErrorCode::kNoPendingAction ||
             code == protocol::ErrorCode::kIllegalAction || code == protocol::ErrorCode::kUnknownTile) {
    quota = QuotaKind::kIllegalOp;
  }
  if (seat < 0 || seat >= static_cast<int>(seats_.size())) {
    co_return;
  }
  const RoomSeat& room_seat = seats_[static_cast<size_t>(seat)];
  if (room_seat.endpoint.IsEmpty()) {
    co_return;  // no wire to answer on
  }
  co_await room_seat.endpoint.Send<&SeatEndpoint::Dispatch>(
      PlayerEnvelope::SeatError(protocol::ErrorFrame(code, message), quota));
}

void RoomActor::ReportResult(const GameResult& result) {
  if (!finished_for_test_ && owner_.IsEmpty()) {
    return;
  }
  // ACCOUNT_DESIGN.md §4.2/§4.3: the owner gets the outcome per seat together with the identity
  // this room has held since the deal. It cannot come from the lobby's registry, whose entry for
  // this seat is already gone if the player disconnected mid-game (§8.5 trusts the seat and keeps
  // the room playing to game_end).
  GameSettlement finished;
  finished.room_id = room_id_;
  finished.reason = result.reason;
  finished.game_id = game_id_;
  // C10：match_over = 「本场到此为止」（match_end 已/即将下发）；lobby 只据此 arm 回收。
  finished.match_over = result.match_over;
  // B3：场终快照随 match_over 的那次通知带出，lobby 据此写 match_results。
  finished.hands_played = result.hands_played;
  finished.dealer_hands = result.dealer_hands;
  finished.totals = result.totals;
  finished.final_rank = result.final_rank;
  finished.seats.reserve(seats_.size());
  for (const RoomSeat& room_seat : seats_) {
    SettledSeat entry;
    entry.player = room_seat.player;
    entry.is_bot = room_seat.is_bot;
    entry.winner = room_seat.seat == result.winner_seat;
    entry.delta_score = result.delta[static_cast<size_t>(room_seat.seat)];
    finished.seats.push_back(std::move(entry));
  }
  if (finished_for_test_) {
    try {
      finished_for_test_(finished);
    } catch (const std::exception& e) {
      spdlog::error("room {}: the game-end notification failed: {}", room_id_, e.what());
    }
    return;
  }
  // Fire and forget, exactly like the old synchronous callback: the lobby records the history
  // and, on match_over, arms the reclaim. Sending it (not calling a std::function) is what lets
  // the message be serialized if the lobby is ever on another node.
  ActorSendScope().spawn(owner_.Send<&RoomOwner::OnRoomFinished>(std::move(finished)));
}

// ---------------------------------------------------------------------------------------
// Envelope handlers
// ---------------------------------------------------------------------------------------

stdexec::task<void> RoomActor::Ready(PlayerInfo player) {
  if (SeatOf(player.player_id) < 0) {
    co_return;
  }
  if (game_started_) {
    spdlog::warn("room {}: ready from {} ignored, the match already started", room_id_, player.player_id);
    co_return;
  }
  // The room is created with every seat already filled (bots take the rest), so the first ready
  // starts the match: there is no separate "everyone is ready" round trip (§1.4.2).
  game_started_ = true;
  co_await Run(game().Setup(Context()));
}

stdexec::task<void> RoomActor::Sync(PlayerInfo player) {
  const int seat = SeatOf(player.player_id);
  if (seat < 0) {
    co_return;
  }
  if (!game_started_) {
    co_await ReplyError(seat, protocol::ErrorCode::kInternal, "the game has not started yet");
    co_return;
  }
  co_await Run(game().Sync(seat));
}

stdexec::task<void> RoomActor::HandleGameFrame(PlayerInfo player, json::Value frame) {
  const int seat = SeatOf(player.player_id);
  if (seat < 0 || !game_started_) {
    co_return;
  }
  // §6.3: the room owns "is this your move / is the window still open / is that tile yours"; the
  // player owns the wire shape. All of it lives in Validate, which must not mutate — including
  // the "a reply for a window that already moved on is dropped silently" case (§6.3), which is
  // the reason there are three outcomes and not two.
  const GameVerdict verdict = game().Validate(seat, frame);
  if (verdict.kind == GameVerdict::Kind::kDrop) {
    co_return;
  }
  if (verdict.kind == GameVerdict::Kind::kRefuse) {
    co_await ReplyError(seat, verdict.code, verdict.message);
    co_return;
  }
  co_await Run(game().Apply(seat, frame));
}

stdexec::task<void> RoomActor::OnTimeout(uint64_t token) {
  co_await Run(game().OnTimeout(token));
}

stdexec::task<void> RoomActor::OnPlayerDisconnected(PlayerInfo player, ex_actor::ActorRef<SeatEndpoint> conn) {
  const int seat = SeatOf(player.player_id);
  if (seat < 0 || seats_[static_cast<size_t>(seat)].disconnected) {
    co_return;
  }
  // A close that arrives after a reconnect belongs to the old socket; the seat is already wired
  // to the new one and must not be marked disconnected again.
  if (!conn.IsEmpty() && seats_[static_cast<size_t>(seat)].endpoint != conn) {
    co_return;
  }
  seats_[static_cast<size_t>(seat)].disconnected = true;
  spdlog::info("room {}: seat {} lost its wire; its window keeps the countdown", room_id_, seat);
  // GAME_DESIGN §5.6.2 / PROTOCOL v2.3 §8.5: a disconnect does NOT play for the seat. The open
  // window stays unanswered until its deadline, so a reconnect inside the countdown still owns
  // the decision. There is nothing to run here — and that is a rule, not an omission.
}

stdexec::task<void> RoomActor::OnIdleTimeout(PlayerInfo player) {
  co_await OnPlayerDisconnected(std::move(player), /*conn=*/ {});
}

stdexec::task<void> RoomActor::OnReconnected(PlayerInfo player, ex_actor::ActorRef<SeatEndpoint> endpoint) {
  const int seat = SeatOf(player.player_id);
  if (seat < 0) {
    co_return;
  }
  seats_[static_cast<size_t>(seat)].endpoint = std::move(endpoint);
  seats_[static_cast<size_t>(seat)].disconnected = false;
  spdlog::info("room {}: seat {} reconnected", room_id_, seat);
  if (!game_started_) {
    // The lobby already sent room_joined. A pending Ready will deal and broadcast game_start to
    // the newly installed endpoint on its own.
    co_return;
  }
  // PROTOCOL v2.3 §8.5: the replayed frames are the game's business (round metadata first, then
  // the authoritative board) — the room only knows which seat asked.
  co_await Run(game().Reconnect(seat));
}

}  // namespace mahjong
