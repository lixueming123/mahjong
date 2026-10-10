#include "room.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "clock.h"
#include "protocol.h"

namespace mahjong {
namespace {

// ---------------------------------------------------------------------------------------
// Frame building helpers (PROTOCOL.md §5). Every field and every frame type is taken from
// the frozen protocol; nothing new is invented here (§0.1).
// ---------------------------------------------------------------------------------------

json::Value TilesToJson(const std::vector<Tile>& tiles) {
  json::Value array = json::Value::Arr(tiles.size());
  for (Tile tile : tiles) {
    array.Append(json::Value::Str(FormatTile(tile)));
  }
  return array;
}

json::Value MeldsToJson(const std::vector<Meld>& melds) {
  json::Value array = json::Value::Arr(melds.size());
  for (const Meld& meld : melds) {
    json::Value entry = json::Value::Obj(2);  // type, tiles
    entry.Set("type", json::Value::Str(MeldTypeName(meld.type)));
    entry.Set("tiles", TilesToJson(meld.tiles));
    array.Append(std::move(entry));
  }
  return array;
}

json::Value OptionsToJson(const std::vector<Option>& options) {
  json::Value array = json::Value::Arr(options.size());
  for (const Option& option : options) {
    json::Value entry = json::Value::Obj(2);  // action, tiles
    entry.Set("action", json::Value::Str(option.action));
    entry.Set("tiles", TilesToJson(option.tiles));
    array.Append(std::move(entry));
  }
  return array;
}

std::string BuildGameStartFrame(int for_seat, int dealer, const std::vector<Tile>& hand) {
  json::Value frame = json::Value::Obj(4);  // type, seat, dealer, hand
  frame.Set("type", json::Value::Str("game_start"));
  frame.Set("seat", json::Value::Int(for_seat));
  frame.Set("dealer", json::Value::Int(dealer));
  frame.Set("hand", TilesToJson(hand));
  return frame.Dump();
}

std::string BuildDrawFrame(Tile tile, int seat, uint64_t seq) {
  json::Value frame = json::Value::Obj(4);  // type, tile, seat, seq
  frame.Set("type", json::Value::Str("draw"));
  frame.Set("tile", json::Value::Str(FormatTile(tile)));
  frame.Set("seat", json::Value::Int(seat));
  frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
  return frame.Dump();
}

std::string BuildDiscardEventFrame(int seat, Tile tile, uint64_t seq) {
  json::Value frame = json::Value::Obj(4);  // type, seat, tile, seq
  frame.Set("type", json::Value::Str("discard_event"));
  frame.Set("seat", json::Value::Int(seat));
  frame.Set("tile", json::Value::Str(FormatTile(tile)));
  frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
  return frame.Dump();
}

std::string BuildActionRequestFrame(uint64_t seq, int64_t deadline_ms, std::string_view window,
                                    const std::vector<Option>& options) {
  json::Value frame = json::Value::Obj(5);  // type, seq, deadline_ms, window, options
  frame.Set("type", json::Value::Str("action_request"));
  frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
  frame.Set("deadline_ms", json::Value::Int(deadline_ms));
  frame.Set("window", json::Value::Str(std::string(window)));
  frame.Set("options", OptionsToJson(options));
  return frame.Dump();
}

std::string BuildActionEventFrame(int seat, std::string_view action, const std::vector<Tile>& tiles, int from_seat,
                                  uint64_t seq) {
  json::Value frame = json::Value::Obj(6);  // type, seat, action, tiles, seq (+ from_seat)
  frame.Set("type", json::Value::Str("action_event"));
  frame.Set("seat", json::Value::Int(seat));
  frame.Set("action", json::Value::Str(std::string(action)));
  frame.Set("tiles", TilesToJson(tiles));
  if (from_seat >= 0) {  // a tsumo win MUST NOT carry from_seat (PROTOCOL.md §5.8)
    frame.Set("from_seat", json::Value::Int(from_seat));
  }
  frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
  return frame.Dump();
}

int ActionPriority(std::string_view action) {
  if (action == "win") return 4;
  if (action == "kong") return 3;
  if (action == "pong") return 2;
  if (action == "chow") return 1;
  return -1;  // pass / anything else: no claim
}

bool HandHasFace(const std::vector<Tile>& hand, Face face) {
  return std::any_of(hand.begin(), hand.end(), [face](Tile tile) { return FaceOf(tile) == face; });
}

void RemoveOneFace(std::vector<Tile>* hand, Face face) {
  auto it = std::find_if(hand->begin(), hand->end(), [face](Tile tile) { return FaceOf(tile) == face; });
  if (it != hand->end()) {
    hand->erase(it);
  }
}

// Removes and returns up to |count| physical tiles of |face| from the hand.
std::vector<Tile> TakeFaces(std::vector<Tile>* hand, Face face, int count) {
  std::vector<Tile> taken;
  for (int i = 0; i < count; ++i) {
    auto it = std::find_if(hand->begin(), hand->end(), [face](Tile tile) { return FaceOf(tile) == face; });
    if (it == hand->end()) {
      break;
    }
    taken.push_back(*it);
    hand->erase(it);
  }
  return taken;
}

MeldType MeldTypeForAction(std::string_view action) {
  if (action == "pong") return MeldType::kPong;
  if (action == "chow") return MeldType::kChow;
  return MeldType::kKong;
}

// Compares two tile collections by face: the physical copy index is not part of the wire
// protocol (PROTOCOL.md §4.6, RULESET.md §1.2).
bool SameFaceMultiset(const std::vector<Tile>& lhs, const std::vector<Tile>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  std::vector<Face> left;
  std::vector<Face> right;
  left.reserve(lhs.size());
  right.reserve(rhs.size());
  for (Tile tile : lhs) {
    left.push_back(FaceOf(tile));
  }
  for (Tile tile : rhs) {
    right.push_back(FaceOf(tile));
  }
  std::sort(left.begin(), left.end());
  std::sort(right.begin(), right.end());
  return left == right;
}

constexpr int kMaxPumpSteps = 20000;  // safety net against a stuck state machine

}  // namespace

int& AiDelayStorage() {
  static int value = kDefaultAiDelayMs;
  return value;
}

RoomActor::RoomActor(std::string room_id, uint64_t seed, int action_timeout_ms, std::vector<RoomSeat> seats,
                     std::shared_ptr<WindowTimer> timer, int ai_delay_ms)
    : room_id_(std::move(room_id)),
      seed_(seed),
      action_timeout_ms_(action_timeout_ms),
      seats_(std::move(seats)),
      timer_(std::move(timer)),
      ai_delay_ms_(ai_delay_ms < 0 ? AiActionDelayMs() : ai_delay_ms) {
  // Seat number == index everywhere in this class; make that true even if the caller handed
  // the seats over in another order.
  std::sort(seats_.begin(), seats_.end(), [](const RoomSeat& lhs, const RoomSeat& rhs) { return lhs.seat < rhs.seat; });
}

void SetAiActionDelayMs(int delay_ms) { AiDelayStorage() = delay_ms; }

int AiActionDelayMs() { return AiDelayStorage(); }

int RoomActor::SeatOf(const std::string& player_id) const {
  for (const RoomSeat& seat : seats_) {
    if (!seat.is_bot && seat.player.player_id == player_id) {
      return seat.seat;
    }
  }
  return -1;
}

// The room's only entry point (room.h). The gate is taken here and nowhere else, so every
// handler below runs with the room to itself — including across a suspension, which the room
// will start to have (see the Deliver note): the framework would otherwise hand this actor the
// next message the moment a handler awaited something.
exec::task<void> RoomActor::Dispatch(RoomEnvelope envelope) {
  AsyncGate::Guard guard = co_await gate_.Acquire();
  switch (envelope.kind) {
    case RoomEnvelope::Kind::kReady:
      co_await Ready(std::move(envelope.player));
      break;
    case RoomEnvelope::Kind::kSync:
      co_await Sync(std::move(envelope.player));
      break;
    case RoomEnvelope::Kind::kGameFrame:
      co_await HandleGameFrame(std::move(envelope.player), std::move(envelope.frame));
      break;
    case RoomEnvelope::Kind::kWindowTimeout:
      co_await OnWindowTimeout(envelope.token);
      break;
    case RoomEnvelope::Kind::kPlayerDisconnected:
      co_await OnPlayerDisconnected(std::move(envelope.player));
      break;
    case RoomEnvelope::Kind::kIdleTimeout:
      co_await OnIdleTimeout(std::move(envelope.player));
      break;
    case RoomEnvelope::Kind::kInstallTimeoutScheduler:
      SetTimeoutScheduler(std::move(envelope.scheduler));
      break;
    case RoomEnvelope::Kind::kInstallFinishedCallback:
      SetFinishedCallback(std::move(envelope.finished));
      break;
    case RoomEnvelope::Kind::kTakeSeat:
      TakeSeat(std::move(envelope.seat_slot), envelope.accepted);
      break;
  }
}

void RoomActor::SetTimeoutScheduler(TimeoutScheduler scheduler) { timeout_scheduler_ = std::move(scheduler); }

void RoomActor::SetFinishedCallback(RoomFinishedCallback callback) { finished_callback_ = std::move(callback); }

void RoomActor::TakeSeat(RoomSeat seat, const std::shared_ptr<bool>& accepted) {
  const auto mark = [&](bool ok) {
    if (accepted) {
      *accepted = ok;
    }
  };
  if (game_started_ || seat.seat < 0 || seat.seat >= kSeatCount) {
    mark(false);
    return;
  }
  RoomSeat& slot = seats_[static_cast<size_t>(seat.seat)];
  // A human seat can only be replaced by its own player, or turned back into a bot.
  if (!seat.is_bot && !slot.is_bot && slot.player.player_id != seat.player.player_id) {
    mark(false);
    return;
  }
  slot = std::move(seat);
  mark(true);
}

void RoomActor::NotifyFinished(const Settlement& settlement) {
  if (!finished_callback_) {
    return;
  }
  // ACCOUNT_DESIGN.md §4.2/§4.3: the owner gets the outcome per seat together with the
  // identity the room has held since the deal. It cannot come from the lobby's registry, whose
  // entry for this seat is already gone if the player disconnected mid-game (§8.5 trusts the
  // seat and keeps the room playing to game_end).
  GameSettlement finished;
  finished.room_id = room_id_;
  finished.reason = settlement.reason;
  finished.seats.reserve(kSeatCount);
  for (int seat = 0; seat < kSeatCount; ++seat) {
    const RoomSeat& room_seat = seats_[static_cast<size_t>(seat)];
    SettledSeat entry;
    entry.player = room_seat.player;
    entry.is_bot = room_seat.is_bot;
    entry.winner = seat == settlement.winner_seat;
    entry.delta_score = settlement.delta[static_cast<size_t>(seat)];
    finished.seats.push_back(std::move(entry));
  }
  try {
    finished_callback_(finished);
  } catch (const std::exception& e) {
    spdlog::error("room {}: the game-end notification failed: {}", room_id_, e.what());
  }
}

bool RoomActor::ScheduleAiAction(int64_t delay_ms) {
  if (!timeout_scheduler_) {
    spdlog::error("room {}: no window timeout scheduler installed for the AI delay; "
                  "deciding inline instead", room_id_);
    return false;
  }
  // Same mechanism as OpenWindowTimer(), different tag: the token carries the current seq
  // plus kAiPacingTokenBit, so the handler can tell the two apart and still drop a message
  // whose window moved on (PROTOCOL.md §0.5).
  timeout_scheduler_(delay_ms, MakeAiPacingToken(seq_));
  return true;
}

exec::task<bool> RoomActor::BotDeclaresTsumo(int seat) {
  if (!discard_window_win_check_) {
    co_return false;
  }
  const std::vector<Tile>& hand = hands_[static_cast<size_t>(seat)];
  const std::vector<Meld>& melds = melds_[static_cast<size_t>(seat)];
  const std::vector<Option> options = DiscardOptions(hand, melds);
  const Tile winning = TsumoWinningTile(seat);
  if (FindOption(options, "win") == nullptr || !IsWinningHand(hand, melds, winning, /*is_tsumo=*/true)) {
    co_return false;
  }
  discard_window_open_ = false;
  discard_window_win_check_ = false;
  drawn_tile_.reset();
  co_await EndWithWin(seat, -1, winning, /*tsumo=*/true);
  co_return true;
}

exec::task<bool> RoomActor::PlayAutomatedDiscard() {
  const int seat = current_seat_;
  ai_discard_pending_ = false;
  if (!discard_window_open_ || NeedsPlayerReply(seat)) {
    co_return false;  // the window moved on while the decision was pending
  }
  if (seats_[static_cast<size_t>(seat)].is_bot) {
    // A bot must declare the win it is offered instead of discarding it (t24).
    if (co_await BotDeclaresTsumo(seat)) {
      co_return false;  // the game is over
    }
    std::optional<Tile> tile =
        ChooseDiscard(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)], drawn_tile_);
    if (!tile.has_value()) {
      co_await EndWithDraw();
      co_return false;
    }
    discard_window_open_ = false;
    co_await ApplyDiscard(seat, *tile);
    co_return true;
  }
  // An entrusted (disconnected) human keeps the frozen §5.6.2 default action, immediately.
  std::optional<Tile> tile = DefaultDiscardFor(seat);
  if (!tile.has_value()) {
    co_await EndWithDraw();
    co_return false;
  }
  discard_window_open_ = false;
  co_await ApplyDiscard(seat, *tile);
  co_return true;
}

