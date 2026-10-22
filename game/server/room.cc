#include "room.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include <spdlog/spdlog.h>

#include "clock.h"
#include "overloaded.h"
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

std::string BuildGameStartFrame(int for_seat, int dealer, int round_index, int round_count,
                                const std::vector<Tile>& hand) {
  // PROTOCOL v1.6 §5.4：+round_index / round_count（低频场次信息，state_sync 不带，§5.9 性能注）。
  json::Value frame = json::Value::Obj(6);  // type, seat, dealer, round_index, round_count, hand
  frame.Set("type", json::Value::Str("game_start"));
  frame.Set("seat", json::Value::Int(for_seat));
  frame.Set("dealer", json::Value::Int(dealer));
  frame.Set("round_index", json::Value::Int(round_index));
  frame.Set("round_count", json::Value::Int(round_count));
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

int& InterHandDelayStorage() {
  static int value = kDefaultInterHandDelayMs;
  return value;
}

RoomActor::RoomActor(std::string room_id, uint64_t seed, int action_timeout_ms, std::vector<RoomSeat> seats,
                     std::shared_ptr<WindowTimer> timer, int ai_delay_ms, int inter_hand_delay_ms)
    : room_id_(std::move(room_id)),
      seed_(seed),
      action_timeout_ms_(action_timeout_ms),
      seats_(std::move(seats)),
      timer_(std::move(timer)),
      ai_delay_ms_(ai_delay_ms < 0 ? AiActionDelayMs() : ai_delay_ms),
      inter_hand_delay_ms_(inter_hand_delay_ms < 0 ? InterHandDelayMs() : inter_hand_delay_ms) {
  // Seat number == index everywhere in this class; make that true even if the caller handed
  // the seats over in another order.
  std::sort(seats_.begin(), seats_.end(), [](const RoomSeat& lhs, const RoomSeat& rhs) { return lhs.seat < rhs.seat; });
}

void SetAiActionDelayMs(int delay_ms) { AiDelayStorage() = delay_ms; }

int AiActionDelayMs() { return AiDelayStorage(); }

void SetInterHandDelayMs(int delay_ms) { InterHandDelayStorage() = delay_ms; }

int InterHandDelayMs() { return InterHandDelayStorage(); }

int RoomActor::SeatOf(const std::string& player_id) const {
  for (const RoomSeat& seat : seats_) {
    if (!seat.is_bot && seat.player.player_id == player_id) {
      return seat.seat;
    }
  }
  return -1;
}

namespace {

// The three kinds below that only install state have no handler to await, but a `std::visit`
// visitor has one return type and Dispatch's other arms return their handler's task — so they
// hand back this finished task instead. It costs those three a coroutine frame they would not
// need alone, which is why it is the price for only them: the scheduler and the finished
// callback are installed once per room and take_seat runs once per join, while game_frame and
// window_timeout — the hot kinds — keep exactly the frames they had before (none extra).
exec::task<void> NoOp() { co_return; }

}  // namespace

// The room's only entry point (room.h). The gate is taken here and nowhere else, so every
// handler below runs with the room to itself — including across a suspension, which the room
// will start to have (see the Deliver note): the framework would otherwise hand this actor the
// next message the moment a handler awaited something.
//
// The visitor returns the task Dispatch awaits rather than being a coroutine itself: a coroutine
// per arm would allocate a frame for every message, and the handlers already are the coroutines
// — this is what `co_await Handler(...)` did before, at the same frame count.
exec::task<void> RoomActor::Dispatch(RoomEnvelope envelope) {
  AsyncGate::Guard guard = co_await gate_.Acquire();
  co_await std::visit(
      overloaded {
          [this](RoomEnvelope::ReadyMsg&& m) { return Ready(std::move(m.player)); },
          [this](RoomEnvelope::SyncMsg&& m) { return Sync(std::move(m.player)); },
          [this](RoomEnvelope::GameFrameMsg&& m) { return HandleGameFrame(std::move(m.player), std::move(m.frame)); },
          [this](RoomEnvelope::WindowTimeoutMsg&& m) { return OnWindowTimeout(m.token); },
          [this](RoomEnvelope::PlayerDisconnectedMsg&& m) { return OnPlayerDisconnected(std::move(m.player)); },
          [this](RoomEnvelope::IdleTimeoutMsg&& m) { return OnIdleTimeout(std::move(m.player)); },
          [this](RoomEnvelope::InstallTimeoutSchedulerMsg&& m) {
            SetTimeoutScheduler(std::move(m.scheduler));
            return NoOp();
          },
          [this](RoomEnvelope::InstallFinishedCallbackMsg&& m) {
            SetFinishedCallback(std::move(m.finished));
            return NoOp();
          },
          [this](RoomEnvelope::TakeSeatMsg&& m) {
            TakeSeat(std::move(m.seat_slot), m.accepted);
            return NoOp();
          },
      },
      std::move(envelope.msg));
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
  // C10：match_over = 「本场到此为止」（match_end 已/即将下发）；lobby 只据此 arm 回收。
  finished.match_over = !settlement.match_will_continue;
  // B3：场终快照随 match_over 的那次通知带出，lobby 据此写 match_results。
  finished.hands_played = settlement.hands_played;
  finished.dealer_hands = settlement.dealer_hands;
  finished.totals = settlement.totals;
  finished.final_rank = settlement.final_rank;
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
  const std::vector<Option> options = DiscardOptions(hand, melds, remaining_);
  const Tile winning = TsumoWinningTile(seat);
  if (FindOption(options, "win") == nullptr || !IsWinningHand(hand, melds, winning, /*is_tsumo=*/true)) {
    co_return false;
  }
  discard_window_open_ = false;
  discard_window_win_check_ = false;
  drawn_tile_.reset();
  co_await EndRoundWithWin(seat, -1, winning, /*tsumo=*/true);
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
      co_await EndRoundWithDraw();
      co_return false;
    }
    discard_window_open_ = false;
    co_await ApplyDiscard(seat, *tile);
    co_return true;
  }
  // An entrusted (disconnected) human keeps the frozen §5.6.2 default action, immediately.
  std::optional<Tile> tile = DefaultDiscardFor(seat);
  if (!tile.has_value()) {
    co_await EndRoundWithDraw();
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
  // and the room must not move on until every seat has been told. Each seat is a PlayerActor
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
  co_await room_seat.endpoint.SendLocal<&SeatEndpoint::Dispatch>(PlayerEnvelope::SeatFrame(std::move(frame)));
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
      PlayerEnvelope::SeatError(protocol::ErrorFrame(code, message), quota));
}

