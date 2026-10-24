#include "lobby.h"

#include <exception>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "actor_scope.h"
#include "protocol.h"

namespace ex = stdexec;

namespace mahjong {
namespace {

// The four seats of a fresh room (PROTOCOL.md §4.2, GAME_DESIGN §2.2 ③): seat 0 is the human
// that owns the room and carries the wire, seats 1..3 are the server-side AI seats.
std::vector<RoomSeat> BuildInitialSeats(const PlayerInfo& player, const ex_actor::ActorRef<SeatEndpoint>& conn) {
  std::vector<RoomSeat> seats;
  seats.reserve(kSeatCount);
  RoomSeat human;
  human.seat = 0;
  human.player = player;
  human.is_bot = false;
  human.endpoint = conn;
  seats.push_back(std::move(human));
  for (int seat = 1; seat < kSeatCount; ++seat) {
    RoomSeat bot;
    bot.seat = seat;
    bot.player.player_id = "bot-" + std::to_string(seat);
    bot.player.name = "AI-" + std::to_string(seat);
    bot.is_bot = true;
    seats.push_back(std::move(bot));
  }
  return seats;
}

}  // namespace

std::string BuildRoomJoinedFrame(const std::string& room_id, int seat, const std::vector<RoomSeat>& seats) {
  json::Value frame = json::Value::Obj();
  frame.Set("type", json::Value::Str("room_joined"));
  frame.Set("room_id", json::Value::Str(room_id));
  frame.Set("seat", json::Value::Int(seat));
  json::Value seat_array = json::Value::Arr();
  for (const RoomSeat& room_seat : seats) {
    json::Value entry = json::Value::Obj();
    entry.Set("seat", json::Value::Int(room_seat.seat));
    entry.Set("name", json::Value::Str(room_seat.player.name));
    entry.Set("is_bot", json::Value::Bool(room_seat.is_bot));
    seat_array.Append(std::move(entry));
  }
  frame.Set("seats", std::move(seat_array));
  return frame.Dump();
}

LobbyActor::LobbyActor(uint64_t seed, int action_timeout_ms, std::shared_ptr<WindowTimer> timer,
                       int game_end_reclaim_ms, DbService db)
    : seed_(seed),
      action_timeout_ms_(action_timeout_ms),
      game_end_reclaim_ms_(game_end_reclaim_ms),
      timer_(std::move(timer)),
      db_(std::move(db)) {}

// Seat-facing replies. The lobby never touches a Connection any more: it messages the seat's
// PlayerActor and the frame lands in that connection's outbox (PROTOCOL §7.2's quota is
// charged by the same message when the error is a BAD_REQUEST). Awaiting them keeps the old
// synchronous behaviour: when a lobby handler returns, its frames are already queued.
stdexec::task<void> LobbyActor::ReplyFrame(const ex_actor::ActorRef<SeatEndpoint>& conn, std::string frame) {
  if (conn.IsEmpty()) {
    co_return;  // no wire (the unit test's seatless players)
  }
  co_await conn.SendLocal<&SeatEndpoint::Dispatch>(PlayerEnvelope::SeatFrame(std::move(frame)));
}

stdexec::task<void> LobbyActor::ReplyError(const ex_actor::ActorRef<SeatEndpoint>& conn, std::string_view code,
                                        std::string_view message) {
  if (conn.IsEmpty()) {
    co_return;
  }
  const QuotaKind quota = code == protocol::kCodeBadRequest ? QuotaKind::kBadFrame : QuotaKind::kNone;
  co_await conn.SendLocal<&SeatEndpoint::Dispatch>(
      PlayerEnvelope::SeatError(protocol::ErrorFrame(code, message), quota));
}

stdexec::task<void> LobbyActor::SetSelfRef(ex_actor::ActorRef<LobbyActor> self) {
  AsyncGate::Guard guard = co_await gate_.Acquire();
  self_ref_ = std::move(self);
}

std::string LobbyActor::NextRoomId() {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "r-%04u", next_room_number_++);
  return std::string(buffer);
}