void RoomActor::ResolveAutomatedClaimAnswers() {
  ai_claim_pending_ = false;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (!claim_recipient_[static_cast<size_t>(seat)] || claim_answers_[static_cast<size_t>(seat)].answered ||
        NeedsPlayerReply(seat)) {
      continue;
    }
    ClaimAnswer answer;
    if (seats_[static_cast<size_t>(seat)].is_bot) {
      const Option choice = ChooseClaimAnswer(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)],
                                              claim_options_[static_cast<size_t>(seat)], claim_tile_);
      answer.action = choice.action.empty() ? "pass" : choice.action;
      answer.tiles = choice.tiles;
    } else if (const Option* pass = FindOption(claim_options_[static_cast<size_t>(seat)], "pass"); pass != nullptr) {
      answer.action = pass->action;
    } else {
      answer.action = "pass";
    }
    answer.answered = true;
    claim_answers_[static_cast<size_t>(seat)] = std::move(answer);
  }
}

bool RoomActor::HasPendingHumanWin() const {
  for (int seat = 0; seat < kSeatCount; ++seat) {
    const ClaimAnswer& answer = claim_answers_[static_cast<size_t>(seat)];
    if (answer.answered && answer.action == "win" && !seats_[static_cast<size_t>(seat)].is_bot) {
      return true;
    }
  }
  return false;
}