std::array<std::string, kSeatCount> RoomActor::StateSyncFrames() const {
  // v1.6（N1 两版缓存）：共享**打码版**（他家 ankan.tiles = []）；仅暗杠持有者额外渲染一份
  // 替换其自身项（额外次数 = 持有者数，实战 ≤2；无暗杠退化 1 份）。禁止每座位重建 4 份，
  // 禁止对所有人下发真实牌面。此循环无 await，渲染期间状态不可变（原 hoist 注释仍成立）。
  std::array<std::string, kSeatCount> frames;
  std::string masked_json;
  bool holder_json_used = false;
  std::string holder_json;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (seats_[static_cast<size_t>(seat)].endpoint.IsEmpty()) {
      continue;  // bots have no wire
    }
    if (SeatHoldsAnkan(seat)) {
      // 暗杠持有者：只此座位看到自己的 4 张真实牌面
      if (!holder_json_used) {
        holder_json = BuildSeatsArray(seat).Dump();
        holder_json_used = true;
      }
      frames[static_cast<size_t>(seat)] = BuildStateSync(seat, holder_json);
      continue;
    }
    if (masked_json.empty()) {
      masked_json = BuildSeatsArray(kSeatsMaskAll).Dump();  // a rendered 4-entry array is never empty
    }
    frames[static_cast<size_t>(seat)] = BuildStateSync(seat, masked_json);
  }
  return frames;
}

std::array<std::string, kSeatCount> RoomActor::StateSyncFramesRevealed() const {
  // v1.6（§5.9 局终揭示）：game_end 前最后一份 state_sync，把**所有** ankan 补全为 4 张真实牌面，
  // 供结算页画完整手牌。复用既有帧，不新增帧、不新增字段。
  std::array<std::string, kSeatCount> frames;
  const std::string revealed = BuildSeatsArray(kSeatsRevealAll).Dump();
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (seats_[static_cast<size_t>(seat)].endpoint.IsEmpty()) {
      continue;
    }
    frames[static_cast<size_t>(seat)] = BuildStateSync(seat, revealed);
  }
  return frames;
}

bool RoomActor::SeatHoldsAnkan(int seat) const {
  for (const Meld& meld : melds_[static_cast<size_t>(seat)]) {
    if (meld.type == MeldType::kAnkan) return true;
  }
  return false;
}