const LobbyActor::PlayerState* LobbyActor::FindPlayer(const std::string& player_id) const {
  auto it = players_.find(player_id);
  return it == players_.end() ? nullptr : &it->second;
}

ex_actor::ActorRef<RoomActor> LobbyActor::FindRoom(const std::string& room_id) const {
  auto it = rooms_.find(room_id);
  return it == rooms_.end() ? ex_actor::ActorRef<RoomActor>() : it->second;
}

stdexec::task<void> LobbyActor::CreateRoom(ex_actor::ActorRef<SeatEndpoint> conn, PlayerInfo player) {
  // Reserve the player, then drop the lock for Spawn. A second create_room sees `creating_`
  // and is rejected; a disconnect erases the reservation so the commit below does not register
  // a room whose owner is already gone.
  std::string room_id;
  uint64_t room_seed = 0;
  std::vector<RoomSeat> seats;
  std::shared_ptr<WindowTimer> timer;
  DbService db;
  ex_actor::ActorRef<LobbyActor> self;
  int reclaim_ms = 0;
  enum class Reject { kNone, kAlready, kNoSelf };
  Reject reject = Reject::kNone;
  {
    AsyncGate::Guard guard = co_await gate_.Acquire();
    const PlayerState* state = FindPlayer(player.player_id);
    if ((state != nullptr && !state->room_id.empty()) || creating_.contains(player.player_id)) {
      reject = Reject::kAlready;
    } else if (self_ref_.IsEmpty()) {
      // Without the way back the room could be spawned but never reclaimed: the reclaim's
      // SendLocal would throw on the timer thread. main() always installs the ref before the
      // gateway starts, so this only fires in a misconfigured process.
      reject = Reject::kNoSelf;
    } else {
      // Every room gets its own wall: the server-level seed alone would deal the very same
      // hand in every room of this process. The derivation stays a pure function of (base
      // seed, room index), so a fixed --seed remains fully reproducible.
      const uint32_t room_index = next_room_number_ - 1;
      room_id = NextRoomId();
      room_seed = DeriveRoomSeed(seed_, room_index);
      seats = BuildInitialSeats(player, conn);
      creating_[player.player_id] = room_id;
      timer = timer_;
      db = db_;
      self = self_ref_;
      reclaim_ms = game_end_reclaim_ms_;
    }
  }
  if (reject == Reject::kAlready) {
    co_await ReplyError(conn, protocol::kCodeBadRequest, "already in a room");
    co_return;
  }
  if (reject == Reject::kNoSelf) {
    spdlog::error("lobby: no self reference installed, refusing to create a room for {}", player.player_id);
    co_await ReplyError(conn, protocol::kCodeInternal, "failed to create room");
    co_return;
  }

  ex_actor::ActorRef<RoomActor> room;
  std::string spawn_error;
  try {
    room = co_await ex_actor::Spawn<RoomActor>(room_id, room_seed, action_timeout_ms_, seats, timer);
  } catch (const std::exception& e) {
    // Answering happens below: a `co_await` is not allowed inside a catch handler.
    spawn_error = e.what();
  }

  bool owned = false;
  {
    AsyncGate::Guard guard = co_await gate_.Acquire();
    auto pending = creating_.find(player.player_id);
    owned = pending != creating_.end() && pending->second == room_id;
    if (owned) {
      creating_.erase(pending);
    }
    if (owned && spawn_error.empty()) {
      players_[player.player_id] = PlayerState {room_id, conn};
      rooms_[room_id] = room;
      room_seats_[room_id] = seats;
      room_started_[room_id] = false;
    }
  }
  if (!spawn_error.empty()) {
    spdlog::error("lobby: failed to create a room for player {}: {}", player.player_id, spawn_error);
    if (owned) {
      co_await ReplyError(conn, protocol::kCodeInternal, "failed to create room");
    }
    co_return;
  }
  if (!owned) {
    // Disconnect won the race: drop the unregistered actor with `room`.
    spdlog::info("lobby: dropped room {} for player {}, the connection closed while it was spawning", room_id,
                 player.player_id);
    co_return;
  }

  // Install the window timeout scheduler: the delayed message carries the window seq and is
  // posted back into the room (PROTOCOL.md §0.5). Enqueued before any client frame can reach
  // the room, because room_joined is only sent below.
  ActorSendScope().spawn(room.SendLocal<&RoomActor::Dispatch>(
      RoomEnvelope::InstallTimeoutScheduler(TimeoutScheduler([room, timer](int64_t delay_ms, uint64_t window_seq) {
        if (timer == nullptr) {
          return;
        }
        timer->Schedule(delay_ms, [room, window_seq]() {
          ActorSendScope().spawn(room.SendLocal<&RoomActor::Dispatch>(RoomEnvelope::WindowTimeout(window_seq)));
        });
      }))));

  // PROTOCOL.md §1.4.5 (v1.6): the callback fires once per ROUND; the per-round match records
  // are written every time, but the reclaim is armed only when the match is over (game_end no
  // longer ends the room — the room survives across the rounds of one match, §0.4).
  ActorSendScope().spawn(room.SendLocal<&RoomActor::Dispatch>(RoomEnvelope::InstallFinishedCallback(
      RoomFinishedCallback([self, timer, room_id, reclaim_ms, db](const GameSettlement& settlement) {
        // ACCOUNT_DESIGN.md §4.1: one row per account-backed human seat, posted to DbActor and
        // then forgotten. The room promises this runs exactly once per round, and it never
        // retries, so a lost write can only cost a row, not a duplicate.
        if (!db.IsEmpty()) {
          for (const SettledSeat& seat : settlement.seats) {
            if (seat.is_bot || seat.player.account_id == 0) {
              continue;  // guests and bots have no history to keep (§4.2)
            }
            ActorSendScope().spawn(db.RecordMatch(MatchRecord {
                seat.player.account_id, settlement.room_id, settlement.reason, seat.delta_score, seat.winner}));
          }
        }
        // ACCOUNT_DESIGN.md v1.3 §4 item 7: the match summary is written ONCE per match (at
        // match_over), one row per account-backed seat — bots and guests have no history to
        // keep (§4.2). Fire-and-forget; a lost row is never a game error.
        if (settlement.match_over && !db.IsEmpty()) {
          for (const SettledSeat& seat : settlement.seats) {
            if (seat.is_bot || seat.player.account_id == 0) {
              continue;
            }
            const int seat_index = static_cast<int>(&seat - settlement.seats.data());
            MatchResultRecord result;
            result.account_id = seat.player.account_id;
            result.room_id = settlement.room_id;
            result.hands_played = settlement.hands_played;
            result.dealer_hands = settlement.dealer_hands[static_cast<size_t>(seat_index)];
            result.final_score = settlement.totals[static_cast<size_t>(seat_index)];
            result.final_rank = settlement.final_rank[static_cast<size_t>(seat_index)];
            ActorSendScope().spawn(db.RecordMatchResult(std::move(result)));
          }
        }
        // C10：局终（game_end）不回收；match_over（match_end 已下发）才 arm 30s 回收。
        if (!settlement.match_over || timer == nullptr) {
          return;
        }
        timer->Schedule(reclaim_ms,
                        [self, room_id]() { ActorSendScope().spawn(self.SendLocal<&LobbyActor::ReapRoom>(room_id)); });
      }))));

  spdlog::info("lobby: created room {} (wall seed {}) for player {} ({})", room_id, room_seed, player.player_id,
               player.name);
  if (!conn.IsEmpty()) {
    // Hand the connection its room first: from here gameplay frames go straight to the room
    // instead of back through this actor (player_actor.cc RouteGameFrame).
    co_await conn.SendLocal<&SeatEndpoint::Dispatch>(PlayerEnvelope::SetRoom(room));
    co_await ReplyFrame(conn, BuildRoomJoinedFrame(room_id, 0, seats));
  }
}