bool RoomActor::IsAutomated(int seat) const {
  const RoomSeat& room_seat = seats_[static_cast<size_t>(seat)];
  return room_seat.is_bot || room_seat.disconnected;
}

// ---------------------------------------------------------------------------------------
// Outbound delivery
// ---------------------------------------------------------------------------------------

exec::task<void> RoomActor::Deliver(std::array<std::string, kSeatCount> frames) {
  // PROTOCOL.md §5.12 asks for a parallel broadcast: a slow seat must not delay the other three,
  // and the room must not move on until every seat has been told. Each seat is a ConnectionActor
  // message now, so this really awaits four sends at once — the fixed arity is deliberate,
  // because `when_all` has no zero-argument form and the seat count is a constant anyway.
  co_await stdexec::when_all(DeliverSeat(0, std::move(frames[0])), DeliverSeat(1, std::move(frames[1])),
                             DeliverSeat(2, std::move(frames[2])), DeliverSeat(3, std::move(frames[3])));
}

exec::task<void> RoomActor::DeliverSeat(int seat, std::string frame) {
  const RoomSeat& room_seat = seats_[static_cast<size_t>(seat)];
  if (frame.empty() || room_seat.endpoint.IsEmpty()) {
    co_return;  // a bot, or a seat with no wire
  }
  co_await room_seat.endpoint.SendLocal<&SeatEndpoint::Dispatch>(ConnectionEnvelope::SeatFrame(std::move(frame)));
}

exec::task<void> RoomActor::ReplyFrame(int seat, std::string frame) {
  std::array<std::string, kSeatCount> frames;
  frames[static_cast<size_t>(seat)] = std::move(frame);
  co_await Deliver(std::move(frames));
}

exec::task<void> RoomActor::ReplyError(int seat, std::string_view code, std::string_view message) {
  // PROTOCOL §7.2: the disconnect quota counts every error answered to a connection, so a
  // rejected room-level action is accounted too. The frame and the quota travel in ONE message,
  // which is what keeps "the frame is already queued" true before a quota can ask for a close.
  // ROOM_NOT_FOUND and INTERNAL are in neither bucket.
  QuotaKind quota = QuotaKind::kNone;
  if (code == protocol::kCodeBadRequest) {
    quota = QuotaKind::kBadFrame;
  } else if (code == protocol::kCodeNotYourTurn || code == protocol::kCodeNoPendingAction ||
             code == protocol::kCodeIllegalAction || code == protocol::kCodeUnknownTile) {
    quota = QuotaKind::kIllegalOp;
  }
  const RoomSeat& room_seat = seats_[static_cast<size_t>(seat)];
  if (room_seat.endpoint.IsEmpty()) {
    co_return;  // no wire to answer on
  }
  co_await room_seat.endpoint.SendLocal<&SeatEndpoint::Dispatch>(
      ConnectionEnvelope::SeatError(protocol::ErrorFrame(code, message), quota));
}

std::array<std::string, kSeatCount> RoomActor::StateSyncFrames() const {
  std::array<std::string, kSeatCount> frames;
  // `seats[]` is the same for every recipient and is most of a frame's build cost, so it is
  // rendered once here and spliced into each frame instead of being rebuilt per seat. The hoist
  // is sound because BuildStateSync reads no mutable state and this loop never suspends: the
  // block cannot change while the four frames are being built. Add an await to this loop and
  // that stops being true. It stays lazy so a room with no wire at all (all bots) still does no
  // JSON work, exactly as before.
  std::string seats_json;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (seats_[static_cast<size_t>(seat)].endpoint.IsEmpty()) {
      continue;  // bots have no wire
    }
    if (seats_json.empty()) {
      seats_json = BuildSeatsArray().Dump();  // a rendered 4-entry array is never empty
    }
    frames[static_cast<size_t>(seat)] = BuildStateSync(seat, seats_json);
  }
  return frames;
}

json::Value RoomActor::BuildSeatsArray() const {
  json::Value seat_array = json::Value::Arr(kSeatCount);
  for (const RoomSeat& room_seat : seats_) {
    json::Value entry = json::Value::Obj(7);  // seat, name, is_bot, hand_count, discards, melds, disconnected
    entry.Set("seat", json::Value::Int(room_seat.seat));
    entry.Set("name", json::Value::Str(room_seat.player.name));
    entry.Set("is_bot", json::Value::Bool(room_seat.is_bot));
    // H6: the public per-seat view MUST NOT contain `hand`, only the tile count.
    entry.Set("hand_count", json::Value::Int(static_cast<int64_t>(hands_[static_cast<size_t>(room_seat.seat)].size())));
    entry.Set("discards", TilesToJson(discards_[static_cast<size_t>(room_seat.seat)]));
    entry.Set("melds", MeldsToJson(melds_[static_cast<size_t>(room_seat.seat)]));
    entry.Set("disconnected", json::Value::Bool(room_seat.disconnected));
    seat_array.Append(std::move(entry));
  }
  return seat_array;
}

std::string RoomActor::BuildStateSync(int for_seat) const { return BuildStateSync(for_seat, BuildSeatsArray().Dump()); }

std::string RoomActor::BuildStateSync(int for_seat, const std::string& seats_json) const {
  json::Value frame = json::Value::Obj(7);  // type, seq, remaining, current_seat, hand, melds, seats
  frame.Set("type", json::Value::Str("state_sync"));
  frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq_)));
  frame.Set("remaining", json::Value::Int(remaining_));
  frame.Set("current_seat", json::Value::Int(current_seat_));
  frame.Set("hand", TilesToJson(hands_[static_cast<size_t>(for_seat)]));
  frame.Set("melds", MeldsToJson(melds_[static_cast<size_t>(for_seat)]));
  // Spliced verbatim; |seats_json| is exactly what building the array in here would have written
  // (BuildSeatsArray().Dump()), which is why the frame bytes are unchanged (§5.9).
  frame.Set("seats", json::Value::RawJson(seats_json));
  return frame.Dump();
}

std::string RoomActor::BuildGameStart(int for_seat) const {
  return BuildGameStartFrame(for_seat, dealer_, hands_[static_cast<size_t>(for_seat)]);
}