json::Value RoomActor::BuildSeatsArray(int viewer) const {
  json::Value seat_array = json::Value::Arr(kSeatCount);
  for (const RoomSeat& room_seat : seats_) {
    json::Value entry = json::Value::Obj(7);  // seat, name, is_bot, hand_count, discards, melds, disconnected
    entry.Set("seat", json::Value::Int(room_seat.seat));
    entry.Set("name", json::Value::Str(room_seat.player.name));
    entry.Set("is_bot", json::Value::Bool(room_seat.is_bot));
    // H6: the public per-seat view MUST NOT contain `hand`, only the tile count.
    entry.Set("hand_count", json::Value::Int(static_cast<int64_t>(hands_[static_cast<size_t>(room_seat.seat)].size())));
    entry.Set("discards", TilesToJson(discards_[static_cast<size_t>(room_seat.seat)]));
    // v1.6（§5.9 暗杠三态）：本家的 ankan 带 4 张真实牌面；他家恒为 []；局终揭示版全部补全。
    const bool reveal = viewer == kSeatsRevealAll || viewer == room_seat.seat;
    json::Value melds = json::Value::Arr(melds_[static_cast<size_t>(room_seat.seat)].size());
    for (const Meld& meld : melds_[static_cast<size_t>(room_seat.seat)]) {
      json::Value item = json::Value::Obj(2);  // type, tiles
      item.Set("type", json::Value::Str(MeldTypeName(meld.type)));
      if (meld.type == MeldType::kAnkan && !reveal) {
        item.Set("tiles", json::Value::Arr(0));  // 他家视角：恒空数组
      } else {
        item.Set("tiles", TilesToJson(meld.tiles));
      }
      melds.Append(std::move(item));
    }
    entry.Set("melds", std::move(melds));
    entry.Set("disconnected", json::Value::Bool(room_seat.disconnected));
    seat_array.Append(std::move(entry));
  }
  return seat_array;
}

std::string RoomActor::BuildStateSync(int for_seat) const {
  return BuildStateSync(for_seat, BuildSeatsArray(for_seat).Dump());
}

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
  return BuildGameStartFrame(for_seat, dealer_, round_index_, round_count_,
                             hands_[static_cast<size_t>(for_seat)]);
}

std::string RoomActor::BuildMatchEnd() const {
  // PROTOCOL v1.6 §5.15：场终帧，无 seq。rank = 1 + 严格大于本座位分数的人数（并列跳号）；
  // 排序 final_score 降序、同分 seat 升序。
  struct Ranked {
    int seat;
    int score;
    int rank;
  };
  std::array<Ranked, kSeatCount> standings {};
  for (int seat = 0; seat < kSeatCount; ++seat) {
    standings[static_cast<size_t>(seat)] = Ranked {seat, match_totals_[static_cast<size_t>(seat)], 1};
  }
  std::sort(standings.begin(), standings.end(), [](const Ranked& a, const Ranked& b) {
    if (a.score != b.score) return a.score > b.score;  // final_score 降序
    return a.seat < b.seat;                            // 同分 seat 升序
  });
  for (size_t i = 0; i < standings.size(); ++i) {
    // rank = 1 + 严格大于本座位分数的人数（并列跳号）；注意 standings 已按分数排序，
    // 必须按**数组下标**回写，不能用 seat 号索引（排序后两者不再相等）。
    int rank = 1;
    for (const Ranked& other : standings) {
      if (other.score > standings[i].score) ++rank;
    }
    standings[i].rank = rank;
  }
  json::Value frame = json::Value::Obj(4);  // type, hands_played, renchan_count, rankings
  frame.Set("type", json::Value::Str("match_end"));
  // D2 裁决：hands_played = 实际计入局数 = round_index 终值 + 1（截断场），正常场 = round_count。
  // 恒等式「game_end 次数 = hands_played + renchan_count」在含 C15 截断的一切场景成立。
  frame.Set("hands_played", json::Value::Int(hands_played_));
  frame.Set("renchan_count", json::Value::Int(renchan_count_));
  json::Value rankings = json::Value::Arr(kSeatCount);
  for (const Ranked& entry : standings) {
    json::Value item = json::Value::Obj(3);  // seat, final_score, rank
    item.Set("seat", json::Value::Int(entry.seat));
    item.Set("final_score", json::Value::Int(entry.score));
    item.Set("rank", json::Value::Int(entry.rank));
    rankings.Append(std::move(item));
  }
  frame.Set("rankings", std::move(rankings));
  return frame.Dump();
}