stdexec::task<void> LobbyActor::ListRooms(ex_actor::ActorRef<SeatEndpoint> conn) {
  json::Value rooms = json::Value::Arr();
  {
    for (const auto& [room_id, seats] : room_seats_) {
      const auto started = room_started_.find(room_id);
      if (started != room_started_.end() && started->second) {
        continue;
      }
      int players = 0;
      std::string host;
      for (const RoomSeat& seat : seats) {
        if (!seat.is_bot) {
          ++players;
          if (seat.seat == 0) {
            host = seat.player.name;
          }
        }
      }
      json::Value item = json::Value::Obj();
      item.Set("room_id", json::Value::Str(room_id));
      item.Set("host", json::Value::Str(host));
      item.Set("players", json::Value::Int(players));
      item.Set("seats", json::Value::Int(kSeatCount));
      rooms.Append(std::move(item));
    }
  }
  json::Value frame = json::Value::Obj();
  frame.Set("type", json::Value::Str("room_list"));
  frame.Set("rooms", std::move(rooms));
  co_await ReplyFrame(conn, frame.Dump());
}

stdexec::task<void> LobbyActor::JoinRoom(ex_actor::ActorRef<SeatEndpoint> conn, PlayerInfo player, std::string room_id) {
  std::string code;
  std::string message;
  RoomSeat claimed;
  std::vector<RoomSeat> snapshot;
  ex_actor::ActorRef<RoomActor> room;
  bool same_room = false;
  {
    AsyncGate::Guard guard = co_await gate_.Acquire();
    const PlayerState* state = FindPlayer(player.player_id);
    if (creating_.contains(player.player_id)) {
      code = protocol::kCodeBadRequest;
      message = "already in a room";
    } else if (state != nullptr && state->room_id == room_id) {
      same_room = true;
      snapshot = room_seats_[room_id];
    } else if (state != nullptr && !state->room_id.empty()) {
      code = protocol::kCodeBadRequest;
      message = "already in a room";
    } else if (const auto started = room_started_.find(room_id);
               !rooms_.contains(room_id) || (started != room_started_.end() && started->second)) {
      // Do not use operator[]: a missing room must not be inserted as "not started".
      const bool already_started = started != room_started_.end() && started->second;
      code = protocol::kCodeRoomNotFound;
      message = already_started ? "room already started" : "no such room: " + room_id;
    } else {
      std::vector<RoomSeat>& seats = room_seats_[room_id];
      int free = -1;
      for (const RoomSeat& seat : seats) {
        if (seat.is_bot) {
          free = seat.seat;
          break;
        }
      }
      if (free < 0) {
        code = protocol::kCodeRoomNotFound;
        message = "room is full";
      } else {
        claimed = seats[static_cast<size_t>(free)];
        claimed.player = player;
        claimed.is_bot = false;
        claimed.disconnected = false;
        claimed.endpoint = conn;
        seats[static_cast<size_t>(free)] = claimed;
        players_[player.player_id] = PlayerState {room_id, conn};
        snapshot = seats;
        room = rooms_[room_id];
      }
    }
  }
  if (!code.empty()) {
    co_await ReplyError(conn, code, message);
    co_return;
  }
  if (same_room) {
    int seat = 0;
    for (const RoomSeat& slot : snapshot) {
      if (!slot.is_bot && slot.player.player_id == player.player_id) {
        seat = slot.seat;
      }
    }
    co_await ReplyFrame(conn, BuildRoomJoinedFrame(room_id, seat, snapshot));
    co_return;
  }

  auto accepted = std::make_shared<bool>(false);
  co_await room.SendLocal<&RoomActor::Dispatch>(RoomEnvelope::TakeSeat(claimed, accepted));
  if (!*accepted) {
    {
      AsyncGate::Guard guard = co_await gate_.Acquire();
      players_.erase(player.player_id);
      std::vector<RoomSeat>& seats = room_seats_[room_id];
      if (claimed.seat >= 0 && claimed.seat < static_cast<int>(seats.size())) {
        RoomSeat bot;
        bot.seat = claimed.seat;
        bot.is_bot = true;
        bot.player.player_id = "bot-" + std::to_string(claimed.seat);
        bot.player.name = "AI-" + std::to_string(claimed.seat);
        seats[static_cast<size_t>(claimed.seat)] = bot;
      }
    }
    co_await ReplyError(conn, protocol::kCodeRoomNotFound, "room already started");
    co_return;
  }
  co_await conn.SendLocal<&SeatEndpoint::Dispatch>(PlayerEnvelope::SetRoom(room));
  co_await ReplyFrame(conn, BuildRoomJoinedFrame(room_id, claimed.seat, snapshot));
}