std::string RoomActor::BuildGameEnd(const Settlement& settlement) const {
  json::Value frame = json::Value::Obj(4);  // type, reason, results (+ win)
  frame.Set("type", json::Value::Str("game_end"));
  frame.Set("reason", json::Value::Str(settlement.reason));

  json::Value results = json::Value::Arr(kSeatCount);
  for (int seat = 0; seat < kSeatCount; ++seat) {
    json::Value entry = json::Value::Obj(4);  // seat, delta_score, hand, win_type
    entry.Set("seat", json::Value::Int(seat));
    entry.Set("delta_score", json::Value::Int(settlement.delta[static_cast<size_t>(seat)]));
    if (seat == settlement.winner_seat) {
      entry.Set("hand", TilesToJson(settlement.winner_flat_hand));
      json::Value types = json::Value::Arr(settlement.winner_types.size());
      for (const std::string& type : settlement.winner_types) {
        types.Append(json::Value::Str(type));
      }
      entry.Set("win_type", std::move(types));
    } else {
      entry.Set("hand", json::Value::Arr());
      entry.Set("win_type", json::Value::Arr());
    }
    results.Append(std::move(entry));
  }
  frame.Set("results", std::move(results));

  if (settlement.winner_seat < 0) {
    frame.Set("win", json::Value::Null());
  } else {
    json::Value win = json::Value::Obj(8);  // seat, tile, from_seat, tsumo, tags, multiplier, per_payer, total
    win.Set("seat", json::Value::Int(settlement.winner_seat));
    win.Set("tile", json::Value::Str(FormatTile(settlement.winning_tile)));
    if (settlement.tsumo) {
      win.Set("from_seat", json::Value::Null());  // the field must exist for a tsumo (§5.10)
    } else {
      win.Set("from_seat", json::Value::Int(settlement.from_seat));
    }
    win.Set("tsumo", json::Value::Bool(settlement.tsumo));
    json::Value tags = json::Value::Arr(settlement.tags.size());
    for (const std::string& tag : settlement.tags) {
      tags.Append(json::Value::Str(tag));
    }
    win.Set("tags", std::move(tags));
    win.Set("multiplier", json::Value::Int(settlement.multiplier));
    win.Set("per_payer", json::Value::Int(settlement.per_payer));
    win.Set("total", json::Value::Int(settlement.total));
    frame.Set("win", std::move(win));
  }
  return frame.Dump();
}

exec::task<void> RoomActor::BroadcastSettlement(const Settlement& settlement) {
  std::array<std::string, kSeatCount> frames;
  const std::string end = BuildGameEnd(settlement);
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (!seats_[static_cast<size_t>(seat)].endpoint.IsEmpty()) {
      frames[static_cast<size_t>(seat)] = end;
    }
  }
  co_await Deliver(std::move(frames));
}

// ---------------------------------------------------------------------------------------
// Turn driving
// ---------------------------------------------------------------------------------------

uint64_t RoomActor::Transition() {
  // PROTOCOL.md §6.2 / GAME_DESIGN §4.3: seq advances once per window transition, and a
  // window opened by this transition carries the new value.
  ++seq_;
  return seq_;
}

exec::task<void> RoomActor::StartGame() {
  game_started_ = true;
  wall_ = BuildWall(seed_);
  Deal deal = DealTiles(wall_);
  hands_ = deal.hands;  // hands[0] is the dealer (14 tiles), the others 13
  wall_ = std::move(deal.wall);
  next_index_ = deal.next_index;
  remaining_ = deal.remaining;
  for (std::vector<Tile>& hand : hands_) {
    SortTiles(hand);
  }
  dealer_ = 0;
  current_seat_ = dealer_;
  seq_ = 0;
  Transition();  // the dealer's first discard window is seq 1 (GAME_DESIGN §4.2 step 1)

  std::array<std::string, kSeatCount> starts;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (!seats_[static_cast<size_t>(seat)].endpoint.IsEmpty()) {
      starts[static_cast<size_t>(seat)] = BuildGameStart(seat);
    }
  }
  co_await Deliver(std::move(starts));
  spdlog::info("room {}: game started (dealer={}, remaining={}, seq={})", room_id_, dealer_, remaining_, seq_);

  // Dealer's first discard window: no draw record, but a 14-tile hand may already be a win
  // (天和, GAME_DESIGN §4.2 step 2).
  co_await OpenDiscardWindow(/*win_check=*/true, std::nullopt);
  phase_ = Phase::kDiscardWindow;
  co_await Pump();
}

exec::task<void> RoomActor::Pump() {
  for (int step = 0; step < kMaxPumpSteps; ++step) {
    switch (phase_) {
      case Phase::kIdle:
      case Phase::kFinished: co_return;

      case Phase::kTurnStart: {
        if (remaining_ <= 0) {
          co_await EndWithDraw();
          co_return;
        }
        co_await DrawForCurrentSeat();
        break;  // -> kDiscardWindow
      }

      case Phase::kDiscardWindow: {
        const int seat = current_seat_;
        if (NeedsPlayerReply(seat)) {
          // The request was sent when the window opened; wait for the reply or for the
          // window timeout message (PROTOCOL.md §0.5).
          co_return;
        }
        // Two kinds of automated seats share this branch, and they deliberately differ:
        //   * a bot seat plays the tile-efficiency policy (t20): keep the hand whose shanten
        //     after the next draw is best — declared on its own draw, and paced by
        //     ai_delay_ms_ so a human can follow the table (GAME_DESIGN v1.4 §5.4);
        //   * an entrusted (disconnected) human seat keeps the frozen deterministic fallback
        //     of GAME_DESIGN §5.6.2 — play the tile just drawn, or the largest tile when the
        //     window has no draw record — always immediately: an entrusted seat represents a
        //     player who already waited, so the pacing must not apply to it.
        if (seats_[static_cast<size_t>(seat)].is_bot && ai_delay_ms_ > 0 &&
            (ai_discard_pending_ || ScheduleAiAction(ai_delay_ms_))) {
          ai_discard_pending_ = true;
          co_return;  // the decision arrives as a message; the actor keeps serving human frames
        }
        if (!co_await PlayAutomatedDiscard()) {
          co_return;
        }
        break;
      }

      case Phase::kClaimWindow: {
        if (claim_pending_humans_ > 0) {
          co_return;  // waiting for those seats' action or for their timeout message
        }
        if (ai_claim_pending_) {
          // GAME_DESIGN v1.4 §5.4: the bots' responses appear ai_delay_ms after the discard.
          // A human `win` is the top priority and no bot answer can change it, so the human
          // is served immediately instead of waiting the pacing out.
          if (!HasPendingHumanWin()) {
            co_return;
          }
          ResolveAutomatedClaimAnswers();
        }
        co_await ResolveClaimWindow();
        break;
      }
    }
  }
  spdlog::error("room {}: pump step limit reached (phase={})", room_id_, static_cast<int>(phase_));
}

exec::task<void> RoomActor::DrawForCurrentSeat() {
  const int seat = current_seat_;
  Tile tile = wall_[static_cast<size_t>(next_index_)];
  ++next_index_;
  remaining_ = kTileCount - next_index_;
  hands_[static_cast<size_t>(seat)].push_back(tile);
  SortTiles(hands_[static_cast<size_t>(seat)]);

  spdlog::debug("room {}: seat {} draws {} (remaining={}, seq={})", room_id_, seat, FormatTile(tile), remaining_, seq_);

  std::array<std::string, kSeatCount> draws;
  if (!seats_[static_cast<size_t>(seat)].endpoint.IsEmpty()) {
    draws[static_cast<size_t>(seat)] = BuildDrawFrame(tile, seat, seq_);
  }
  co_await Deliver(std::move(draws));
  co_await Deliver(StateSyncFrames());

  co_await OpenDiscardWindow(/*win_check=*/true, tile);
  phase_ = Phase::kDiscardWindow;
}

exec::task<void> RoomActor::OpenDiscardWindow(bool win_check, std::optional<Tile> drawn_tile) {
  drawn_tile_ = drawn_tile;
  discard_window_open_ = true;
  ai_discard_pending_ = false;  // a fresh window schedules its own pacing message
  discard_window_win_check_ = win_check;
  const int seat = current_seat_;
  if (!NeedsPlayerReply(seat)) {
    co_return;  // bots / entrusted seats act inside this same message: no frame, no timer
  }
  std::vector<Option> options;
  if (win_check) {
    options = DiscardOptions(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)]);
  }
  co_await ReplyFrame(seat, BuildActionRequestFrame(seq_, clock::EpochMs() + action_timeout_ms_, "discard", options));
  OpenWindowTimer();
}