std::string RoomActor::BuildGameEnd(const Settlement& settlement) const {
  json::Value frame = json::Value::Obj(8);  // type, reason, round_index, dealer, renchan_count,
                                            // match_will_continue, results (+ win)
  frame.Set("type", json::Value::Str("game_end"));
  frame.Set("reason", json::Value::Str(settlement.reason));
  // v1.6 新增顶层字段：场次进度（game_end 是低频帧，state_sync 不带这些，§5.9 性能注）
  frame.Set("round_index", json::Value::Int(settlement.round_index));
  frame.Set("dealer", json::Value::Int(settlement.dealer));
  frame.Set("renchan_count", json::Value::Int(settlement.renchan_count));
  frame.Set("match_will_continue", json::Value::Bool(settlement.match_will_continue));

  json::Value results = json::Value::Arr(kSeatCount);
  for (int seat = 0; seat < kSeatCount; ++seat) {
    json::Value entry = json::Value::Obj(5);  // seat, delta_score, match_total, hand, win_type
    entry.Set("seat", json::Value::Int(seat));
    entry.Set("delta_score", json::Value::Int(settlement.delta[static_cast<size_t>(seat)]));
    // v1.6：本场累计得失分（含本局），四家之和恒为 0
    entry.Set("match_total", json::Value::Int(settlement.totals[static_cast<size_t>(seat)]));
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
    json::Value win = json::Value::Obj(12);
    // seat, tile, from_seat, tsumo, tags, multiplier, per_payer, total,
    // yaku, han, han_breakdown, dealer_bonus, rinshan
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
    // v1.6（B1 番数制）：win.yaku[] 按冻结判定序，每 id 至多一条
    json::Value yaku = json::Value::Arr(settlement.yaku.size());
    for (const Yaku& item : settlement.yaku) {
      json::Value entry = json::Value::Obj(2);  // id, han
      entry.Set("id", json::Value::Str(item.id));
      entry.Set("han", json::Value::Int(item.han));
      yaku.Append(std::move(entry));
    }
    win.Set("yaku", std::move(yaku));
    // 番数四段式（B1.1 裁决）：base = Σ yaku.han；tsumo 恒 0（自摸不是番种，
    // "自摸 ×2" 已并入支付公式的 2R/6R）；dealer = 庄家加成；total = 封顶后合计。
    json::Value breakdown = json::Value::Obj(4);  // base, tsumo, dealer, total
    int base = 0;
    for (const Yaku& item : settlement.yaku) base += item.han;
    breakdown.Set("base", json::Value::Int(base));
    breakdown.Set("tsumo", json::Value::Int(0));
    breakdown.Set("dealer", json::Value::Int(settlement.dealer_bonus));
    breakdown.Set("total", json::Value::Int(settlement.han));
    win.Set("han_breakdown", std::move(breakdown));
    win.Set("han", json::Value::Int(settlement.han));  // == han_breakdown.total（§5.10）
    win.Set("dealer_bonus", json::Value::Bool(settlement.dealer_bonus == 1));
    win.Set("rinshan", json::Value::Bool(settlement.rinshan));
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

exec::task<void> RoomActor::StartMatch() {
  // v1.6（C2）：一场一次。这里只动**场次**状态；局内状态全部归 StartRound（C3 清单）。
  game_started_ = true;
  round_index_ = 0;
  hands_played_ = 0;
  renchan_count_ = 0;
  match_totals_ = {};
  dealer_hands_ = {};  // B3：坐庄手数随场次清零
  dealer_ = 0;  // 首局庄家 = seat 0（GAME_DESIGN v1.10 §3.3）
  co_await StartRound();
}

exec::task<void> RoomActor::StartRound() {
  // v1.6（C3）：每局一次的重置清单。**禁止重置**：seq_（跨局单调，§6.2）、room_id_、seats_、
  // match_totals_、round_index_/renchan_count_/hands_played_（由局终推进逻辑维护）。
  melds_ = {};
  discards_ = {};
  ResetClaimWindow();  // 复用既有清单，别手写（C3）
  discard_window_open_ = false;
  discard_window_win_check_ = false;
  drawn_tile_.reset();
  ai_discard_pending_ = false;
  round_advance_pending_ = false;
  kong_draw_tile_.reset();

  wall_ = BuildWall(seed_);
  // C9：庄家显式传参（B1 改造后的签名）。发牌后不变量：庄家 14 张、其余 13 张、牌山 83。
  Deal deal = DealTiles(wall_, dealer_);
  hands_ = deal.hands;  // hands[dealer] is the dealer (14 tiles), the others 13
  wall_ = std::move(deal.wall);
  next_index_ = deal.next_index;
  remaining_ = deal.remaining;
  for (std::vector<Tile>& hand : hands_) {
    SortTiles(hand);
  }
  current_seat_ = dealer_;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    const size_t expected = seat == dealer_ ? 14u : 13u;
    if (hands_[static_cast<size_t>(seat)].size() != expected || remaining_ != kWallSizeAfterDeal) {
      spdlog::error("room {}: deal invariant broken (dealer={} hand={} remaining={})", room_id_, dealer_,
                    hands_[static_cast<size_t>(seat)].size(), remaining_);
    }
  }
  Transition();  // the dealer's first discard window carries the new seq（seq 跨局单调，不归零）

  std::array<std::string, kSeatCount> starts;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (!seats_[static_cast<size_t>(seat)].endpoint.IsEmpty()) {
      starts[static_cast<size_t>(seat)] = BuildGameStart(seat);
    }
  }
  co_await Deliver(std::move(starts));
  spdlog::info("room {}: round started (round_index={}, dealer={}, remaining={}, seq={})", room_id_, round_index_,
               dealer_, remaining_, seq_);

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
      case Phase::kRoundEnd:
      case Phase::kMatchEnd: co_return;

      case Phase::kTurnStart: {
        if (remaining_ <= 0) {
          co_await EndRoundWithDraw();
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
    // B2 接线（B1 裁决 F）：显式传 remaining_，缺省 −1 永远不出杠选项
    options = DiscardOptions(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)], remaining_);
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
  kong_draw_tile_.reset();  // 打出即放弃杠上开花机会（该窗口的补牌记录作废）

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
    co_await EndRoundWithWin(best_seat, from_seat, tile, /*tsumo=*/false);
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
      // No tile left for the replacement draw: the round ends (GAME_DESIGN §4.2 step 12).
      co_await EndRoundWithDraw();
      co_return;
    }
    Tile drawn = wall_[static_cast<size_t>(next_index_)];
    ++next_index_;
    remaining_ = kTileCount - next_index_;  // 派生口径（B1 红线：禁止递减式）
    hands_[static_cast<size_t>(seat)].push_back(drawn);
    SortTiles(hands_[static_cast<size_t>(seat)]);
    replacement = drawn;
    kong_draw_tile_ = drawn;  // 杠上开花记录：本窗口内自摸且和牌张 == 它 ⇒ rinshan（B1 接线）
    std::array<std::string, kSeatCount> draws;
    if (!seats_[static_cast<size_t>(seat)].endpoint.IsEmpty()) {
      draws[static_cast<size_t>(seat)] = BuildDrawFrame(drawn, seat, seq_);
    }
    co_await Deliver(std::move(draws));
  }
  co_await Deliver(StateSyncFrames());
  co_await OpenDiscardWindow(/*win_check=*/action == "kong", replacement);
}