stdexec::task<void> LobbyActor::Ready(ex_actor::ActorRef<SeatEndpoint> conn, PlayerInfo player) {
  std::string error;
  ex_actor::ActorRef<RoomActor> room;
  {
    AsyncGate::Guard guard = co_await gate_.Acquire();
    const PlayerState* state = FindPlayer(player.player_id);
    if (state == nullptr || state->room_id.empty()) {
      error = "not in a room";
    } else {
      room = FindRoom(state->room_id);
      if (room.IsEmpty()) {
        error = "room is gone";
      } else {
        // All four seats are always occupied here, so `ready` starts the game immediately
        // (PROTOCOL.md §4.4). From now on a disconnect entrusts the seat instead of dropping
        // the room (§8.5).
        room_started_[state->room_id] = true;
      }
    }
  }
  if (!error.empty()) {
    co_await ReplyError(conn, protocol::kCodeRoomNotFound, error);
    co_return;
  }
  ActorSendScope().spawn(room.SendLocal<&RoomActor::Dispatch>(RoomEnvelope::Ready(std::move(player))));
}

stdexec::task<void> LobbyActor::Sync(ex_actor::ActorRef<SeatEndpoint> conn, PlayerInfo player) {
  std::string error;
  ex_actor::ActorRef<RoomActor> room;
  {
    AsyncGate::Guard guard = co_await gate_.Acquire();
    const PlayerState* state = FindPlayer(player.player_id);
    if (state == nullptr || state->room_id.empty()) {
      error = "not in a room";
    } else {
      room = FindRoom(state->room_id);
      if (room.IsEmpty()) {
        error = "room is gone";
      }
    }
  }
  if (!error.empty()) {
    co_await ReplyError(conn, protocol::kCodeRoomNotFound, error);
    co_return;
  }
  ActorSendScope().spawn(room.SendLocal<&RoomActor::Dispatch>(RoomEnvelope::Sync(std::move(player))));
}