exec::task<void> RoomActor::ApplyDiscard(int seat, Tile tile) {
  const uint64_t old_seq = seq_;
  RemoveOneFace(&hands_[static_cast<size_t>(seat)], FaceOf(tile));
  discards_[static_cast<size_t>(seat)].push_back(tile);
  drawn_tile_.reset();
  discard_window_open_ = false;
  discard_window_win_check_ = false;

  Transition();  // close the discard window; a claim window (if any) takes the new seq

  // One frame for the whole table (PROTOCOL.md §5.6: `discard_event` is broadcast and carries
  // nothing seat-specific), so it is built once and handed to every wired seat — the same shape
  // BroadcastSettlement already uses.
  std::array<std::string, kSeatCount> events;
  const std::string event = BuildDiscardEventFrame(seat, tile, old_seq);
  for (int target = 0; target < kSeatCount; ++target) {
    if (!seats_[static_cast<size_t>(target)].endpoint.IsEmpty()) {
      events[static_cast<size_t>(target)] = event;
    }
  }
  co_await Deliver(std::move(events));
  spdlog::debug("room {}: seat {} discards {} (seq={})", room_id_, seat, FormatTile(tile), old_seq);

  std::array<bool, kSeatCount> recipients {};
  std::array<std::vector<Option>, kSeatCount> options;
  bool any_claim = false;
  for (int other = 0; other < kSeatCount; ++other) {
    if (other == seat) {
      continue;
    }
    options[static_cast<size_t>(other)] =
        ClaimOptions(hands_[static_cast<size_t>(other)], melds_[static_cast<size_t>(other)], tile, remaining_,
                     /*is_next_player=*/other == (seat + 1) % kSeatCount);
    if (HasNonPassOption(options[static_cast<size_t>(other)])) {
      recipients[static_cast<size_t>(other)] = true;
      any_claim = true;
    }
  }

  if (!any_claim) {
    // GAME_DESIGN §4.2 step 7: nobody has a non-pass option, so the claim window opens and
    // closes right away (this consumes one seq, see the numbering in §4.3) and the next
    // player draws inside the new window.
    Transition();
    current_seat_ = (seat + 1) % kSeatCount;
    phase_ = Phase::kTurnStart;
    co_return;
  }
  co_await OpenClaimWindow(seat, tile, recipients, options);
}

exec::task<void> RoomActor::OpenClaimWindow(int from_seat, Tile tile, const std::array<bool, kSeatCount>& recipients,
                                const std::array<std::vector<Option>, kSeatCount>& options) {
  claim_open_ = true;
  claim_from_seat_ = from_seat;
  claim_tile_ = tile;
  claim_recipient_ = recipients;
  claim_options_ = options;
  claim_answers_ = {};
  claim_pending_humans_ = 0;
  ai_claim_pending_ = false;
  phase_ = Phase::kClaimWindow;
  bool ai_pending = false;

  std::array<std::string, kSeatCount> requests;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (!recipients[static_cast<size_t>(seat)]) {
      continue;
    }
    if (!NeedsPlayerReply(seat)) {
      // GAME_DESIGN v1.4 §5.4: a bot's meld/self-draw choice is its own decision, so it is
      // executed ai_delay_ms after the discard (still without consuming the 15s deadline:
      // an all-AI window keeps its own pacing timer only). An entrusted human seat is not an
      // AI: it records the frozen §5.6.2 default action immediately, exactly as the timeout
      // message would.
      // A bot only takes a meld that does not worsen the shanten of its hand (t21/t23).
      if (seats_[static_cast<size_t>(seat)].is_bot && ai_delay_ms_ > 0) {
        ai_pending = true;
        continue;
      }
      Option choice;
      if (seats_[static_cast<size_t>(seat)].is_bot) {
        choice = ChooseClaimAnswer(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)],
                                   options[static_cast<size_t>(seat)], tile);
      } else if (const Option* pass = FindOption(options[static_cast<size_t>(seat)], "pass"); pass != nullptr) {
        choice = *pass;
      } else {
        choice.action = "pass";
      }
      ClaimAnswer answer;
      answer.answered = true;
      answer.action = choice.action.empty() ? "pass" : choice.action;
      answer.tiles = choice.tiles;
      claim_answers_[static_cast<size_t>(seat)] = std::move(answer);
      continue;
    }
    ++claim_pending_humans_;
    if (!seats_[static_cast<size_t>(seat)].endpoint.IsEmpty()) {
      requests[static_cast<size_t>(seat)] = BuildActionRequestFrame(
          seq_, clock::EpochMs() + action_timeout_ms_, "claim", options[static_cast<size_t>(seat)]);
    }
  }
  co_await Deliver(std::move(requests));
  if (claim_pending_humans_ > 0) {
    OpenWindowTimer();
  }
  if (ai_pending) {
    if (ScheduleAiAction(ai_delay_ms_)) {
      ai_claim_pending_ = true;
    } else {
      // No scheduler (never in production): decide at once instead of leaving the window
      // waiting for a paced answer that can never arrive.
      ResolveAutomatedClaimAnswers();
    }
  }
}

Option RoomActor::ChooseClaimAnswer(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                    const std::vector<Option>& options, Tile claimed) {
  Option pass;
  if (const Option* declared = FindOption(options, "pass"); declared != nullptr) {
    pass = *declared;
  } else {
    pass.action = "pass";
  }
  if (const Option* win = FindOption(options, "win"); win != nullptr) {
    return *win;  // never leave a win on the table
  }

  const int before = Shanten(hand, melds);
  const Option* best = nullptr;
  int best_shanten = 0;

  // Pong is considered first, so it keeps a tie against any chow combination (the frozen
  // claim priority is win > kong > pong > chow > pass).
  if (const Option* pong = FindOption(options, "pong"); pong != nullptr) {
    const std::vector<Tile> consume(2, claimed);
    if (const std::optional<int> after = ShantenAfterMeld(hand, melds, claimed, consume, MeldType::kPong);
        after.has_value() && *after <= before) {
      best = pong;
      best_shanten = *after;
    }
  }
  // Every chow combination is evaluated; the lowest post-meld shanten wins, ties keep the
  // order of |options| (ClaimOptions is frozen, so this stays deterministic).
  for (const Option& option : options) {
    if (option.action != "chow") {
      continue;
    }
    const std::optional<int> after = ShantenAfterMeld(hand, melds, claimed, option.tiles, MeldType::kChow);
    if (!after.has_value() || *after > before) {
      continue;
    }
    if (best == nullptr || *after < best_shanten) {
      best = &option;
      best_shanten = *after;
    }
  }
  return best != nullptr ? *best : pass;
}

void RoomActor::ResetClaimWindow() {
  claim_open_ = false;
  ai_claim_pending_ = false;
  claim_from_seat_ = -1;
  claim_recipient_ = {};
  claim_options_ = {};
  claim_answers_ = {};
  claim_pending_humans_ = 0;
}