// v1.6 自理杠（ankan / kakan，PROTOCOL §4.6）：仅本家打牌窗口可触发（HandleAction 已重算合法性）。
// 张数公式逐字不变：一个杠在 melds 里仍是 1 组、账面 3 张，第 4 张由 kans 单独补回 ——
// 三个杠的差别只在「第 4 张从哪来」，本函数只负责把它放进去并补摸 1 张。
exec::task<void> RoomActor::ApplySelfKan(int seat, const std::string& action,
                                         const std::vector<Tile>& client_tiles) {
  const Face face = FaceOf(client_tiles.front());
  std::optional<KongResult> result;
  if (action == "ankan") {
    result = ApplyAnkan(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)], face);
  } else {  // kakan：找到与该牌面同面的 pong 组（KakanMeldIndices 升序 ⇒ 第一个同面组，确定）
    for (int index : KakanMeldIndices(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)])) {
      const Meld& meld = melds_[static_cast<size_t>(seat)][static_cast<size_t>(index)];
      if (!meld.tiles.empty() && FaceOf(meld.tiles.front()) == face) {
        result = ApplyKakan(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)], index);
        break;
      }
    }
  }
  if (!result.has_value()) {
    // 理论不可达：HandleAction 已按 DiscardOptions 的窗口选项重算过合法性。
    co_await ReplyError(seat, protocol::kCodeIllegalAction, action + " is not legal anymore");
    co_return;
  }
  hands_[static_cast<size_t>(seat)] = std::move(result->hand);
  melds_[static_cast<size_t>(seat)] = std::move(result->melds);

  const uint64_t old_seq = seq_;
  Transition();  // close the discard window; the replacement-draw window takes the new seq
  phase_ = Phase::kDiscardWindow;

  // action_event（PROTOCOL §5.8 v1.6）：ankan/kakan **不带 from_seat**、**不动牌河**；
  // tiles = 恰 1 个牌面元素（与请求同形）。
  std::array<std::string, kSeatCount> events;
  const std::string event = BuildActionEventFrame(seat, action, client_tiles, /*from_seat=*/-1, old_seq);
  for (int target = 0; target < kSeatCount; ++target) {
    if (!seats_[static_cast<size_t>(target)].endpoint.IsEmpty()) {
      events[static_cast<size_t>(target)] = event;
    }
  }
  co_await Deliver(std::move(events));
  spdlog::info("room {}: seat {} {} (seq {} -> {})", room_id_, seat, action, old_seq, seq_);

  // 立即补摸 1 张（无抢杠窗口，§0.4）；remaining 只能由 kTileCount - next_index_ 派生。
  if (remaining_ <= 0) {
    co_await EndRoundWithDraw();
    co_return;
  }
  Tile drawn = wall_[static_cast<size_t>(next_index_)];
  ++next_index_;
  remaining_ = kTileCount - next_index_;
  hands_[static_cast<size_t>(seat)].push_back(drawn);
  SortTiles(hands_[static_cast<size_t>(seat)]);
  kong_draw_tile_ = drawn;  // 杠上开花记录
  std::array<std::string, kSeatCount> draws;
  if (!seats_[static_cast<size_t>(seat)].endpoint.IsEmpty()) {
    draws[static_cast<size_t>(seat)] = BuildDrawFrame(drawn, seat, seq_);
  }
  co_await Deliver(std::move(draws));

  // 自理杠之后必定下发 state_sync（§5.12/§5.8 注 4：他家以此权威覆盖 melds[]）。
  co_await Deliver(StateSyncFrames());
  co_await OpenDiscardWindow(/*win_check=*/true, drawn);
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