stdexec::task<void> LobbyActor::OnConnectionClosed(PlayerInfo player) {
  AsyncGate::Guard guard = co_await gate_.Acquire();
  // A create still in Spawn has no players_ entry yet. Dropping the reservation makes that
  // commit refuse the room.
  creating_.erase(player.player_id);
  auto it = players_.find(player.player_id);
  if (it == players_.end()) {
    co_return;
  }
  std::string room_id = it->second.room_id;
  players_.erase(it);
  if (room_id.empty()) {
    spdlog::info("lobby: player {} left the lobby", player.player_id);
    co_return;
  }
  ex_actor::ActorRef<RoomActor> room = FindRoom(room_id);
  if (room_started_[room_id]) {
    if (!room.IsEmpty()) {
      // PROTOCOL.md §8.5: during a game the seat becomes entrusted and the round keeps going.
      ActorSendScope().spawn(room.SendLocal<&RoomActor::Dispatch>(RoomEnvelope::PlayerDisconnected(player)));
    }
    spdlog::info("lobby: player {} disconnected mid-game, room {} keeps playing (seat entrusted)", player.player_id,
                 room_id);
    co_return;
  }
  auto roster = room_seats_.find(room_id);
  int seat_index = -1;
  if (roster != room_seats_.end()) {
    for (const RoomSeat& seat : roster->second) {
      if (!seat.is_bot && seat.player.player_id == player.player_id) {
        seat_index = seat.seat;
      }
    }
  }
  // A joiner leaving before the deal frees the seat. The owner leaving drops the room.
  if (seat_index > 0 && roster != room_seats_.end()) {
    RoomSeat bot;
    bot.seat = seat_index;
    bot.is_bot = true;
    bot.player.player_id = "bot-" + std::to_string(seat_index);
    bot.player.name = "AI-" + std::to_string(seat_index);
    roster->second[static_cast<size_t>(seat_index)] = bot;
    if (!room.IsEmpty()) {
      ActorSendScope().spawn(room.SendLocal<&RoomActor::Dispatch>(RoomEnvelope::TakeSeat(bot, nullptr)));
    }
    spdlog::info("lobby: player {} left room {} before it started", player.player_id, room_id);
    co_return;
  }
  rooms_.erase(room_id);
  room_seats_.erase(room_id);
  room_started_.erase(room_id);
  spdlog::info("lobby: player {} disconnected, room {} dropped", player.player_id, room_id);
}