exec::task<void> RoomActor::ResolveClaimWindow() {
  int best_seat = -1;
  int best_priority = -1;
  int best_distance = kSeatCount;
  std::string best_action;
  std::vector<Tile> best_tiles;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (!claim_recipient_[static_cast<size_t>(seat)] || !claim_answers_[static_cast<size_t>(seat)].answered) {
      continue;
    }
    const ClaimAnswer& answer = claim_answers_[static_cast<size_t>(seat)];
    const int priority = ActionPriority(answer.action);
    if (priority < 0) {
      continue;  // pass
    }
    // GAME_DESIGN §5.5: win > kong > pong > chow > pass, and within a tier the seat closest
    // to the discarder in turn order wins (S+1, S+2, S+3).
    const int distance = (seat - claim_from_seat_ + kSeatCount) % kSeatCount;
    if (priority > best_priority || (priority == best_priority && distance < best_distance)) {
      best_seat = seat;
      best_priority = priority;
      best_distance = distance;
      best_action = answer.action;
      best_tiles = answer.tiles;
    }
  }

  const int from_seat = claim_from_seat_;
  const Tile tile = claim_tile_;
  ResetClaimWindow();

  if (best_seat < 0) {
    // Everybody passed: the next player draws inside the new window.
    Transition();
    current_seat_ = (from_seat + 1) % kSeatCount;
    phase_ = Phase::kTurnStart;
    co_return;
  }
  if (best_action == "win") {
    co_await EndWithWin(best_seat, from_seat, tile, /*tsumo=*/false);
    co_return;
  }
  co_await ApplyMeld(best_seat, best_action, best_tiles, from_seat, tile);
}

exec::task<void> RoomActor::ApplyMeld(int seat, const std::string& action, const std::vector<Tile>& client_tiles,
                                      int from_seat, Tile claimed_tile) {
  const Face claimed_face = FaceOf(claimed_tile);
  Meld meld;
  meld.type = MeldTypeForAction(action);
  std::vector<Tile> from_hand;
  if (action == "pong") {
    from_hand = TakeFaces(&hands_[static_cast<size_t>(seat)], claimed_face, 2);
  } else if (action == "kong") {
    from_hand = TakeFaces(&hands_[static_cast<size_t>(seat)], claimed_face, 3);
  } else {  // chow: the two submitted tiles are the ones leaving the hand
    for (Tile tile : client_tiles) {
      from_hand.push_back(tile);
      RemoveOneFace(&hands_[static_cast<size_t>(seat)], FaceOf(tile));
    }
  }
  meld.tiles = from_hand;
  meld.tiles.push_back(claimed_tile);
  SortTiles(meld.tiles);
  melds_[static_cast<size_t>(seat)].push_back(std::move(meld));
  if (from_seat >= 0 && !discards_[static_cast<size_t>(from_seat)].empty()) {
    discards_[static_cast<size_t>(from_seat)].pop_back();  // GAME_DESIGN §4.2 step 10
  }

  current_seat_ = seat;
  const uint64_t old_seq = seq_;
  Transition();  // close the claim window and open the melder's discard window (§4.3)
  phase_ = Phase::kDiscardWindow;

  // Broadcast to all four seats (§5.8) with no seat-specific field, so one build serves them all.
  std::array<std::string, kSeatCount> events;
  const std::string event = BuildActionEventFrame(seat, action, client_tiles, from_seat, old_seq);
  for (int target = 0; target < kSeatCount; ++target) {
    if (!seats_[static_cast<size_t>(target)].endpoint.IsEmpty()) {
      events[static_cast<size_t>(target)] = event;
    }
  }
  co_await Deliver(std::move(events));
  spdlog::info("room {}: seat {} {} {} from seat {} (seq {} -> {})", room_id_, seat, action, FormatTile(claimed_tile),
               from_seat, old_seq, seq_);

  std::optional<Tile> replacement;
  if (action == "kong") {
    if (remaining_ <= 0) {
      // No tile left for the replacement draw: the hand ends (GAME_DESIGN §4.2 step 12).
      co_await EndWithDraw();
      co_return;
    }
    Tile drawn = wall_[static_cast<size_t>(next_index_)];
    ++next_index_;
    remaining_ = kTileCount - next_index_;
    hands_[static_cast<size_t>(seat)].push_back(drawn);
    SortTiles(hands_[static_cast<size_t>(seat)]);
    replacement = drawn;
    std::array<std::string, kSeatCount> draws;
    if (!seats_[static_cast<size_t>(seat)].endpoint.IsEmpty()) {
      draws[static_cast<size_t>(seat)] = BuildDrawFrame(drawn, seat, seq_);
    }
    co_await Deliver(std::move(draws));
  }
  co_await Deliver(StateSyncFrames());
  co_await OpenDiscardWindow(/*win_check=*/action == "kong", replacement);
}

std::optional<Tile> RoomActor::DefaultDiscardFor(int seat) const {
  return DefaultDiscard(hands_[static_cast<size_t>(seat)], drawn_tile_);
}

Tile RoomActor::TsumoWinningTile(int seat) const {
  const std::vector<Tile>& hand = hands_[static_cast<size_t>(seat)];
  if (drawn_tile_.has_value()) {
    return *drawn_tile_;
  }
  const std::vector<Tile> sorted = SortedTiles(hand);
  return sorted.empty() ? Tile {} : sorted.back();
}

exec::task<void> RoomActor::EndWithWin(int winner_seat, int from_seat, Tile winning_tile, bool tsumo) {
  const uint64_t old_seq = seq_;
  if (!tsumo) {
    // Ron: the winning tile completes the winner's hand (it stays visible in the river).
    hands_[static_cast<size_t>(winner_seat)].push_back(winning_tile);
    SortTiles(hands_[static_cast<size_t>(winner_seat)]);
  }
  const std::vector<Tile>& concealed = hands_[static_cast<size_t>(winner_seat)];
  const ScoreResult score =
      ScoreWin(winner_seat, tsumo ? -1 : from_seat, concealed, melds_[static_cast<size_t>(winner_seat)]);

  Transition();  // close the window; no new window follows
  phase_ = Phase::kFinished;
  claim_open_ = false;
  discard_window_open_ = false;
  discard_window_win_check_ = false;
  drawn_tile_.reset();

  Settlement settlement;
  settlement.reason = tsumo ? "win_tsumo" : "win_ron";
  settlement.delta = score.delta;
  settlement.winner_seat = winner_seat;
  settlement.winner_flat_hand = concealed;
  for (const Meld& meld : melds_[static_cast<size_t>(winner_seat)]) {
    settlement.winner_flat_hand.insert(settlement.winner_flat_hand.end(), meld.tiles.begin(), meld.tiles.end());
  }
  SortTiles(settlement.winner_flat_hand);
  settlement.tags = WinTags(concealed, melds_[static_cast<size_t>(winner_seat)]);
  settlement.winner_types = settlement.tags;
  settlement.winner_types.emplace_back(tsumo ? "tsumo" : "ron");
  settlement.tsumo = tsumo;
  settlement.winning_tile = winning_tile;
  settlement.from_seat = tsumo ? -1 : from_seat;
  settlement.multiplier = score.multiplier;
  settlement.per_payer = score.per_payer;
  settlement.total = score.total;

  // Broadcast to all four seats (§5.8) with no seat-specific field, so one build serves them all.
  std::array<std::string, kSeatCount> events;
  const std::string event = BuildActionEventFrame(winner_seat, "win", {winning_tile}, tsumo ? -1 : from_seat, old_seq);
  for (int target = 0; target < kSeatCount; ++target) {
    if (!seats_[static_cast<size_t>(target)].endpoint.IsEmpty()) {
      events[static_cast<size_t>(target)] = event;
    }
  }
  co_await Deliver(std::move(events));
  co_await Deliver(StateSyncFrames());  // PROTOCOL.md §5.12: state_sync right before game_end
  co_await BroadcastSettlement(settlement);
  spdlog::info("room {}: game over (reason={}, winner={}, tile={}, M={}, total={}, seq {} -> {})", room_id_,
               settlement.reason, winner_seat, FormatTile(winning_tile), score.multiplier, score.total, old_seq, seq_);
  NotifyFinished(settlement);
}