exec::task<void> RoomActor::EndRoundWithWin(int winner_seat, int from_seat, Tile winning_tile, bool tsumo) {
  const uint64_t old_seq = seq_;
  if (!tsumo) {
    // Ron: the winning tile completes the winner's hand (it stays visible in the river).
    hands_[static_cast<size_t>(winner_seat)].push_back(winning_tile);
    SortTiles(hands_[static_cast<size_t>(winner_seat)]);
  }
  const std::vector<Tile>& concealed = hands_[static_cast<size_t>(winner_seat)];
  // B1/B2 算分接线：ScoreHand + 完整 ScoreContext —— 庄家加成按 winner == dealer_，
  // 杠上开花按「本局杠补牌记录」：该窗口内自摸且和牌张就是本次杠的补牌 ⇒ true。
  ScoreContext score_ctx;
  score_ctx.winner_seat = winner_seat;
  score_ctx.from_seat = tsumo ? -1 : from_seat;
  score_ctx.winner_is_dealer = winner_seat == dealer_;
  score_ctx.is_rinshan = tsumo && kong_draw_tile_.has_value() && winning_tile == *kong_draw_tile_;
  const ScoreResult score = ScoreHand(concealed, melds_[static_cast<size_t>(winner_seat)],
                                      winning_tile, score_ctx);

  Transition();  // close the window; no new window follows
  phase_ = Phase::kRoundEnd;
  claim_open_ = false;
  discard_window_open_ = false;
  discard_window_win_check_ = false;
  drawn_tile_.reset();
  kong_draw_tile_.reset();

  Settlement settlement;
  settlement.reason = tsumo ? "win_tsumo" : "win_ron";
  settlement.delta = score.delta;
  settlement.winner_seat = winner_seat;
  settlement.winner_flat_hand = concealed;
  for (const Meld& meld : melds_[static_cast<size_t>(winner_seat)]) {
    settlement.winner_flat_hand.insert(settlement.winner_flat_hand.end(), meld.tiles.begin(), meld.tiles.end());
  }
  SortTiles(settlement.winner_flat_hand);
  // B1: tags 直接取自判番结果（单一出口），不再单独调 WinTags —— 消除「同一件事判两遍」的漂移风险。
  settlement.tags.clear();
  for (const Yaku& yaku : score.yaku) settlement.tags.push_back(yaku.id);
  settlement.winner_types = settlement.tags;
  settlement.winner_types.emplace_back(tsumo ? "tsumo" : "ron");
  settlement.tsumo = tsumo;
  settlement.winning_tile = winning_tile;
  settlement.from_seat = tsumo ? -1 : from_seat;
  settlement.multiplier = score.multiplier;
  settlement.per_payer = score.per_payer;
  settlement.total = score.total;
  // v1.6（C8）场次字段
  settlement.round_index = round_index_;
  settlement.dealer = dealer_;
  settlement.yaku = score.yaku;
  settlement.han = score.han;
  settlement.dealer_bonus = score.dealer_bonus;
  settlement.rinshan = score.rinshan;

  // Broadcast to all four seats (§5.8) with no seat-specific field, so one build serves them all.
  std::array<std::string, kSeatCount> events;
  const std::string event = BuildActionEventFrame(winner_seat, "win", {winning_tile}, tsumo ? -1 : from_seat, old_seq);
  for (int target = 0; target < kSeatCount; ++target) {
    if (!seats_[static_cast<size_t>(target)].endpoint.IsEmpty()) {
      events[static_cast<size_t>(target)] = event;
    }
  }
  co_await Deliver(std::move(events));
  // PROTOCOL.md §5.12 + §5.9 v1.6：game_end 前的最后一份 state_sync 必须**揭示全部暗杠**。
  co_await Deliver(StateSyncFramesRevealed());
  co_await AdvanceAfterRound(settlement, score);
}

exec::task<void> RoomActor::EndRoundWithDraw() {
  Transition();
  phase_ = Phase::kRoundEnd;
  claim_open_ = false;
  discard_window_open_ = false;
  discard_window_win_check_ = false;
  drawn_tile_.reset();
  kong_draw_tile_.reset();

  const ScoreResult score = ScoreDraw();
  Settlement settlement;
  settlement.reason = "draw_exhausted";
  settlement.delta = score.delta;
  settlement.winner_seat = -1;
  // v1.6（C8）场次字段
  settlement.round_index = round_index_;
  settlement.dealer = dealer_;

  // PROTOCOL.md §5.12 + §5.9 v1.6：荒庄前最后一份 state_sync 同样揭示全部暗杠。
  co_await Deliver(StateSyncFramesRevealed());
  co_await AdvanceAfterRound(settlement, score);
}