stdexec::task<size_t> LobbyActor::room_count() {
  AsyncGate::Guard guard = co_await gate_.Acquire();
  co_return rooms_.size();
}

stdexec::task<void> LobbyActor::ReapRoom(std::string room_id) {
  // Drop the registry under the lock, then close sockets without it. A close awaits the
  // connection actor; holding the lobby lock across that would stall every other lobby message.
  std::vector<ex_actor::ActorRef<SeatEndpoint>> closing;
  {
    AsyncGate::Guard guard = co_await gate_.Acquire();
    auto room_it = rooms_.find(room_id);
    if (room_it == rooms_.end()) {
      co_return;  // already reclaimed (or never registered)
    }
    for (auto it = players_.begin(); it != players_.end();) {
      if (it->second.room_id != room_id) {
        ++it;
        continue;
      }
      if (!it->second.conn.IsEmpty()) {
        closing.push_back(it->second.conn);
      }
      it = players_.erase(it);
    }
    rooms_.erase(room_it);
    room_seats_.erase(room_id);
    room_started_.erase(room_id);
  }
  // PROTOCOL.md §1.4.5: the game is long over, so neither the room nor the connection it was
  // holding is of any use. Dropping the last ActorRef also destroys the RoomActor and its
  // state; the connections are asked to close so the gateway frees the sockets.
  for (const ex_actor::ActorRef<SeatEndpoint>& conn : closing) {
    co_await conn.SendLocal<&SeatEndpoint::Dispatch>(PlayerEnvelope::Close("room reclaimed after game_end"));
  }
  spdlog::info("lobby: reclaimed room {} after game_end ({} connection(s) closed)", room_id, closing.size());
}

stdexec::task<void> LobbyActor::OnIdleTimeout(ex_actor::ActorRef<SeatEndpoint> conn, PlayerInfo player) {
  bool drop_connection = false;
  ex_actor::ActorRef<RoomActor> room;
  {
    AsyncGate::Guard guard = co_await gate_.Acquire();
    const PlayerState* state = FindPlayer(player.player_id);
    if (state == nullptr || state->room_id.empty()) {
      // PROTOCOL.md §1.6: in the lobby a 20s silence drops the connection. A create still in
      // Spawn has no players_ entry yet; closing the socket makes OnConnectionClosed cancel it.
      drop_connection = true;
    } else {
      room = FindRoom(state->room_id);
    }
  }
  if (drop_connection) {
    co_await conn.SendLocal<&SeatEndpoint::Dispatch>(PlayerEnvelope::Close("idle timeout in lobby"));
    co_return;
  }
  if (!room.IsEmpty()) {
    // In game the connection stays open and the seat becomes entrusted (§1.6/§6.4).
    ActorSendScope().spawn(room.SendLocal<&RoomActor::Dispatch>(RoomEnvelope::IdleTimeout(std::move(player))));
  }
}

}  // namespace mahjong