exec::task<void> RoomActor::EndWithDraw() {
  Transition();
  phase_ = Phase::kFinished;
  claim_open_ = false;
  discard_window_open_ = false;
  discard_window_win_check_ = false;
  drawn_tile_.reset();

  const ScoreResult score = ScoreDraw();
  Settlement settlement;
  settlement.reason = "draw_exhausted";
  settlement.delta = score.delta;
  settlement.winner_seat = -1;

  co_await Deliver(StateSyncFrames());  // final authoritative snapshot before game_end
  co_await BroadcastSettlement(settlement);
  spdlog::info("room {}: game over (reason=draw_exhausted, remaining={}, seq={})", room_id_, remaining_, seq_);
  NotifyFinished(settlement);
}

// ---------------------------------------------------------------------------------------
// Windows, timers, client frames
// ---------------------------------------------------------------------------------------

void RoomActor::OpenWindowTimer() {
  if (!timeout_scheduler_) {
    spdlog::error("room {}: no window timeout scheduler installed", room_id_);
    return;
  }
  // The scheduled message carries the window it belongs to; if that window already moved on
  // when it arrives, OnWindowTimeout drops it (seq invalidation, PROTOCOL.md §0.5).
  timeout_scheduler_(action_timeout_ms_, seq_);
}

exec::task<void> RoomActor::OnWindowTimeout(uint64_t token) {
  // Two kinds of scheduled messages arrive here: a real window deadline (plain envelope seq)
  // and an AI pacing deadline (same seq, tagged with kAiDelayTokenBit). Both use the very
  // same invalidation rule — if the window moved on, seq_ changed and the message is dropped
  // without touching any state (PROTOCOL.md §0.5).
  const bool ai_pacing = IsAiPacingToken(token);
  const uint64_t window_seq = WindowSeqOfToken(token);
  if (window_seq != seq_) {
    spdlog::debug("room {}: stale {} for window {} (current {}) dropped", room_id_,
                  ai_pacing ? "AI pacing deadline" : "timeout", window_seq, seq_);
    co_return;
  }
  if (ai_pacing) {
    // GAME_DESIGN v1.4 §5.4: the bot decision that was paced. A human reply, a window
    // deadline, or the end of the round all bump seq_, which is why the check above is
    // enough to keep the two mechanisms from ever interleaving.
    if (phase_ == Phase::kDiscardWindow && discard_window_open_ && ai_discard_pending_) {
      if (!co_await PlayAutomatedDiscard()) {
        co_return;
      }
      co_await Pump();
      co_return;
    }
    if (phase_ == Phase::kClaimWindow && claim_open_ && ai_claim_pending_) {
      ResolveAutomatedClaimAnswers();
      co_await Pump();
      co_return;
    }
    co_return;
  }
  if (phase_ == Phase::kDiscardWindow && discard_window_open_) {
    const int seat = current_seat_;
    if (!NeedsPlayerReply(seat)) {
      co_return;  // already auto-played
    }
    std::optional<Tile> tile = DefaultDiscardFor(seat);
    if (!tile.has_value()) {
      co_return;
    }
    // Deliberately NOT ChooseDiscard: "play the tile just drawn (or the largest tile when
    // there is no draw record)" is the deterministic fallback frozen by GAME_DESIGN §5.6.2,
    // and both the t17/t22 statistics and the timeout unit test depend on its predictability.
    // Entrusted (disconnected) seats use the same default action. Consequence by design: an
    // AI builds its hand, a timed-out human seat does not.
    spdlog::info("room {}: seat {} timed out, default discard {}", room_id_, seat, FormatTile(*tile));
    discard_window_open_ = false;
    co_await ApplyDiscard(seat, *tile);
    co_await Pump();
    co_return;
  }
  if (phase_ == Phase::kClaimWindow && claim_open_ && claim_pending_humans_ > 0) {
    for (int seat = 0; seat < kSeatCount; ++seat) {
      if (claim_recipient_[static_cast<size_t>(seat)] && !claim_answers_[static_cast<size_t>(seat)].answered) {
        ClaimAnswer answer;
        answer.answered = true;
        answer.action = "pass";
        claim_answers_[static_cast<size_t>(seat)] = std::move(answer);
      }
    }
    claim_pending_humans_ = 0;
    spdlog::info("room {}: claim window {} timed out, unanswered seats pass", room_id_, window_seq);
    co_await Pump();
  }
}

exec::task<void> RoomActor::Ready(PlayerInfo player) {
  if (SeatOf(player.player_id) < 0) {
    co_return;
  }
  if (game_started_) {
    spdlog::warn("room {}: ready from {} ignored, the game already started", room_id_, player.player_id);
    co_return;
  }
  co_await StartGame();
}

exec::task<void> RoomActor::Sync(PlayerInfo player) {
  const int seat = SeatOf(player.player_id);
  if (seat < 0) {
    co_return;
  }
  if (!game_started_) {
    co_await ReplyError(seat, protocol::kCodeInternal, "the game has not started yet");
    co_return;
  }
  co_await ReplyFrame(seat, BuildStateSync(seat));
}

exec::task<void> RoomActor::OnPlayerDisconnected(PlayerInfo player) {
  const int seat = SeatOf(player.player_id);
  if (seat < 0 || seats_[static_cast<size_t>(seat)].disconnected) {
    co_return;
  }
  seats_[static_cast<size_t>(seat)].disconnected = true;
  spdlog::info("room {}: seat {} is entrusted now", room_id_, seat);
  // GAME_DESIGN §5.6.2: an entrusted seat plays the default action immediately, and a pending
  // claim window counts it as a pass (no 15s wait).
  if (claim_open_ && claim_recipient_[static_cast<size_t>(seat)] &&
      !claim_answers_[static_cast<size_t>(seat)].answered) {
    ClaimAnswer answer;
    answer.answered = true;
    answer.action = "pass";
    claim_answers_[static_cast<size_t>(seat)] = std::move(answer);
    --claim_pending_humans_;
  }
  co_await Pump();
}

exec::task<void> RoomActor::OnIdleTimeout(PlayerInfo player) { co_await OnPlayerDisconnected(std::move(player)); }

exec::task<void> RoomActor::HandleGameFrame(PlayerInfo player, json::Value frame) {
  const int seat = SeatOf(player.player_id);
  if (seat < 0 || !game_started_ || phase_ == Phase::kFinished) {
    co_return;
  }
  const json::Value* type = frame.Find("type");
  const json::Value* seq_value = frame.Find("seq");
  if (type == nullptr || !type->IsString() || seq_value == nullptr || !seq_value->IsInteger()) {
    co_return;  // ConnectionActor already answers BAD_REQUEST for a malformed frame
  }
  const uint64_t seq = static_cast<uint64_t>(seq_value->AsInt());
  if (seq != seq_) {
    // PROTOCOL.md §6.3: a reply for a window that already moved on is dropped silently.
    spdlog::debug("room {}: stale {} (seq {} != {}) dropped", room_id_, type->AsString(), seq, seq_);
    co_return;
  }
  if (type->AsString() == "discard") {
    co_await HandleDiscard(seat, std::move(frame));
  } else if (type->AsString() == "action") {
    co_await HandleAction(seat, std::move(frame));
  }
}