// v1.6（C7 局终 → 场终的推进）：荒庄也无条件累加（+0）、逐局 RecordMatch（C10）、
// NextDealer + ClassifyRound 判定连庄/轮庄（荒庄一律连庄）、C15 双闸、场终 match_end、
// 否则 kRoundEnd + 武装 C6 推进令牌（C5：本函数 co_return，推进由定时消息触发）。
exec::task<void> RoomActor::AdvanceAfterRound(Settlement& settlement, const ScoreResult& score) {
  // C7：荒庄也 +0 无条件累加；match_total 含本局。B3：坐庄手数每局 +1（含连庄的额外手）。
  for (int seat = 0; seat < kSeatCount; ++seat) {
    match_totals_[static_cast<size_t>(seat)] += settlement.delta[static_cast<size_t>(seat)];
  }
  settlement.totals = match_totals_;
  ++dealer_hands_[static_cast<size_t>(settlement.dealer)];
  ++hands_played_;

  // 连庄/轮庄判定（§3.3）：荒庄一律连庄（ClassifyRound 的显式消歧）。
  const RoundResult result = ClassifyRound(settlement.reason, settlement.winner_seat, settlement.dealer);
  if (result == RoundResult::kNonDealerWin) {
    dealer_ = NextDealer(dealer_, result);
    ++round_index_;
  } else {
    ++renchan_count_;  // 庄家和牌或荒庄 ⇒ 连庄（round_index 不动）
  }
  // B2 决策 D1：game_end.renchan_count 报**含本局触发**的累计值（post-increment）——
  // 这与 match_end.renchan_count「与最后一次 game_end 同值」的冻结约束一致（PROTOCOL §5.15）。

  // C15 双闸：round_index 走满 4 局，或连庄次数超过上限，本场结束。
  const bool match_over = round_index_ >= round_count_ || renchan_count_ > kMaxRenchan;
  settlement.match_will_continue = !match_over;
  settlement.renchan_count = renchan_count_;
  if (match_over) {
    // B3 场终快照：与 match_end 同源（hands_played_ / dealer_hands_ / ComputeFinalRanks）。
    settlement.hands_played = hands_played_;
    settlement.dealer_hands = dealer_hands_;
    settlement.final_rank = ComputeFinalRanks();
  }

  co_await BroadcastSettlement(settlement);  // game_end（局终帧，一场 4..8 次）
  spdlog::info("room {}: round over (reason={}, round_index={}, dealer={}, winner={}, R={}, han={}, seq {} -> {})",
               room_id_, settlement.reason, settlement.round_index, settlement.dealer, settlement.winner_seat,
               score.multiplier, settlement.han, seq_, seq_);

  NotifyFinished(settlement);  // C10：逐局通知；lobby 只在 match_over 时回收
  if (!match_over) {
    phase_ = Phase::kRoundEnd;
    // C6：推进令牌必须在 Transition() **之后** arm —— 令牌携带局终后的新 seq，
    // 上一局最后窗口的 timeout（旧 seq、无标签）被第一道判据丢弃，两者永不相交。
    Transition();
    round_advance_pending_ = true;
    if (!timeout_scheduler_) {
      // 与 AI pacing 同款的回退（t31 的"永不等待不会到来的回复"不变式）：
      // 无调度器时立即推进，而不是停在一局终等一条永远不会来的消息。
      spdlog::error("room {}: no timeout scheduler installed for the round advance; advancing inline", room_id_);
      co_await StartRound();
      co_return;
    }
    timeout_scheduler_(inter_hand_delay_ms_, MakeRoundAdvanceToken(seq_));
    co_return;
  }
  co_await FinishMatch();
}

exec::task<void> RoomActor::FinishMatch() {
  // v1.6（C7 场终）：match_end 广播 + kMatchEnd；之后只剩 §1.4.5 的 30s 回收（C10）。
  phase_ = Phase::kMatchEnd;
  round_advance_pending_ = false;
  const std::string match_end = BuildMatchEnd();
  std::array<std::string, kSeatCount> frames;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (!seats_[static_cast<size_t>(seat)].endpoint.IsEmpty()) {
      frames[static_cast<size_t>(seat)] = match_end;
    }
  }
  co_await Deliver(std::move(frames));
  spdlog::info("room {}: match over (rounds={}, renchan={}, hands_played={})", room_id_, round_index_,
               renchan_count_, hands_played_);
}