exec::task<void> RoomActor::HandleDiscard(int seat, json::Value frame) {
  const json::Value* tile_value = frame.Find("tile");
  if (phase_ != Phase::kDiscardWindow || !discard_window_open_ || seat != current_seat_) {
    co_await ReplyError(seat, protocol::kCodeNotYourTurn, "there is no discard window for you");
    co_return;
  }
  if (tile_value == nullptr || !tile_value->IsString()) {
    co_await ReplyError(seat, protocol::kCodeBadRequest, "discard requires a string \"tile\"");
    co_return;
  }
  std::optional<Tile> tile = ParseTile(tile_value->AsString());
  if (!tile.has_value() || !HandHasFace(hands_[static_cast<size_t>(seat)], FaceOf(*tile))) {
    co_await ReplyError(seat, protocol::kCodeUnknownTile, "tile is not in your hand: " + tile_value->AsString());
    co_return;
  }
  discard_window_open_ = false;
  co_await ApplyDiscard(seat, *tile);
  co_await Pump();
}

exec::task<void> RoomActor::HandleAction(int seat, json::Value frame) {
  const json::Value* action_value = frame.Find("action");
  const json::Value* tiles_value = frame.Find("tiles");
  if (action_value == nullptr || !action_value->IsString() || tiles_value == nullptr || !tiles_value->IsArray()) {
    co_await ReplyError(seat, protocol::kCodeBadRequest, "action requires \"action\" and \"tiles\"");
    co_return;
  }
  std::vector<Tile> tiles;
  for (const json::Value& entry : tiles_value->AsArray()) {
    if (!entry.IsString()) {
      co_await ReplyError(seat, protocol::kCodeBadRequest, "\"tiles\" must only contain strings");
      co_return;
    }
    std::optional<Tile> tile = ParseTile(entry.AsString());
    if (!tile.has_value()) {
      co_await ReplyError(seat, protocol::kCodeUnknownTile, "invalid tile code: " + entry.AsString());
      co_return;
    }
    tiles.push_back(*tile);
  }
  const std::string action = action_value->AsString();

  if (phase_ == Phase::kDiscardWindow && discard_window_open_) {
    // H1: a discard window only accepts `discard` or `win`; `pass` is ILLEGAL_ACTION.
    if (seat != current_seat_) {
      co_await ReplyError(seat, protocol::kCodeNoPendingAction, "the discard window belongs to another seat");
      co_return;
    }
    if (action != "win") {
      co_await ReplyError(seat, protocol::kCodeIllegalAction, "the discard window only accepts a discard or a win");
      co_return;
    }
    if (!tiles.empty()) {
      co_await ReplyError(seat, protocol::kCodeIllegalAction, "win must not carry tiles");
      co_return;
    }
    if (!discard_window_win_check_) {
      // PROTOCOL §6.1/§6.3: the window this seat answered offered `options: []`, so a `win`
      // here is outside its options → ILLEGAL_ACTION. Judged by what the window offered, never
      // by a recomputation: right after a pong/chow the concealed hand can already form a
      // complete hand, and recomputing would turn "pong" into a free self-draw win.
      co_await ReplyError(seat, protocol::kCodeIllegalAction, "win is not available in this window");
      co_return;
    }
    const std::vector<Tile>& hand = hands_[static_cast<size_t>(seat)];
    const std::vector<Option> options = DiscardOptions(hand, melds_[static_cast<size_t>(seat)]);
    if (FindOption(options, "win") == nullptr) {
      co_await ReplyError(seat, protocol::kCodeIllegalAction, "win is not available in this window");
      co_return;
    }
    // H8: never trust the client — recompute with the rules library.
    const Tile winning = TsumoWinningTile(seat);
    if (!IsWinningHand(hand, melds_[static_cast<size_t>(seat)], winning, /*is_tsumo=*/true)) {
      co_await ReplyError(seat, protocol::kCodeIllegalAction, "the hand is not a winning hand");
      co_return;
    }
    discard_window_open_ = false;
    co_await EndWithWin(seat, -1, winning, /*tsumo=*/true);
    co_return;
  }

  if (phase_ != Phase::kClaimWindow || !claim_open_ || !claim_recipient_[static_cast<size_t>(seat)]) {
    co_await ReplyError(seat, protocol::kCodeNoPendingAction, "there is no action window for you");
    co_return;
  }
  if (claim_answers_[static_cast<size_t>(seat)].answered) {
    co_await ReplyError(seat, protocol::kCodeIllegalAction, "this seat already answered the window");
    co_return;
  }
  const std::vector<Option>& seat_options = claim_options_[static_cast<size_t>(seat)];
  if (FindOption(seat_options, action) == nullptr) {
    co_await ReplyError(seat, protocol::kCodeIllegalAction, "action is not in this window's options: " + action);
    co_return;
  }
  const std::vector<Tile>& hand = hands_[static_cast<size_t>(seat)];
  const std::vector<Meld>& melds = melds_[static_cast<size_t>(seat)];
  if (action == "win") {
    if (!tiles.empty()) {
      co_await ReplyError(seat, protocol::kCodeIllegalAction, "win must not carry tiles");
      co_return;
    }
    if (!IsWinningHand(hand, melds, claim_tile_, /*is_tsumo=*/false)) {
      co_await ReplyError(seat, protocol::kCodeIllegalAction, "the hand is not a winning hand");
      co_return;
    }
  } else if (action == "pong") {
    if (!SameFaceMultiset(tiles, {claim_tile_}) || !CanPong(hand, claim_tile_)) {
      co_await ReplyError(seat, protocol::kCodeIllegalAction, "pong is not legal for this tile");
      co_return;
    }
  } else if (action == "kong") {
    if (!SameFaceMultiset(tiles, {claim_tile_}) || !CanKong(hand, claim_tile_, remaining_)) {
      co_await ReplyError(seat, protocol::kCodeIllegalAction, "kong is not legal for this tile");
      co_return;
    }
  } else if (action == "chow") {
    if (seat != (claim_from_seat_ + 1) % kSeatCount) {
      co_await ReplyError(seat, protocol::kCodeIllegalAction, "only the next player may chow");
      co_return;
    }
    const std::vector<std::array<Tile, 2>> combinations = ChowCombinations(hand, claim_tile_);
    const bool matches =
        std::any_of(combinations.begin(), combinations.end(), [&tiles](const std::array<Tile, 2>& pair) {
          return SameFaceMultiset(tiles, {pair[0], pair[1]});
        });
    if (!matches) {
      co_await ReplyError(seat, protocol::kCodeIllegalAction, "chow tiles do not form a run with the discarded tile");
      co_return;
    }
  } else if (action == "pass") {
    if (!tiles.empty()) {
      co_await ReplyError(seat, protocol::kCodeIllegalAction, "pass must not carry tiles");
      co_return;
    }
  }

  ClaimAnswer answer;
  answer.answered = true;
  answer.action = action;
  answer.tiles = tiles;
  claim_answers_[static_cast<size_t>(seat)] = std::move(answer);
  if (claim_pending_humans_ > 0) {
    --claim_pending_humans_;
  }
  co_await Pump();
}

}  // namespace mahjong