// match_end.rankings 的 rank（并列跳号）；D2/B3：场终快照与 match_end 必须同源，故独立成函数。
std::array<int, kSeatCount> RoomActor::ComputeFinalRanks() const {
  std::array<int, kSeatCount> ranks {};
  for (int seat = 0; seat < kSeatCount; ++seat) {
    int rank = 1;
    for (int other = 0; other < kSeatCount; ++other) {
      if (match_totals_[static_cast<size_t>(other)] > match_totals_[static_cast<size_t>(seat)]) ++rank;
    }
    ranks[static_cast<size_t>(seat)] = rank;
  }
  return ranks;
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
  // Three kinds of scheduled messages arrive here: a real window deadline (plain envelope seq),
  // an AI pacing deadline (kAiPacingTokenBit), and the v1.6 round-advance deadline
  // (kRoundAdvanceTokenBit). **判据顺序固定（C6，不得调换）**：
  const bool ai_pacing = IsAiPacingToken(token);
  const bool round_advance = IsRoundAdvanceToken(token);
  const uint64_t window_seq = WindowSeqOfToken(token);
  // ① seq 已作废 ⇒ 一律丢弃（含上一局最后窗口的 timeout —— 它带旧 seq，永远进不了后面）。
  if (window_seq != seq_) {
    spdlog::debug("room {}: stale {} for window {} (current {}) dropped", room_id_,
                  ai_pacing ? "AI pacing deadline" : (round_advance ? "round advance" : "timeout"),
                  window_seq, seq_);
    co_return;
  }
  // ② AI pacing → 现有分支
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
  // ③ 局间推进令牌：仅 kRoundEnd 时恰好推进一局（C5：一次消息只推进一局）；否则丢弃。
  if (round_advance) {
    if (phase_ == Phase::kRoundEnd && round_advance_pending_) {
      round_advance_pending_ = false;
      spdlog::info("room {}: inter-hand delay elapsed, starting round {} (dealer={})", room_id_, round_index_,
                   dealer_);
      co_await StartRound();
    }
    co_return;
  }
  // ④ 窗口超时
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
    // AI builds its hand, a timed-out human seat does not. 超时永不选杠（PROTOCOL v1.6 §6.4）。
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
    spdlog::warn("room {}: ready from {} ignored, the match already started", room_id_, player.player_id);
    co_return;
  }
  co_await StartMatch();
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
  if (seat < 0 || !game_started_) {
    co_return;
  }
  // v1.6（C4/C13）：只有**场终**才静默早退；局终后的对局帧回 BAD_REQUEST（新局未开始，
  // 客户端不得把 game_end 当作可继续对局的信号）。
  if (phase_ == Phase::kMatchEnd) {
    co_return;
  }
  if (phase_ == Phase::kRoundEnd) {
    co_await ReplyError(seat, protocol::kCodeBadRequest, "the round has ended; wait for the next game_start");
    co_return;
  }
  const json::Value* type = frame.Find("type");
  const json::Value* seq_value = frame.Find("seq");
  if (type == nullptr || !type->IsString() || seq_value == nullptr || !seq_value->IsInteger()) {
    co_return;  // PlayerActor already answers BAD_REQUEST for a malformed frame
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
    // H1 (v1.6): a discard window accepts `discard`, or an action **in the window's options**
    // (`win` / `ankan` / `kakan`); `pass` is still ILLEGAL_ACTION.
    if (seat != current_seat_) {
      co_await ReplyError(seat, protocol::kCodeNoPendingAction, "the discard window belongs to another seat");
      co_return;
    }
    if (action == "ankan" || action == "kakan") {
      // v1.6 自理杠（PROTOCOL §4.6）：只在打牌窗口、tiles 恰 1 个牌面元素、以窗口 options 为准，
      // 且按规则库重算合法性（§871）。合法性由 DiscardOptions 已含 remaining ≥ 1 守卫。
      if (tiles.size() != 1) {
        co_await ReplyError(seat, protocol::kCodeIllegalAction,
                            action + " must carry exactly one tile face");
        co_return;
      }
      const std::vector<Tile>& hand = hands_[static_cast<size_t>(seat)];
      // H8: never trust the client — recompute with the rules library (显式传 remaining_，
      // 缺省 −1 永远不出杠选项).
      const std::vector<Option> options =
          DiscardOptions(hand, melds_[static_cast<size_t>(seat)], remaining_);
      const Option* option = FindOption(options, action);
      if (option == nullptr || !SameFaceMultiset(tiles, option->tiles)) {
        co_await ReplyError(seat, protocol::kCodeIllegalAction,
                            action + " is not available in this window");
        co_return;
      }
      discard_window_open_ = false;
      co_await ApplySelfKan(seat, action, tiles);
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
    const std::vector<Option> options =
        DiscardOptions(hand, melds_[static_cast<size_t>(seat)], remaining_);
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
    co_await EndRoundWithWin(seat, -1, winning, /*tsumo=*/true);
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
