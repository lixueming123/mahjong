#include "riichi/riichi_mahjong_game.h"

#include <algorithm>
#include <array>
#include <cassert>
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
// the frozen protocol; nothing new is invented here (§0.1). Riichi always serialises the red
// five as `0m`/`0p`/`0s` (v1.18) — it is a distinct physical tile with its own han — which is
// the one place where these builders differ from the classic game's.
// ---------------------------------------------------------------------------------------

json::Value TilesToJson(const std::vector<Tile>& tiles) {
  json::Value array = json::Value::Arr(tiles.size());
  for (Tile tile : tiles) {
    array.Append(json::Value::Str(FormatTile(tile, /*red=*/true)));
  }
  return array;
}

// 牌面码（不带赤）：立直宣言牌的候选 / 喰い替え禁止的牌面都是**牌面**，赤 5 与普通 5 等价。
json::Value TilesToJsonFaces(const std::vector<Tile>& tiles) {
  json::Value array = json::Value::Arr(tiles.size());
  for (Tile tile : tiles) {
    array.Append(json::Value::Str(FormatTile(tile, /*red=*/false)));
  }
  return array;
}

json::Value FacesToJson(const std::vector<Face>& faces) {
  json::Value array = json::Value::Arr(faces.size());
  for (Face face : faces) {
    array.Append(json::Value::Str(FormatTile(MakeTileFromFace(face), /*red=*/false)));
  }
  return array;
}

// 满贯分档 → 线上整数。只在日麻出站用得到（classic 是番数制，没有 limit），
// 所以它随日麻走，不是共享契约的一部分。
protocol::WireLimit ToWire(riichi::Limit limit) {
  switch (limit) {
    case riichi::Limit::kNone:
      return protocol::WireLimit::kNone;
    case riichi::Limit::kMangan:
      return protocol::WireLimit::kMangan;
    case riichi::Limit::kHaneman:
      return protocol::WireLimit::kHaneman;
    case riichi::Limit::kBaiman:
      return protocol::WireLimit::kBaiman;
    case riichi::Limit::kSanbaiman:
      return protocol::WireLimit::kSanbaiman;
    case riichi::Limit::kYakuman:
      return protocol::WireLimit::kYakuman;
  }
  return protocol::WireLimit::kInvalid;
}

json::Value MeldsToJson(const std::vector<Meld>& melds) {
  json::Value array = json::Value::Arr(melds.size());
  for (const Meld& meld : melds) {
    json::Value entry = json::Value::Obj(2);  // type, tiles
    entry.Set("type", json::Value::Int(protocol::WireOf(protocol::ToWire(meld.type))));
    entry.Set("tiles", TilesToJson(meld.tiles));
    array.Append(std::move(entry));
  }
  return array;
}

json::Value OptionsToJson(const std::vector<Option>& options) {
  json::Value array = json::Value::Arr(options.size());
  for (const Option& option : options) {
    json::Value entry = json::Value::Obj(2);  // action, tiles
    entry.Set("action", json::Value::Int(protocol::WireOf(option.action)));
    entry.Set("tiles", TilesToJson(option.tiles));
    array.Append(std::move(entry));
  }
  return array;
}

std::string BuildGameStartFrame(int for_seat, int dealer, int round_index, int round_count,
                                const std::vector<Tile>& hand, const std::array<int, kSeatCount>& points,
                                const std::vector<Tile>& dora_indicators, int round_wind,
                                const std::array<int, kSeatCount>& match_total, int renchan_count) {
  // PROTOCOL v1.6 §5.4：+round_index / round_count（低频场次信息，state_sync 不带，§5.9 性能注）。
  // 日麻追加：持点、宝牌指示牌、场风/自风。v2.2：+match_total / renchan_count。
  json::Value frame = json::Value::Obj(12);
  frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kGameStart)));
  frame.Set("seat", json::Value::Int(for_seat));
  frame.Set("dealer", json::Value::Int(dealer));
  frame.Set("round_index", json::Value::Int(round_index));
  frame.Set("round_count", json::Value::Int(round_count));
  frame.Set("hand", TilesToJson(hand));
  json::Value totals = json::Value::Arr(kSeatCount);
  for (int seat = 0; seat < kSeatCount; ++seat) {
    totals.Append(json::Value::Int(match_total[static_cast<size_t>(seat)]));
  }
  frame.Set("match_total", std::move(totals));
  frame.Set("renchan_count", json::Value::Int(renchan_count));
  json::Value scores = json::Value::Arr(kSeatCount);
  for (int seat = 0; seat < kSeatCount; ++seat) {
    scores.Append(json::Value::Int(points[static_cast<size_t>(seat)]));
  }
  frame.Set("points", std::move(scores));
  frame.Set("dora_indicators", TilesToJson(dora_indicators));
  frame.Set("round_wind", json::Value::Int(round_wind));
  frame.Set("seat_wind", json::Value::Int(riichi::SeatWindOf(round_wind, for_seat, dealer)));
  return frame.Dump();
}

std::string BuildDrawFrame(Tile tile, int seat, uint64_t seq) {
  json::Value frame = json::Value::Obj(4);  // type, tile, seat, seq
  frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kDraw)));
  // 赤 5（v1.18）在日麻是**独立的牌**（`0m`），摸到它必须报 `0m` 而不是 `5m`：
  // 客户端要靠这个字面量把它渲染成红牌，并且它和手里的普通 5 是两张不同的牌。
  frame.Set("tile", json::Value::Str(FormatTile(tile, /*red=*/true)));
  frame.Set("seat", json::Value::Int(seat));
  frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
  return frame.Dump();
}

std::string BuildDiscardEventFrame(int seat, Tile tile, uint64_t seq) {
  json::Value frame = json::Value::Obj(4);  // type, seat, tile, seq
  frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kDiscardEvent)));
  frame.Set("seat", json::Value::Int(seat));
  // 同上：牌河里要能看出被打出的是赤 5。
  frame.Set("tile", json::Value::Str(FormatTile(tile, /*red=*/true)));
  frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
  return frame.Dump();
}

std::string BuildActionRequestFrame(uint64_t seq, int64_t deadline_ms, protocol::Window window,
                                    const std::vector<Option>& options, std::optional<Tile> claimed = std::nullopt,
                                    const std::vector<Tile>& riichi_tiles = {},
                                    const std::vector<Face>& forbidden_discards = {}) {
  const bool has_riichi_tiles = !riichi_tiles.empty();
  const bool has_forbidden = window == protocol::Window::kDiscard && !forbidden_discards.empty();
  json::Value frame =
      json::Value::Obj((claimed.has_value() ? 6 : 5) + (has_riichi_tiles ? 1 : 0) + (has_forbidden ? 1 : 0));
  frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kActionRequest)));
  frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
  frame.Set("deadline_ms", json::Value::Int(deadline_ms));
  frame.Set("window", json::Value::Int(protocol::WireOf(window)));
  frame.Set("options", OptionsToJson(options));
  if (claimed.has_value()) {
    // 抢杠的那张牌不在任何人的牌河里，客户端没法从 state_sync 推出来，必须显式带上。
    frame.Set("tile", json::Value::Str(FormatTile(*claimed, /*red=*/true)));
  }
  if (has_riichi_tiles) {
    // v1.21 §4.4：立直宣言牌的候选牌面（打出后仍听牌、且不落振听）。
    // 恒用普通牌面码（5m 而不是 0m）：候选是**牌面**，赤 5 与普通 5 作宣言牌等价。
    frame.Set("riichi_tiles", TilesToJsonFaces(riichi_tiles));
  }
  if (has_forbidden) {
    // v2.5 §12.15：喰い替え禁止 —— 本窗口打不出去的牌面。只在打牌窗口、且确实有限制时出现，
    // 空表**不写该键**（绝大多数窗口因此与 v2.4 逐字节一致）。
    frame.Set("forbidden_discards", FacesToJson(forbidden_discards));
  }
  return frame.Dump();
}

std::string BuildActionEventFrame(int seat, protocol::Action action, const std::vector<Tile>& tiles, int from_seat,
                                  uint64_t seq) {
  json::Value frame = json::Value::Obj(6);  // type, seat, action, tiles, seq (+ from_seat)
  frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kActionEvent)));
  frame.Set("seat", json::Value::Int(seat));
  frame.Set("action", json::Value::Int(protocol::WireOf(action)));
  frame.Set("tiles", TilesToJson(tiles));
  if (from_seat >= 0) {  // a tsumo win MUST NOT carry from_seat (PROTOCOL.md §5.8)
    frame.Set("from_seat", json::Value::Int(from_seat));
  }
  frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
  return frame.Dump();
}

int ActionPriority(protocol::Action action) {
  if (action == protocol::Action::kWin) return 4;
  if (action == protocol::Action::kKong) return 3;
  if (action == protocol::Action::kPong) return 2;
  if (action == protocol::Action::kChow) return 1;
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

// 赤 5 与普通 5 是**两张不同的物理牌**（v1.18）：先把 id 精确匹配的移除掉，
// 找不到才退回「按牌面移除最左一张」（AI 与旧客户端只发牌面，`5m` 会 ParseTile 成 copy 0）。
// classic 不把赤牌序列化成 `0m`，但同一段代码要为「与 riichi 同形」的牌张保持确定性。
std::optional<Tile> RemoveOneTile(std::vector<Tile>* hand, Tile tile) {
  auto exact = std::find(hand->begin(), hand->end(), tile);
  if (exact == hand->end()) {
    // 退回牌面匹配时**同样优先普通 5**：手里同时有 `5m` 与 `0m` 却只报了 `5m`，不该把赤牌打出去。
    const Face face = FaceOf(tile);
    for (auto it = hand->begin(); it != hand->end(); ++it) {
      if (FaceOf(*it) != face) {
        continue;
      }
      if (!IsRedFive(*it)) {
        exact = it;
        break;
      }
      if (exact == hand->end()) {
        exact = it;  // 兜底：先记下赤牌，后面若出现普通 5 会被上面的 break 抢先
      }
    }
  }
  if (exact == hand->end()) {
    return std::nullopt;
  }
  const Tile removed = *exact;
  hand->erase(exact);
  return removed;
}

// Removes and returns up to |count| physical tiles of |face| from the hand.
// 赤 5（v1.18）：同一牌面里**优先扣普通 5**，赤牌留在手里继续计番；只有普通 5 不够时才动赤牌。
std::vector<Tile> TakeFaces(std::vector<Tile>* hand, Face face, int count) {
  std::vector<Tile> taken;
  for (int i = 0; i < count; ++i) {
    std::optional<size_t> pick;
    // 第一轮只找普通 5；找不到再退回「随便一张同面」（必是赤牌）。
    for (size_t index = 0; index < hand->size() && !pick.has_value(); ++index) {
      if (FaceOf((*hand)[index]) == face && !IsRedFive((*hand)[index])) {
        pick = index;
      }
    }
    if (!pick.has_value()) {
      for (size_t index = 0; index < hand->size() && !pick.has_value(); ++index) {
        if (FaceOf((*hand)[index]) == face) {
          pick = index;
        }
      }
    }
    if (!pick.has_value()) {
      break;
    }
    taken.push_back((*hand)[*pick]);
    hand->erase(hand->begin() + static_cast<std::ptrdiff_t>(*pick));
  }
  return taken;
}

MeldType MeldTypeForAction(protocol::Action action) {
  if (action == protocol::Action::kPong) return MeldType::kPong;
  if (action == protocol::Action::kChow) return MeldType::kChow;
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

// The launch options are the wire's `length` selector, encoded by the registry as
// {"length":N}. Anything unreadable means "tonpu": the server's default game, and the one every
// client that never sends the field gets.
constexpr int kHanchanLengthOption = 1;

int ParseLengthOption(const std::string& options_json) {
  if (options_json.empty()) {
    return 0;
  }
  const std::optional<json::Value> parsed = json::Value::Parse(options_json, nullptr);
  if (!parsed.has_value()) {
    spdlog::error("riichi: unreadable options \"{}\"; dealing a tonpu match", options_json);
    return 0;
  }
  if (const json::Value* length = parsed->Find("length"); length != nullptr && length->IsInteger()) {
    return static_cast<int>(length->AsInt());
  }
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Game entry points
// ---------------------------------------------------------------------------------------

GameStep RiichiMahjongGame::Setup(const GameContext& context) {
  room_id_ = context.room_id;
  seed_ = context.seed;
  action_timeout_ms_ = context.action_timeout_ms;
  ai_delay_ms_ = context.ai_delay_ms < 0 ? kRiichiDefaultAiDelayMs : context.ai_delay_ms;
  inter_hand_delay_ms_ = context.inter_hand_delay_ms < 0 ? kRiichiDefaultInterHandDelayMs : context.inter_hand_delay_ms;
  deadlines_supported_ = context.deadlines_supported;
  seats_ = context.seats;
  // Seat number == index everywhere in this class; make that true even if the caller handed
  // the seats over in another order.
  std::sort(seats_.begin(), seats_.end(), [](const GameSeat& lhs, const GameSeat& rhs) { return lhs.seat < rhs.seat; });
  if (seats_.size() != static_cast<size_t>(kSeatCount)) {
    spdlog::error("room {}: riichi needs exactly {} seats, got {}", room_id_, kSeatCount, seats_.size());
  }
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (seats_[static_cast<size_t>(seat)].seat != seat) {
      spdlog::error("room {}: seat table is not dense at {} (got {})", room_id_, seat,
                    seats_[static_cast<size_t>(seat)].seat);
    }
  }

  // 局数来自玩法自己的配置串（lobby 只认识线级的 mode + length，把它编码进 options_json）。
  round_count_ = RiichiRoundCountOf(ParseLengthOption(context.options_json));

  GameStep step;
  StartMatch(step);
  return step;
}

int RiichiMahjongGame::SeatOf(const std::string& player_id) const {
  for (const GameSeat& seat : seats_) {
    if (!seat.is_bot && seat.player_id == player_id) {
      return seat.seat;
    }
  }
  return -1;
}

// Seats that may act right now. A discard window belongs to exactly one seat; a claim window
// belongs to the recipients that have not answered yet. Used by tests and by anything that
// needs to ask "who is the table waiting for" without knowing the rules.
std::vector<int> RiichiMahjongGame::ActiveSeats() const {
  std::vector<int> seats;
  if (phase_ == Phase::kDiscardWindow && discard_window_open_) {
    seats.push_back(current_seat_);
    return seats;
  }
  if (phase_ == Phase::kClaimWindow && claim_open_) {
    for (int seat = 0; seat < kSeatCount; ++seat) {
      if (claim_recipient_[static_cast<size_t>(seat)] && !claim_answers_[static_cast<size_t>(seat)].answered) {
        seats.push_back(seat);
      }
    }
  }
  return seats;
}

void RiichiMahjongGame::OnSeatPresence(int seat, bool present) {
  if (seat < 0 || seat >= kSeatCount) {
    return;
  }
  disconnected_[static_cast<size_t>(seat)] = !present;
}

// The room's gate (is this seat's turn, is the window still open, is that tile in hand) lives
// here, and so does the §6.3 rule that a reply for a window which already moved on is dropped
// *silently* while a well-formed action the rules refuse is answered. Nothing in this function
// mutates: it is called on every inbound frame, including the ones that are then dropped.
GameVerdict RiichiMahjongGame::Validate(int seat, const json::Value& frame) const {
  if (!game_started_) {
    return GameVerdict::Drop();
  }
  // autoplay carries no seq: it is a seat flag, not a window reply. A bot seat is refused
  // here so is_bot can never be cleared by this frame.
  if (const json::Value* type = frame.Find("type");
      type != nullptr && type->IsInteger() &&
      protocol::ParseFrameType(type->AsInt()) == protocol::FrameType::kAutoplay) {
    if (phase_ == Phase::kMatchEnd || IsAutomated(seat)) {
      return GameVerdict::Drop();
    }
    const json::Value* on = frame.Find("on");
    if (on == nullptr || !on->IsBool()) {
      return GameVerdict::Refuse(protocol::ErrorCode::kBadRequest, "autoplay requires a boolean \"on\"");
    }
    return GameVerdict::Accept();
  }
  // v1.6（C4/C13）：只有**场终**才静默早退；局终后的对局帧回 BAD_REQUEST（新局未开始，
  // 客户端不得把 game_end 当作可继续对局的信号）。
  if (phase_ == Phase::kMatchEnd) {
    return GameVerdict::Drop();
  }
  if (phase_ == Phase::kRoundEnd) {
    return GameVerdict::Refuse(protocol::ErrorCode::kBadRequest, "the round has ended; wait for the next game_start");
  }
  const json::Value* type = frame.Find("type");
  const json::Value* seq_value = frame.Find("seq");
  if (type == nullptr || !type->IsInteger() || seq_value == nullptr || !seq_value->IsInteger()) {
    return GameVerdict::Drop();  // PlayerActor already answers BAD_REQUEST for a malformed frame
  }
  if (static_cast<uint64_t>(seq_value->AsInt()) != seq_) {
    // PROTOCOL.md §6.3: a reply for a window that already moved on is dropped silently.
    spdlog::debug("room {}: stale frame type {} (seq {} != {}) dropped", room_id_, type->AsInt(), seq_value->AsInt(),
                  seq_);
    return GameVerdict::Drop();
  }
  const std::optional<protocol::FrameType> frame_type = protocol::ParseFrameType(type->AsInt());
  if (frame_type == protocol::FrameType::kDiscard || frame_type == protocol::FrameType::kAction) {
    // A click that races the handoff must not be charged to the illegal-op quota.
    if (autoplay_[static_cast<size_t>(seat)]) {
      return GameVerdict::Drop();
    }
  }
  if (frame_type == protocol::FrameType::kDiscard) {
    return ValidateDiscard(seat, frame);
  }
  if (frame_type == protocol::FrameType::kAction) {
    return ValidateAction(seat, frame);
  }
  return GameVerdict::Drop();
}

GameVerdict RiichiMahjongGame::ValidateDiscard(int seat, const json::Value& frame) const {
  const json::Value* tile_value = frame.Find("tile");
  if (phase_ != Phase::kDiscardWindow || !discard_window_open_ || seat != current_seat_) {
    return GameVerdict::Refuse(protocol::ErrorCode::kNotYourTurn, "there is no discard window for you");
  }
  if (tile_value == nullptr || !tile_value->IsString()) {
    return GameVerdict::Refuse(protocol::ErrorCode::kBadRequest, "discard requires a string \"tile\"");
  }
  std::optional<Tile> tile = ParseTile(tile_value->AsString());
  // 赤 5（`0m`/`0p`/`0s`）必须真的在手里那一张；普通 `5m` 只要求牌面在手（可能是赤牌那张）。
  const std::vector<Tile>& hand = hands_[static_cast<size_t>(seat)];
  const bool holds_tile =
      tile.has_value() &&
      (IsRedFive(*tile) ? std::find(hand.begin(), hand.end(), *tile) != hand.end() : HandHasFace(hand, FaceOf(*tile)));
  if (!tile.has_value() || !holds_tile) {
    return GameVerdict::Refuse(protocol::ErrorCode::kUnknownTile,
                               "tile is not in your hand: " + tile_value->AsString());
  }
  if (RiichiDeclared(seat) && drawn_tile_.has_value() && FaceOf(*tile) != FaceOf(*drawn_tile_)) {
    // 立直后手牌锁定：只能把刚摸到的那张打出去（摸切）。宣言那一条走 action{riichi}，不经过这里。
    return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction,
                               "after riichi you may only discard the tile you drew");
  }
  if (IsKuikaeForbidden(seat, *tile)) {
    // 喰い替え禁止（v2.5）：刚吃/碰完的那一巡，不能打回刚鸣的牌，也不能打它的スジ。
    return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction,
                               "this tile cannot be discarded right after a claim (kuikae): " + tile_value->AsString());
  }
  return GameVerdict::Accept();
}

// H1 (v1.6): a discard window accepts `discard`, or an action **in the window's options**
// (`win` / `ankan` / `kakan`); `pass` is still ILLEGAL_ACTION. Everything is recomputed with
// the rules library (H8: never trust the client), and nothing here mutates — the same checks
// run again as guards in ApplyActionFrame only where a mutation needs the parsed value.
GameVerdict RiichiMahjongGame::ValidateAction(int seat, const json::Value& frame) const {
  const json::Value* action_value = frame.Find("action");
  const json::Value* tiles_value = frame.Find("tiles");
  if (action_value == nullptr || !action_value->IsInteger() || tiles_value == nullptr || !tiles_value->IsArray()) {
    return GameVerdict::Refuse(protocol::ErrorCode::kBadRequest, "action requires \"action\" and \"tiles\"");
  }
  std::vector<Tile> tiles;
  for (const json::Value& entry : tiles_value->AsArray()) {
    if (!entry.IsString()) {
      return GameVerdict::Refuse(protocol::ErrorCode::kBadRequest, "\"tiles\" must only contain strings");
    }
    std::optional<Tile> tile = ParseTile(entry.AsString());
    if (!tile.has_value()) {
      return GameVerdict::Refuse(protocol::ErrorCode::kUnknownTile, "invalid tile code: " + entry.AsString());
    }
    tiles.push_back(*tile);
  }
  // frame_parser 已把未知动作拦成 BAD_REQUEST；这里再解析一次拿枚举，后续全部按枚举判定。
  const std::optional<protocol::Action> parsed_action = protocol::ParseAction(action_value->AsInt());
  if (!parsed_action.has_value()) {
    return GameVerdict::Refuse(protocol::ErrorCode::kBadRequest,
                               "unknown action: " + std::to_string(action_value->AsInt()));
  }
  const protocol::Action action = *parsed_action;

  if (phase_ == Phase::kDiscardWindow && discard_window_open_) {
    if (seat != current_seat_) {
      return GameVerdict::Refuse(protocol::ErrorCode::kNoPendingAction, "the discard window belongs to another seat");
    }
    if (action == protocol::Action::kAnkan || action == protocol::Action::kKakan) {
      // v1.6 自理杠（PROTOCOL §4.6）：只在打牌窗口、tiles 恰 1 个牌面元素、以窗口 options 为准，
      // 且按规则库重算合法性（§871）。合法性由 DiscardOptions 已含 remaining ≥ 1 守卫。
      if (tiles.size() != 1) {
        return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction,
                                   std::string(protocol::NameOf(action)) + " must carry exactly one tile face");
      }
      const std::vector<Option> options = DiscardOptionsFor(seat);
      const Option* option = FindOption(options, action);
      if (option == nullptr || !SameFaceMultiset(tiles, option->tiles)) {
        return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction,
                                   std::string(protocol::NameOf(action)) + " is not available in this window");
      }
      return GameVerdict::Accept();
    }
    if (action == protocol::Action::kRiichi) {
      // 立直宣言：与打牌是同一条 action（tiles 恰 1 个牌面 = 宣言后打出的那张）。
      // classic 在同一个位置回的是 "riichi is not enabled in this room"——那是它没有立直；
      // 这里立直就是本玩法，所以只校验「此刻能不能宣言」。
      if (tiles.size() != 1) {
        return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction, "riichi must carry exactly one discard tile");
      }
      if (!CanDeclareRiichiNow(seat, tiles.front())) {
        return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction, "cannot declare riichi now");
      }
      return GameVerdict::Accept();
    }
    if (action != protocol::Action::kWin) {
      return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction,
                                 "the discard window only accepts a discard or a win");
    }
    if (!tiles.empty()) {
      return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction, "win must not carry tiles");
    }
    if (!discard_window_win_check_) {
      // PROTOCOL §6.1/§6.3: the window this seat answered offered `options: []`, so a `win`
      // here is outside its options → ILLEGAL_ACTION. Judged by what the window offered, never
      // by a recomputation: right after a pong/chow the concealed hand can already form a
      // complete hand, and recomputing would turn "pong" into a free self-draw win.
      return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction, "win is not available in this window");
    }
    const std::vector<Option> options = DiscardOptionsFor(seat);
    if (FindOption(options, protocol::Action::kWin) == nullptr) {
      return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction, "win is not available in this window");
    }
    // H8: never trust the client — recompute with the rules library.
    // 用日麻的牌型口径（`IsWinShape`）：共享的 `IsWinningHand` 是番数制口径，认不出国士无双，
    // 而窗口选项（`riichi::DiscardOptions`）已经把国士的 win 放进去了 —— 两处口径必须一致。
    if (!IsWinShape(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)], TsumoWinningTile(seat),
                    /*is_tsumo=*/true)) {
      return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction, "the hand is not a winning hand");
    }
    return GameVerdict::Accept();
  }

  if (phase_ != Phase::kClaimWindow || !claim_open_ || !claim_recipient_[static_cast<size_t>(seat)]) {
    return GameVerdict::Refuse(protocol::ErrorCode::kNoPendingAction, "there is no action window for you");
  }
  if (claim_answers_[static_cast<size_t>(seat)].answered) {
    return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction, "this seat already answered the window");
  }
  const std::vector<Option>& seat_options = claim_options_[static_cast<size_t>(seat)];
  if (FindOption(seat_options, action) == nullptr) {
    return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction,
                               "action is not in this window's options: " + std::string(protocol::NameOf(action)));
  }
  const std::vector<Tile>& hand = hands_[static_cast<size_t>(seat)];
  const std::vector<Meld>& melds = melds_[static_cast<size_t>(seat)];
  if (action == protocol::Action::kWin) {
    if (!tiles.empty()) {
      return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction, "win must not carry tiles");
    }
    if (!IsWinShape(hand, melds, claim_tile_, /*is_tsumo=*/false)) {
      return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction, "the hand is not a winning hand");
    }
  } else if (action == protocol::Action::kPong) {
    if (!SameFaceMultiset(tiles, {claim_tile_}) || !CanPong(hand, claim_tile_)) {
      return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction, "pong is not legal for this tile");
    }
  } else if (action == protocol::Action::kKong) {
    if (!SameFaceMultiset(tiles, {claim_tile_}) || !CanKong(hand, claim_tile_, remaining_)) {
      return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction, "kong is not legal for this tile");
    }
  } else if (action == protocol::Action::kChow) {
    if (seat != (claim_from_seat_ + 1) % kSeatCount) {
      return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction, "only the next player may chow");
    }
    const std::vector<std::array<Tile, 2>> combinations = ChowCombinations(hand, claim_tile_);
    const bool matches =
        std::any_of(combinations.begin(), combinations.end(),
                    [&tiles](const std::array<Tile, 2>& pair) { return SameFaceMultiset(tiles, {pair[0], pair[1]}); });
    if (!matches) {
      return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction,
                                 "chow tiles do not form a run with the discarded tile");
    }
  } else if (action == protocol::Action::kPass) {
    if (!tiles.empty()) {
      return GameVerdict::Refuse(protocol::ErrorCode::kIllegalAction, "pass must not carry tiles");
    }
  }
  return GameVerdict::Accept();
}

GameStep RiichiMahjongGame::Apply(int seat, const json::Value& action) {
  GameStep step;
  const json::Value* type = action.Find("type");
  if (type == nullptr || !type->IsInteger()) {
    return step;
  }
  const std::optional<protocol::FrameType> frame_type = protocol::ParseFrameType(type->AsInt());
  if (frame_type == protocol::FrameType::kAutoplay) {
    const json::Value* on = action.Find("on");
    if (on != nullptr && on->IsBool()) {
      ApplyAutoplay(step, seat, on->AsBool());
    }
  } else if (frame_type == protocol::FrameType::kDiscard) {
    ApplyDiscardFrame(step, seat, action);
  } else if (frame_type == protocol::FrameType::kAction) {
    ApplyActionFrame(step, seat, action);
  }
  return step;
}

void RiichiMahjongGame::EmitOpenRequest(GameStep& step, int seat) {
  if (!SeatPresent(seat) || window_deadline_ms_ <= clock::EpochMs()) {
    return;
  }
  if (phase_ == Phase::kDiscardWindow && discard_window_open_ && current_seat_ == seat) {
    Emit(step, seat,
         BuildActionRequestFrame(seq_, window_deadline_ms_, protocol::Window::kDiscard, discard_options_, std::nullopt,
                                 discard_riichi_tiles_, KuikaeFacesFor(seat)));
  } else if (phase_ == Phase::kClaimWindow && claim_open_ && claim_recipient_[static_cast<size_t>(seat)] &&
             !claim_answers_[static_cast<size_t>(seat)].answered) {
    Emit(step, seat,
         BuildActionRequestFrame(seq_, window_deadline_ms_,
                                 claim_chankan_ ? protocol::Window::kChankan : protocol::Window::kClaim,
                                 claim_options_[static_cast<size_t>(seat)],
                                 claim_chankan_ ? std::optional<Tile>(claim_tile_) : std::nullopt));
  }
}

void RiichiMahjongGame::ApplyAutoplay(GameStep& step, int seat, bool on) {
  // A bot is already played by the AI. This frame must not clear is_bot.
  if (seat < 0 || seat >= kSeatCount || IsAutomated(seat)) {
    return;
  }
  const size_t index = static_cast<size_t>(seat);
  if (autoplay_[index] == on) {
    EmitPerSeat(step, StateSyncFrames());
    return;
  }
  autoplay_[index] = on;
  spdlog::info("room {}: seat {} autoplay {}", room_id_, seat, on ? "on" : "off");
  const bool discard_here = phase_ == Phase::kDiscardWindow && discard_window_open_ && current_seat_ == seat;
  const bool claim_here =
      phase_ == Phase::kClaimWindow && claim_open_ && claim_recipient_[index] && !claim_answers_[index].answered;
  if (claim_here) {
    if (on) {
      if (claim_pending_humans_ > 0) {
        --claim_pending_humans_;
      }
      if (ai_delay_ms_ > 0) {
        if (!ai_claim_pending_) {
          if (ScheduleAiAction(step, ai_delay_ms_)) {
            ai_claim_pending_ = true;
          } else {
            ResolveAutomatedClaimAnswers();
          }
        }
      } else {
        ResolveAutomatedClaimAnswers();
      }
    } else {
      const bool timer_already = claim_pending_humans_ > 0;
      ++claim_pending_humans_;
      EmitOpenRequest(step, seat);
      if (!timer_already) {
        OpenWindowTimer(step);
      }
    }
  }
  if (discard_here && !on) {
    // The pacing message stays armed; clearing the flag makes it a no-op. The original
    // window timer still owns the deadline, so the replayed request keeps that deadline.
    ai_discard_pending_ = false;
    EmitOpenRequest(step, seat);
  }
  EmitPerSeat(step, StateSyncFrames());
  if (on && (discard_here || claim_here)) {
    Pump(step);
  }
}

void RiichiMahjongGame::ApplyDiscardFrame(GameStep& step, int seat, const json::Value& frame) {
  const json::Value* tile_value = frame.Find("tile");
  if (tile_value == nullptr || !tile_value->IsString()) {
    return;  // Validate already refused this
  }
  const std::optional<Tile> tile = ParseTile(tile_value->AsString());
  if (!tile.has_value()) {
    return;
  }
  discard_window_open_ = false;
  ApplyDiscard(step, seat, *tile);
  Pump(step);
}

void RiichiMahjongGame::ApplyActionFrame(GameStep& step, int seat, const json::Value& frame) {
  const json::Value* action_value = frame.Find("action");
  const json::Value* tiles_value = frame.Find("tiles");
  if (action_value == nullptr || !action_value->IsInteger() || tiles_value == nullptr || !tiles_value->IsArray()) {
    return;
  }
  const std::optional<protocol::Action> parsed_action = protocol::ParseAction(action_value->AsInt());
  if (!parsed_action.has_value()) {
    return;
  }
  const protocol::Action action = *parsed_action;
  std::vector<Tile> tiles;
  for (const json::Value& entry : tiles_value->AsArray()) {
    if (!entry.IsString()) {
      continue;
    }
    if (const std::optional<Tile> tile = ParseTile(entry.AsString()); tile.has_value()) {
      tiles.push_back(*tile);
    }
  }

  if (phase_ == Phase::kDiscardWindow && discard_window_open_) {
    if (action == protocol::Action::kAnkan || action == protocol::Action::kKakan) {
      discard_window_open_ = false;
      if (action == protocol::Action::kKakan) {
        // 加杠的那张牌先给三家一次抢杠机会。确认无人抢才真正落地，所以这里不能直接
        // ApplySelfKan —— 那样第 4 张已经进了杠、宝牌也翻了，追不回来。
        // 赤 5：`ApplySelfKan` 优先拿普通 5，抢杠窗口里的「被抢牌」必须与真正离手的那张一致，
        // 否则会把一张不存在的赤牌记成和牌张（凭空多 1 番）。
        const Face kan_face = FaceOf(tiles.front());
        Tile actual = tiles.front();
        for (Tile candidate : hands_[static_cast<size_t>(seat)]) {
          if (FaceOf(candidate) == kan_face && !IsRedFive(candidate)) {
            actual = candidate;
            break;
          }
        }
        OpenChankanWindow(step, seat, actual);
        return;
      }
      ApplySelfKan(step, seat, action, tiles);
      return;
    }
    if (action == protocol::Action::kRiichi) {
      // 宣言 → 收棒、广播 event、再打那张（宣言牌横置）。Validate 已核过 CanDeclareRiichiNow。
      discard_window_open_ = false;
      DeclareRiichi(step, seat);
      ApplyDiscard(step, seat, tiles.front());
      Pump(step);
      return;
    }
    // The only remaining accepted action in a discard window is a self-draw win.
    discard_window_open_ = false;
    EndRoundWithWin(step, seat, /*from_seat=*/-1, TsumoWinningTile(seat), /*tsumo=*/true);
    return;
  }

  ClaimAnswer answer;
  answer.answered = true;
  answer.action = action;
  answer.tiles = tiles;
  claim_answers_[static_cast<size_t>(seat)] = std::move(answer);
  if (claim_pending_humans_ > 0) {
    --claim_pending_humans_;
  }
  Pump(step);
}

// ---------------------------------------------------------------------------------------
// Turn driving
// ---------------------------------------------------------------------------------------

uint64_t RiichiMahjongGame::Transition() {
  // PROTOCOL.md §6.2 / GAME_DESIGN §4.3: seq advances once per window transition, and a
  // window opened by this transition carries the new value.
  ++seq_;
  return seq_;
}

void RiichiMahjongGame::StartMatch(GameStep& step) {
  // v1.6（C2）：一场一次。这里只动**场次**状态；局内状态全部归 StartRound（C3 清单）。
  game_started_ = true;
  round_index_ = 0;
  hands_played_ = 0;
  renchan_count_ = 0;
  match_totals_ = {};
  dealer_hands_ = {};  // B3：坐庄手数随场次清零
  tenpai_seq_ = {};    // v1.7 §5.16：tenpai_seq 从 1 起；跨局不重置，新场次才归零
  dealer_ = 0;         // 首局庄家 = seat 0（GAME_DESIGN v1.10 §3.3）
  match_end_pending_ = false;
  // 起家 25000 点、无供托/本场。round_count_ 由 Setup 按 options 定（东场 4 / 半庄 8）。
  points_.fill(riichi::kStartPoints);
  kyoutaku_ = 0;
  honba_ = 0;
  round_wind_ = 0;  // 东场 1 局：round_index_ = 0
  StartRound(step);
}

void RiichiMahjongGame::StartRound(GameStep& step) {
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
  discard_options_.clear();
  discard_riichi_tiles_.clear();
  for (auto& kuikae : kuikae_faces_) {
    kuikae.clear();
  }
  // 场风只由「打到第几局」决定：东场 0..3 ⇒ 东，南场 4..7 ⇒ 南。连庄不推进 round_index，
  // 因此续庄不会换场风。
  round_wind_ = riichi::RoundWindOf(round_index_);
  // 局内状态：立直 / 一发 / 振听每局清零
  riichi_ = {};
  ippatsu_ = {};
  ippatsu_armed_ = {};
  furiten_ever_ = {};
  for (auto& temporary : temporary_furiten_) {
    temporary.fill(false);
  }
  draw_dealer_tenpai_ = false;

  // v1.6 局种子：**房间种子只管「房间之间不同」，局种子管「局与局之间不同」，两者缺一不可**。
  // 只用 seed_ 时一场 4 局的牌山会逐局完全相同（用户实玩 blocker：第 1 局与第 2 局起手牌
  // 一模一样的根因）。hands_played_ 在 AdvanceAfterRound 里逐局 +1，且在本行之前已更新到
  // 「本局序号」（第 1 手 = 0），所以它就是 hand_index。
  wall_ = BuildWall(DeriveRoundSeed(seed_, static_cast<uint32_t>(hands_played_)));
  // C9：庄家显式传参（B1 改造后的签名）。发牌后不变量：庄家 14 张、其余 13 张、牌山 83。
  Deal deal = DealTiles(wall_, dealer_);
  hands_ = deal.hands;  // hands[dealer] is the dealer (14 tiles), the others 13
  wall_ = std::move(deal.wall);
  next_index_ = deal.next_index;
  // 末尾 14 张是王牌（0..3 岭上 / 4..8 宝牌指示 / 9..13 里宝指示），可摸区只到 122。
  drawable_end_ = kTileCount - 14;
  dead_wall_.assign(wall_.begin() + drawable_end_, wall_.end());
  dead_draw_index_ = 0;
  dora_indicators_.clear();
  FlipDoraIndicator();  // 开局翻开第一张宝牌指示牌
  remaining_ = drawable_end_ - next_index_;
  for (std::vector<Tile>& hand : hands_) {
    SortTiles(hand);
  }
  current_seat_ = dealer_;
  const int expected_remaining = drawable_end_ - 53;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    const size_t expected = seat == dealer_ ? 14u : 13u;
    if (hands_[static_cast<size_t>(seat)].size() != expected || remaining_ != expected_remaining) {
      spdlog::error("room {}: deal invariant broken (dealer={} hand={} remaining={})", room_id_, dealer_,
                    hands_[static_cast<size_t>(seat)].size(), remaining_);
    }
  }
  Transition();  // the dealer's first discard window carries the new seq（seq 跨局单调，不归零）

  std::array<std::string, kSeatCount> starts;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (seats_[static_cast<size_t>(seat)].wired) {
      starts[static_cast<size_t>(seat)] = BuildGameStart(seat);
    }
  }
  EmitPerSeat(step, std::move(starts));
  spdlog::info("room {}: round started (round_index={}, dealer={}, remaining={}, seq={})", room_id_, round_index_,
               dealer_, remaining_, seq_);

  // Dealer's first discard window: no draw record, but a 14-tile hand may already be a win
  // (天和, GAME_DESIGN §4.2 step 2).
  OpenDiscardWindow(step, /*win_check=*/true, std::nullopt);
  phase_ = Phase::kDiscardWindow;
  Pump(step);
}

void RiichiMahjongGame::Pump(GameStep& step) {
  for (int iteration = 0; iteration < kMaxPumpSteps; ++iteration) {
    switch (phase_) {
      case Phase::kIdle:
      case Phase::kRoundEnd:
      case Phase::kMatchEnd:
        return;

      case Phase::kTurnStart: {
        if (remaining_ <= 0) {
          EndRoundWithDraw(step);
          return;
        }
        DrawForCurrentSeat(step);
        break;  // -> kDiscardWindow
      }

      case Phase::kDiscardWindow: {
        const int seat = current_seat_;
        if (NeedsPlayerReply(seat)) {
          // The request was sent when the window opened; wait for the reply or for the
          // window timeout message (PROTOCOL.md §0.5).
          return;
        }
        // Two kinds of automated seats share this branch, and they deliberately differ:
        //   * a bot seat plays the tile-efficiency policy (t20): keep the hand whose shanten
        //     after the next draw is best — declared on its own draw, and paced by
        //     ai_delay_ms_ so a human can follow the table (GAME_DESIGN v1.4 §5.4);
        //   * a human seat with no wire keeps the frozen deterministic fallback of
        //     GAME_DESIGN §5.6.2 — play the tile just drawn, or the largest tile when the
        //     window has no draw record — immediately: there is nobody to answer it. A
        //     disconnected seat with a wire is NOT here; its window waits for the deadline.
        if (PlaysWithAi(seat) && ai_delay_ms_ > 0 && (ai_discard_pending_ || ScheduleAiAction(step, ai_delay_ms_))) {
          ai_discard_pending_ = true;
          return;  // the decision arrives as a deadline; the table keeps serving human frames
        }
        if (!PlayAutomatedDiscard(step)) {
          return;
        }
        break;
      }

      case Phase::kClaimWindow: {
        if (claim_pending_humans_ > 0) {
          return;  // waiting for those seats' action or for their timeout message
        }
        if (ai_claim_pending_) {
          // GAME_DESIGN v1.4 §5.4: the bots' responses appear ai_delay_ms after the discard.
          // A human `win` is the top priority and no bot answer can change it, so the human
          // is served immediately instead of waiting the pacing out.
          if (!HasPendingHumanWin()) {
            return;
          }
          ResolveAutomatedClaimAnswers();
        }
        ResolveClaimWindow(step);
        break;
      }
    }
  }
  spdlog::error("room {}: pump step limit reached (phase={})", room_id_, static_cast<int>(phase_));
}

void RiichiMahjongGame::DrawForCurrentSeat(GameStep& step) {
  const int seat = current_seat_;
  // 立直后跨过「下一摸」：这一摸仍然可以一发，打完这张就失效
  ippatsu_armed_[static_cast<size_t>(seat)] = false;
  // 同巡振听结束：摸牌即「跨过这一巡」，本巡放过的牌面重新可和。
  temporary_furiten_[static_cast<size_t>(seat)].fill(false);
  Tile tile = wall_[static_cast<size_t>(next_index_)];
  ++next_index_;
  remaining_ = drawable_end_ - next_index_;
  hands_[static_cast<size_t>(seat)].push_back(tile);
  SortTiles(hands_[static_cast<size_t>(seat)]);

  spdlog::debug("room {}: seat {} draws {} (remaining={}, seq={})", room_id_, seat, FormatTile(tile), remaining_, seq_);

  if (seats_[static_cast<size_t>(seat)].wired) {
    Emit(step, seat, BuildDrawFrame(tile, seat, seq_));
  }
  EmitPerSeat(step, StateSyncFrames());

  OpenDiscardWindow(step, /*win_check=*/true, tile);
  phase_ = Phase::kDiscardWindow;
}

void RiichiMahjongGame::OpenDiscardWindow(GameStep& step, bool win_check, std::optional<Tile> drawn_tile) {
  drawn_tile_ = drawn_tile;
  discard_window_open_ = true;
  ai_discard_pending_ = false;  // a fresh window schedules its own pacing message
  discard_window_win_check_ = win_check;
  const int seat = current_seat_;
  // Clear any previous window's replay data before the early return for an automated seat.
  window_deadline_ms_ = 0;
  discard_options_.clear();
  discard_riichi_tiles_.clear();
  if (!NeedsPlayerReply(seat)) {
    return;  // bots / seats with no wire act inside this same step: no frame, no deadline
  }
  // A disconnected human still gets a real window here: the deadline is the only path that may
  // play for them, and a reconnect before it replays this exact request (PROTOCOL v2.3 §8.5).
  std::vector<Option> options;
  std::vector<Tile> riichi_tiles;
  if (win_check) {
    // 显式传 remaining_，缺省 −1 永远不出杠选项。日麻下由 DiscardOptionsFor 追加立直选项、
    // 并在已立直时去掉 kakan；这里同时取走「立直宣言牌候选」，随帧下发（v1.21 §4.4）。
    options = DiscardOptionsFor(seat, &riichi_tiles);
  }
  // PROTOCOL v2.3 §8.5: remember the exact window so a reconnect can replay it instead of
  // guessing options or granting a fresh deadline.
  window_deadline_ms_ = clock::EpochMs() + action_timeout_ms_;
  discard_options_ = options;
  discard_riichi_tiles_ = riichi_tiles;
  if (SeatPresent(seat)) {
    // 喰い替え禁止（v2.5）：刚吃/碰完的当巡，禁掉的牌面随帧下发，客户端据此压暗它们。
    // 每次下发都现算（不进 replay 快照）：它是 `kuikae_faces_` 与手牌的纯函数，重放时同值。
    Emit(step, seat,
         BuildActionRequestFrame(seq_, window_deadline_ms_, protocol::Window::kDiscard, options, std::nullopt,
                                 riichi_tiles, KuikaeFacesFor(seat)));
  }
  OpenWindowTimer(step);
}

void RiichiMahjongGame::ApplyDiscard(GameStep& step, int seat, Tile tile) {
  const uint64_t old_seq = seq_;
  if (ippatsu_[static_cast<size_t>(seat)] && !ippatsu_armed_[static_cast<size_t>(seat)] && drawn_tile_.has_value()) {
    ippatsu_[static_cast<size_t>(seat)] = false;  // 摸打一巡已过，一发失效
  }
  // 赤 5（v1.18）与普通 5 是两张**不同**的物理牌：优先按 id 精确移除，退不到再按牌面移。
  // **牌河里要放真正离手的那一张**（而不是客户端报文里的那张），否则「手里有赤 5、打 5m」
  // 会让手牌和牌河的赤/常显示对不上。
  const Tile discarded = RemoveOneTile(&hands_[static_cast<size_t>(seat)], tile).value_or(tile);
  tile = discarded;
  discards_[static_cast<size_t>(seat)].push_back(discarded);
  // 喰い替え禁止只管「鸣牌后的那一张」：这张打出去，当巡的限制就用完了（PROTOCOL §12.15）。
  kuikae_faces_[static_cast<size_t>(seat)].clear();
  drawn_tile_.reset();
  discard_window_open_ = false;
  discard_window_win_check_ = false;
  kong_draw_tile_.reset();  // 打出即放弃杠上开花机会（该窗口的补牌记录作废）

  Transition();  // close the discard window; a claim window (if any) takes the new seq

  // One frame for the whole table (PROTOCOL.md §5.6: `discard_event` is broadcast and carries
  // nothing seat-specific), so it is built once and handed to every wired seat.
  EmitAll(step, BuildDiscardEventFrame(seat, tile, old_seq));
  spdlog::debug("room {}: seat {} discards {} (seq={})", room_id_, seat, FormatTile(tile), old_seq);
  // v1.7 §5.16 T1：本家打牌生效后（手牌回到 13 张等待态）——听牌唯一有定义的状态，主时机。
  DeliverTenpai(step, seat);

  std::array<bool, kSeatCount> recipients {};
  std::array<std::vector<Option>, kSeatCount> options;
  bool any_claim = false;
  for (int other = 0; other < kSeatCount; ++other) {
    if (other == seat) {
      continue;
    }
    options[static_cast<size_t>(other)] =
        ClaimOptionsFor(other, tile, /*is_next_player=*/other == (seat + 1) % kSeatCount, /*from_seat=*/seat);
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
    return;
  }
  OpenClaimWindow(step, seat, tile, recipients, options);
}

void RiichiMahjongGame::OpenClaimWindow(GameStep& step, int from_seat, Tile tile,
                                        const std::array<bool, kSeatCount>& recipients,
                                        const std::array<std::vector<Option>, kSeatCount>& options, bool chankan) {
  claim_open_ = true;
  claim_chankan_ = chankan;
  claim_from_seat_ = from_seat;
  claim_tile_ = tile;
  claim_recipient_ = recipients;
  claim_options_ = options;
  claim_answers_ = {};
  claim_pending_humans_ = 0;
  ai_claim_pending_ = false;
  // PROTOCOL v2.2 §8.5: same deadline the frame carries, so a replay is exact.
  window_deadline_ms_ = clock::EpochMs() + action_timeout_ms_;
  phase_ = Phase::kClaimWindow;
  bool ai_pending = false;

  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (!recipients[static_cast<size_t>(seat)]) {
      continue;
    }
    if (!NeedsPlayerReply(seat)) {
      // GAME_DESIGN v1.4 §5.4: a bot's meld/self-draw choice is its own decision, so it is
      // executed ai_delay_ms after the discard (still without consuming the 15s deadline:
      // an all-AI window keeps its own pacing timer only). A human seat with no wire records
      // the frozen §5.6.2 default action immediately, exactly as the timeout message would.
      // A disconnected human is not here: it stays a pending recipient until the deadline.
      // A bot only takes a meld that does not worsen the shanten of its hand (t21/t23).
      if (PlaysWithAi(seat) && ai_delay_ms_ > 0) {
        ai_pending = true;
        continue;
      }
      Option choice;
      if (PlaysWithAi(seat)) {
        choice = ChooseClaimAnswer(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)],
                                   options[static_cast<size_t>(seat)], tile);
      } else if (const Option* pass = FindOption(options[static_cast<size_t>(seat)], protocol::Action::kPass);
                 pass != nullptr) {
        choice = *pass;
      } else {
        choice.action = protocol::Action::kPass;
      }
      ClaimAnswer answer;
      answer.answered = true;
      answer.action = choice.action;
      answer.tiles = choice.tiles;
      claim_answers_[static_cast<size_t>(seat)] = std::move(answer);
      continue;
    }
    ++claim_pending_humans_;
    if (SeatPresent(seat)) {
      Emit(step, seat,
           BuildActionRequestFrame(
               seq_, window_deadline_ms_, chankan ? protocol::Window::kChankan : protocol::Window::kClaim,
               options[static_cast<size_t>(seat)], chankan ? std::optional<Tile>(tile) : std::nullopt));
    }
  }
  if (claim_pending_humans_ > 0) {
    OpenWindowTimer(step);
  }
  if (ai_pending) {
    if (ScheduleAiAction(step, ai_delay_ms_)) {
      ai_claim_pending_ = true;
    } else {
      // No scheduler (never in production): decide at once instead of leaving the window
      // waiting for a paced answer that can never arrive.
      ResolveAutomatedClaimAnswers();
    }
  }
}

Option RiichiMahjongGame::ChooseClaimAnswer(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                            const std::vector<Option>& options, Tile claimed) {
  Option pass;
  if (const Option* declared = FindOption(options, protocol::Action::kPass); declared != nullptr) {
    pass = *declared;
  } else {
    pass.action = protocol::Action::kPass;
  }
  if (const Option* win = FindOption(options, protocol::Action::kWin); win != nullptr) {
    return *win;  // never leave a win on the table
  }

  const int before = Shanten(hand, melds);
  const Option* best = nullptr;
  int best_shanten = 0;

  // Pong is considered first, so it keeps a tie against any chow combination (the frozen
  // claim priority is win > kong > pong > chow > pass).
  if (const Option* pong = FindOption(options, protocol::Action::kPong); pong != nullptr) {
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
    if (option.action != protocol::Action::kChow) {
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

void RiichiMahjongGame::ResetClaimWindow() {
  claim_open_ = false;
  ai_claim_pending_ = false;
  claim_from_seat_ = -1;
  claim_recipient_ = {};
  claim_options_ = {};
  claim_answers_ = {};
  claim_pending_humans_ = 0;
  claim_chankan_ = false;
}

void RiichiMahjongGame::ResolveClaimWindow(GameStep& step) {
  int best_seat = -1;
  int best_priority = -1;
  int best_distance = kSeatCount;
  protocol::Action best_action = protocol::Action::kInvalid;
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

  // 见逃：窗口里给了 win 却没和（含超时未答）⇒ 振听。分两种（v1.22）：
  //   * 已立直 ⇒ 见逃振听**整局持久**（`furiten_ever_`），立直后不认「同巡」这种临时豁免；
  //   * 未立直 ⇒ **同巡振听**：只挡这一张的牌面，到自己下次摸牌为止（`temporary_furiten_`）。
  //     按牌面记是关键：放过 3m 只该挡 3m，不该连带挡住同一手的 6m。
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (!claim_recipient_[static_cast<size_t>(seat)]) {
      continue;
    }
    if (FindOption(claim_options_[static_cast<size_t>(seat)], protocol::Action::kWin) == nullptr) {
      continue;
    }
    if (claim_answers_[static_cast<size_t>(seat)].action != protocol::Action::kWin) {
      if (riichi_[static_cast<size_t>(seat)]) {
        furiten_ever_[static_cast<size_t>(seat)] = true;
      } else {
        temporary_furiten_[static_cast<size_t>(seat)][FaceOf(claim_tile_)] = true;
        spdlog::debug("room {}: seat {} enters temporary furiten on {}", room_id_, seat, FormatTile(claim_tile_));
      }
    }
  }

  const int from_seat = claim_from_seat_;
  const Tile tile = claim_tile_;
  const bool chankan = claim_chankan_;
  ResetClaimWindow();

  if (best_seat < 0) {
    if (chankan) {
      // 没人抢杠：加杠继续落地（补牌、翻宝牌、开打牌窗口都在 ApplySelfKan 里）。
      // 抢杠窗口自己的 seq 已经由开窗时的 Transition 拿到，这里不再转 —— ApplySelfKan 会转一次。
      ApplySelfKan(step, from_seat, protocol::Action::kKakan, std::vector<Tile> {tile});
      return;
    }
    // Everybody passed: the next player draws inside the new window.
    Transition();
    current_seat_ = (from_seat + 1) % kSeatCount;
    phase_ = Phase::kTurnStart;
    return;
  }
  if (best_action == protocol::Action::kWin) {
    if (chankan) {
      // 被抢杠：杠没成立，第 4 张从加杠者手里离开（随后 EndRoundWithWin 会把它放进和牌者手里）。
      RemoveOneTile(&hands_[static_cast<size_t>(from_seat)], tile);
      EndRoundWithWin(step, best_seat, from_seat, tile, /*tsumo=*/false, /*is_chankan=*/true);
      return;
    }
    EndRoundWithWin(step, best_seat, from_seat, tile, /*tsumo=*/false);
    return;
  }
  ApplyMeld(step, best_seat, best_action, best_tiles, from_seat, tile);
}

void RiichiMahjongGame::ApplyMeld(GameStep& step, int seat, protocol::Action action,
                                  const std::vector<Tile>& client_tiles, int from_seat, Tile claimed_tile) {
  const Face claimed_face = FaceOf(claimed_tile);
  Meld meld;
  meld.type = MeldTypeForAction(action);
  std::vector<Tile> from_hand;
  if (action == protocol::Action::kPong) {
    from_hand = TakeFaces(&hands_[static_cast<size_t>(seat)], claimed_face, 2);
  } else if (action == protocol::Action::kKong) {
    from_hand = TakeFaces(&hands_[static_cast<size_t>(seat)], claimed_face, 3);
  } else {  // chow：客户端提交的两张就是要离开手牌的那两张（赤 5 走 id 精确匹配）
    for (Tile tile : client_tiles) {
      const std::optional<Tile> removed = RemoveOneTile(&hands_[static_cast<size_t>(seat)], tile);
      from_hand.push_back(removed.value_or(tile));
    }
  }
  meld.tiles = from_hand;
  meld.tiles.push_back(claimed_tile);
  SortTiles(meld.tiles);
  melds_[static_cast<size_t>(seat)].push_back(std::move(meld));
  // 喰い替え禁止（v2.5）：吃/碰之后当巡不能打「与该组能组成面子的牌面」。
  // 计算用的是**真正离手的那几张**（`from_hand`，赤 5 已按物理牌解析），但只按牌面记 ——
  // 赤 5 与普通 5 同面，禁一张就是禁两面。
  kuikae_faces_[static_cast<size_t>(seat)] = riichi::KuikaeForbiddenFaces(from_hand, claimed_tile);
  ippatsu_ = {};  // 任何鸣牌（吃/碰/明杠）都中断全场的一发
  // 同巡振听解除（v1.22）：放过和牌张之后**自己鸣了牌**，本巡就已经翻页 —— 放过的牌面重新
  // 可和。与「下次摸牌解除」同属「这一巡结束」的两种入口（吃碰杠后必然要打一张，见下）。
  temporary_furiten_[static_cast<size_t>(seat)].fill(false);
  // FEEL_SPEC F-SORT-01b 触发点 ④：**每次鸣牌扣牌后**重排 `hands_[seat]`（pong/chow/kong 同源）。
  // 扣牌本身保序，此调用按规格显式声明该不变量，避免将来改动扣牌实现时静默破序。
  // 只排手牌：`discards_`（牌河顺序是信息）与 `melds_[].tiles`（组内序由 §4.8 冻结）一律不排。
  SortTiles(hands_[static_cast<size_t>(seat)]);
  if (from_seat >= 0 && !discards_[static_cast<size_t>(from_seat)].empty()) {
    discards_[static_cast<size_t>(from_seat)].pop_back();  // GAME_DESIGN §4.2 step 10
  }

  current_seat_ = seat;
  const uint64_t old_seq = seq_;
  Transition();  // close the claim window and open the melder's discard window (§4.3)
  phase_ = Phase::kDiscardWindow;

  // Broadcast to all four seats (§5.8) with no seat-specific field, so one build serves them all.
  EmitAll(step, BuildActionEventFrame(seat, action, client_tiles, from_seat, old_seq));
  spdlog::info("room {}: seat {} {} {} from seat {} (seq {} -> {})", room_id_, seat, protocol::NameOf(action),
               FormatTile(claimed_tile), from_seat, old_seq, seq_);

  std::optional<Tile> replacement;
  if (action == protocol::Action::kKong) {
    if (remaining_ <= 0) {
      // No tile left for the replacement draw: the round ends (GAME_DESIGN §4.2 step 12).
      EndRoundWithDraw(step);
      return;
    }
    const Tile drawn = DrawReplacementTile();
    FlipDoraIndicator();  // 每杠翻一张新的宝牌指示牌
    hands_[static_cast<size_t>(seat)].push_back(drawn);
    SortTiles(hands_[static_cast<size_t>(seat)]);
    replacement = drawn;
    kong_draw_tile_ = drawn;  // 杠上开花记录：本窗口内自摸且和牌张 == 它 ⇒ rinshan（B1 接线）
    if (seats_[static_cast<size_t>(seat)].wired) {
      Emit(step, seat, BuildDrawFrame(drawn, seat, seq_));
    }
  }
  EmitPerSeat(step, StateSyncFrames());
  // v1.7 §5.16 T2：本家副露（含杠补牌）生效后重算听牌——此刻是 14 逻辑张（待打牌），
  // 因此实际下发的是「清空」信号（is_tenpai=false / waits: []），T1 会在打牌后补上真值。
  DeliverTenpai(step, seat);
  OpenDiscardWindow(step, /*win_check=*/action == protocol::Action::kKong, replacement);
}

// v1.6 自理杠（ankan / kakan，PROTOCOL §4.6）：仅本家打牌窗口可触发（ValidateAction 已重算合法性）。
// 张数公式逐字不变：一个杠在 melds 里仍是 1 组、账面 3 张，第 4 张由 kans 单独补回 ——
// 三个杠的差别只在「第 4 张从哪来」，本函数只负责把它放进去并补摸 1 张。
void RiichiMahjongGame::ApplySelfKan(GameStep& step, int seat, protocol::Action action,
                                     const std::vector<Tile>& client_tiles) {
  const Face face = FaceOf(client_tiles.front());
  std::optional<KongResult> result;
  if (action == protocol::Action::kAnkan) {
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
    // ValidateAction 已按 DiscardOptions 的窗口选项重算过合法性，走到这里说明选项列表与规则
    // 库不一致；不改状态、不下发，留一条错误日志定位。
    spdlog::error("room {}: seat {} {} is no longer legal", room_id_, seat, protocol::NameOf(action));
    return;
  }
  hands_[static_cast<size_t>(seat)] = std::move(result->hand);
  melds_[static_cast<size_t>(seat)] = std::move(result->melds);
  ippatsu_ = {};  // 暗杠/加杠同样中断一发

  const uint64_t old_seq = seq_;
  Transition();  // close the discard window; the replacement-draw window takes the new seq
  phase_ = Phase::kDiscardWindow;

  // action_event（PROTOCOL §5.8 v1.6）：ankan/kakan **不带 from_seat**、**不动牌河**。
  // PROTOCOL v1.7 §11.5 修正：`ankan` 的 `tiles` **恒为 []（对所有收件人，含暗杠者本人）**——
  // action_event 是广播帧，v1.6 带 [T] 等于把暗杠牌面告诉三家，绕过 state_sync 的打码；
  // 本家牌面不受损（紧随其后的 state_sync.melds[] 带 4 张真实牌面）。`kakan` 不变（公开动作）。
  const std::vector<Tile> event_tiles = action == protocol::Action::kAnkan ? std::vector<Tile> {} : client_tiles;
  EmitAll(step, BuildActionEventFrame(seat, action, event_tiles, /*from_seat=*/-1, old_seq));
  spdlog::info("room {}: seat {} {} (seq {} -> {})", room_id_, seat, protocol::NameOf(action), old_seq, seq_);

  // 立即补摸 1 张（无抢杠窗口，§0.4）；remaining 只能由 kTileCount - next_index_ 派生。
  if (remaining_ <= 0) {
    EndRoundWithDraw(step);
    return;
  }
  const Tile drawn = DrawReplacementTile();
  FlipDoraIndicator();
  hands_[static_cast<size_t>(seat)].push_back(drawn);
  SortTiles(hands_[static_cast<size_t>(seat)]);
  kong_draw_tile_ = drawn;  // 杠上开花记录
  if (seats_[static_cast<size_t>(seat)].wired) {
    Emit(step, seat, BuildDrawFrame(drawn, seat, seq_));
  }

  // 自理杠之后必定下发 state_sync（§5.12/§5.8 注 4：他家以此权威覆盖 melds[]）。
  EmitPerSeat(step, StateSyncFrames());
  // v1.7 §5.16 T2：自理杠（暗杠/加杠）同属副露——本家听牌重算（14 逻辑张 ⇒ 清空信号）。
  DeliverTenpai(step, seat);
  OpenDiscardWindow(step, /*win_check=*/true, drawn);
}

void RiichiMahjongGame::EndRoundWithWin(GameStep& step, int winner_seat, int from_seat, Tile winning_tile, bool tsumo,
                                        bool is_chankan) {
  const uint64_t old_seq = seq_;
  if (!tsumo) {
    // Ron: the winning tile completes the winner's hand (it stays visible in the river).
    hands_[static_cast<size_t>(winner_seat)].push_back(winning_tile);
    SortTiles(hands_[static_cast<size_t>(winner_seat)]);
  }
  const std::vector<Tile>& concealed = hands_[static_cast<size_t>(winner_seat)];
  // 算分走 riichi::ScoreHand（符 + 番 + 点数表）：只接收纯函数输入，房间不做任何数值换算。
  const riichi::WinContext ctx = BuildRiichiContext(winner_seat, from_seat, tsumo, winning_tile, is_chankan);
  const riichi::ScoreResult score =
      riichi::ScoreHand(concealed, melds_[static_cast<size_t>(winner_seat)], winning_tile, ctx);
  const bool was_riichi = ctx.is_riichi;

  Transition();  // close the window; no new window follows
  phase_ = Phase::kRoundEnd;
  claim_open_ = false;
  discard_window_open_ = false;
  discard_window_win_check_ = false;
  drawn_tile_.reset();
  kong_draw_tile_.reset();

  Settlement settlement;
  settlement.reason = tsumo ? protocol::RoundEndReason::kWinTsumo : protocol::RoundEndReason::kWinRon;
  settlement.winner_seat = winner_seat;
  settlement.winner_flat_hand = concealed;
  for (const Meld& meld : melds_[static_cast<size_t>(winner_seat)]) {
    settlement.winner_flat_hand.insert(settlement.winner_flat_hand.end(), meld.tiles.begin(), meld.tiles.end());
  }
  SortTiles(settlement.winner_flat_hand);
  // v4.5：结算摊牌数据（四家）——只写进 game_end，state_sync 仍不泄露。
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (seat == winner_seat) {
      settlement.flat_hands[static_cast<size_t>(seat)] = settlement.winner_flat_hand;
      continue;
    }
    std::vector<Tile> flat = hands_[static_cast<size_t>(seat)];
    for (const Meld& meld : melds_[static_cast<size_t>(seat)]) {
      flat.insert(flat.end(), meld.tiles.begin(), meld.tiles.end());
    }
    SortTiles(flat);
    settlement.flat_hands[static_cast<size_t>(seat)] = std::move(flat);
  }
  settlement.delta = score.delta;
  // 供托：立直棒全部归和牌者。宣言时已从各家持点扣除，所以「四家 delta 之和为 0」仍成立。
  if (kyoutaku_ > 0) {
    settlement.delta[static_cast<size_t>(winner_seat)] += kyoutaku_ * riichi::kRiichiStickPoints;
  }
  settlement.kyoutaku = kyoutaku_;
  kyoutaku_ = 0;
  settlement.points = score.points;
  settlement.fu = score.fu;
  settlement.han = score.han;
  settlement.limit = score.limit;
  settlement.riichi = was_riichi;
  settlement.dora = score.dora;
  settlement.ura_dora = score.ura_dora;
  settlement.aka_dora = score.aka;
  settlement.dora_indicators = dora_indicators_;
  settlement.dealer_bonus = winner_seat == dealer_ ? 1 : 0;
  settlement.rinshan = std::any_of(score.yaku.begin(), score.yaku.end(),
                                   [](const riichi::Yaku& item) { return item.id == protocol::YakuId::kRinshan; });
  settlement.yaku.clear();
  for (const riichi::Yaku& item : score.yaku) {
    settlement.yaku.push_back(Yaku {.id = item.id, .han = item.han});
  }
  settlement.multiplier = score.points;  // 客户端旧字段：日麻下用点数填充
  settlement.per_payer = score.per_payer;
  settlement.total = score.points;
  for (const Yaku& yaku : settlement.yaku) {
    settlement.tags.push_back(yaku.id);
  }
  settlement.winner_types = settlement.tags;
  settlement.winner_types.emplace_back(tsumo ? protocol::YakuId::kWinTsumo : protocol::YakuId::kWinRon);
  settlement.tsumo = tsumo;
  settlement.winning_tile = winning_tile;
  settlement.from_seat = tsumo ? -1 : from_seat;
  // v1.6（C8）场次字段
  settlement.round_index = round_index_;
  settlement.dealer = dealer_;
  // ⚠️ 这里**故意不写** `settlement.honba`：拆分前的 room.cc 只在荒庄路径赋过它
  // （`EndRoundWithDraw`），和牌路径一直是 0。本场数**已经算进 delta/点数**（ctx.honba 用真值），
  // 所以这只是 `win.honba` 这个展示字段的老 bug。先把线上字节保住（golden 冻结值就是这么来的），
  // 要修它是一次**有意的**协议变更：改这一行 + 重冻三条 golden + 同步 PROTOCOL.md。
  // 持点：供托/本场已计入 delta，因此这里一次性落账
  ApplyDeltaToPoints(settlement.delta);
  settlement.points_after = points_;

  EmitAll(step,
          BuildActionEventFrame(winner_seat, protocol::Action::kWin, {winning_tile}, tsumo ? -1 : from_seat, old_seq));
  // PROTOCOL.md §5.12 + §5.9 v1.6：game_end 前的最后一份 state_sync 必须**揭示全部暗杠**。
  EmitPerSeat(step, StateSyncFramesRevealed());
  AdvanceAfterRound(step, settlement);
}

void RiichiMahjongGame::EndRoundWithDraw(GameStep& step) {
  Transition();
  phase_ = Phase::kRoundEnd;
  claim_open_ = false;
  discard_window_open_ = false;
  discard_window_win_check_ = false;
  drawn_tile_.reset();
  kong_draw_tile_.reset();

  Settlement settlement;
  settlement.reason = protocol::RoundEndReason::kDrawExhausted;
  settlement.winner_seat = -1;
  // 荒庄时四家都是 13 张等待态：逐家判「形式听牌」，再按 3000 点平摊收付。
  for (int seat = 0; seat < kSeatCount; ++seat) {
    settlement.tenpai[static_cast<size_t>(seat)] =
        !riichi::RiichiWaitingTiles(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)]).empty();
  }
  draw_dealer_tenpai_ = settlement.tenpai[static_cast<size_t>(dealer_)];
  settlement.delta = riichi::DrawPenalty(settlement.tenpai);
  // v4.5：荒庄也摊四家手牌（只随 game_end 下发）。
  for (int seat = 0; seat < kSeatCount; ++seat) {
    std::vector<Tile> flat = hands_[static_cast<size_t>(seat)];
    for (const Meld& meld : melds_[static_cast<size_t>(seat)]) {
      flat.insert(flat.end(), meld.tiles.begin(), meld.tiles.end());
    }
    SortTiles(flat);
    settlement.flat_hands[static_cast<size_t>(seat)] = std::move(flat);
  }
  // v1.6（C8）场次字段
  settlement.round_index = round_index_;
  settlement.dealer = dealer_;
  settlement.honba = honba_;
  // delta 已含听牌罚符
  ApplyDeltaToPoints(settlement.delta);
  settlement.points_after = points_;

  // PROTOCOL.md §5.12 + §5.9 v1.6：荒庄前最后一份 state_sync 同样揭示全部暗杠。
  EmitPerSeat(step, StateSyncFramesRevealed());
  AdvanceAfterRound(step, settlement);
}

// v1.6（C7 局终 → 场终的推进）：荒庄也无条件累加（+0）、逐局上报（C10）、
// NextDealer + ClassifyRound 判定连庄/轮庄（荒庄一律连庄）、C15 双闸、场终 match_end、
// 否则 kRoundEnd + 武装 C6 推进令牌（C5：本函数返回，推进由 deadline 触发）。
void RiichiMahjongGame::AdvanceAfterRound(GameStep& step, Settlement& settlement) {
  // C7：荒庄也 +0 无条件累加；match_total 含本局。B3：坐庄手数每局 +1（含连庄的额外手）。
  for (int seat = 0; seat < kSeatCount; ++seat) {
    match_totals_[static_cast<size_t>(seat)] += settlement.delta[static_cast<size_t>(seat)];
  }
  settlement.totals = match_totals_;
  settlement.points_after = points_;
  ++dealer_hands_[static_cast<size_t>(settlement.dealer)];
  ++hands_played_;

  // v1.19（雀魂口径）：任一持点 **< 0** ⇒ 击飞，本场立即终局，优先级高于连庄与局数
  // （庄家本来能连庄，也不再开下一局）。恰好 0 点不算击飞；负点不截断，照实进排名。
  const bool busted = riichi::IsBusted(points_);

  // 连庄/轮庄判定（§3.3）：荒庄按「庄家是否听牌」决定 —— 没听就轮庄。
  // 击飞时**不推进**庄家与局号：本场到此为止，场面数字停在被打穿的那一局。
  if (!busted) {
    RoundResult result = ClassifyRound(settlement.reason, settlement.winner_seat, settlement.dealer);
    if (result == RoundResult::kDrawExhausted && !draw_dealer_tenpai_) {
      result = RoundResult::kNonDealerWin;  // 庄家未听牌 ⇒ 轮庄（本场随之下庄清零）
    }
    if (result == RoundResult::kNonDealerWin) {
      dealer_ = NextDealer(dealer_, result);
      ++round_index_;
      honba_ = 0;  // 闲家和牌 ⇒ 本场清零
    } else {
      ++renchan_count_;  // 庄家和牌或荒庄 ⇒ 连庄（round_index 不动）
      ++honba_;          // 连庄每本 +300 点（支付在下一局的算分里）
    }
  }
  // B2 决策 D1：game_end.renchan_count 报**含本局触发**的累计值（post-increment）——
  // 这与 match_end.renchan_count「与最后一次 game_end 同值」的冻结约束一致（PROTOCOL §5.15）。

  // 双闸：round_index 走满（东场 4 / 半庄 8），或连庄次数超过上限，本场结束。
  // 连庄上限放宽到「局数」：真实规则里连庄不封顶，用 4 会把半庄在第 5 局就砍断。
  const bool match_over = busted || round_index_ >= round_count_ || renchan_count_ > round_count_;
  settlement.match_will_continue = !match_over;
  settlement.renchan_count = renchan_count_;
  if (busted) {
    spdlog::info("room {}: match over by bust (round_index={}, points={},{},{},{})", room_id_, settlement.round_index,
                 points_[0], points_[1], points_[2], points_[3]);
  }

  // v1.20（雀魂口径）：**场终时台面上的立直棒归一位**。
  // 立直宣言的 1000 点当时就从持点里扣走了（记在 delta 里），棒挂在 kyoutaku_ 上等下一次和牌；
  // 若本场以荒庄或击飞收尾，就没有和牌者来收，棒会凭空消失（四家持点之和不再等于 100000）。
  // 归账要动四处：points_（终局持点）、match_totals_（本场累计得失分）、本局 delta / totals，
  // 再重算 points_after —— 最后一局的 game_end 必须已经含这笔，否则它和 match_end.rankings
  // 的 final_score 会打架（客户端按前者播报、按后者排名）。
  if (match_over && kyoutaku_ > 0) {
    const riichi::KyoutakuAward award = riichi::AwardLeftoverKyoutaku(points_, kyoutaku_);
    if (award.seat >= 0) {
      const size_t seat = static_cast<size_t>(award.seat);
      points_[seat] += award.points;
      match_totals_[seat] += award.points;
      settlement.delta[seat] += award.points;
      settlement.totals[seat] += award.points;
      settlement.points_after = points_;
      settlement.kyoutaku_award = award.sticks;
      settlement.kyoutaku_award_seat = award.seat;
      kyoutaku_ = 0;
      spdlog::info("room {}: match over, {} leftover riichi stick(s) awarded to seat {} (+{} points)", room_id_,
                   award.sticks, award.seat, award.points);
    }
  }
  if (match_over) {
    // B3 场终快照：与 match_end 同源（hands_played_ / dealer_hands_ / ComputeFinalRanks）。
    settlement.hands_played = hands_played_;
    settlement.dealer_hands = dealer_hands_;
    settlement.final_rank = ComputeFinalRanks();
  }

  BroadcastSettlement(step, settlement);  // game_end（局终帧，一场 4..8 次）
  spdlog::info("room {}: round over (reason={}, round_index={}, dealer={}, winner={}, points={}, han={}, seq {})",
               room_id_, protocol::NameOf(settlement.reason), settlement.round_index, settlement.dealer,
               settlement.winner_seat, settlement.points, settlement.han, seq_);

  // C10：逐局上报；房间只在 match_over 时把它当作回收信号。
  GameResult result;
  result.reason = settlement.reason;
  result.winner_seat = settlement.winner_seat;
  result.delta = settlement.delta;
  result.match_over = !settlement.match_will_continue;
  result.hands_played = settlement.hands_played;
  result.dealer_hands = settlement.dealer_hands;
  result.totals = settlement.totals;
  result.final_rank = settlement.final_rank;
  step.results.push_back(result);

  if (!match_over) {
    phase_ = Phase::kRoundEnd;
    // C6：推进令牌必须在 Transition() **之后** arm —— 令牌携带局终后的新 seq，
    // 上一局最后窗口的 timeout（旧 seq、无标签）被第一道判据丢弃，两者永不相交。
    Transition();
    round_advance_pending_ = true;
    if (!deadlines_supported_) {
      // 与 AI pacing 同款的回退（t31 的"永不等待不会到来的回复"不变式）：
      // 无调度器时立即推进，而不是停在一局终等一条永远不会来的消息。
      spdlog::error("room {}: no timeout scheduler installed for the round advance; advancing inline", room_id_);
      StartRound(step);
      return;
    }
    Arm(step, inter_hand_delay_ms_, MakeRoundAdvanceToken(seq_));
    return;
  }
  // v1.8（SETTLEMENT_BROADCAST_RULINGS §2.1 / C8）：场终也走**同一段播报窗**——match_end 不许
  // 抢在结算播报前面到（客户端不缓存权威帧，只能靠服务端延迟）。**禁止 sleep_for / 阻塞**：
  // 复用同一个一次性 deadline + 独立令牌位（kMatchEndTokenBit），与局间推进同款
  // 「arm 在 Transition() 之后」。`kRoundEnd` 是两条延迟共用的相位，靠 pending 位区分。
  if (inter_hand_delay_ms_ > 0 && deadlines_supported_) {
    phase_ = Phase::kRoundEnd;
    Transition();
    match_end_pending_ = true;
    Arm(step, inter_hand_delay_ms_, MakeMatchEndToken(seq_));
    return;
  }
  if (inter_hand_delay_ms_ > 0) {
    // 无调度器时不能停在一局终等一条永远不会来的消息（t31 的同款回退）：立即场终。
    spdlog::error("room {}: no timeout scheduler for the match-end delay; finishing inline", room_id_);
  }
  FinishMatch(step);
}

void RiichiMahjongGame::FinishMatch(GameStep& step) {
  // v1.6（C7 场终）：match_end 广播 + kMatchEnd；之后只剩 §1.4.5 的 30s 回收（C10）。
  phase_ = Phase::kMatchEnd;
  round_advance_pending_ = false;
  match_end_pending_ = false;
  EmitAll(step, BuildMatchEnd());
  spdlog::info("room {}: match over (rounds={}, renchan={}, hands_played={})", room_id_, round_index_, renchan_count_,
               hands_played_);
}

// match_end.rankings 的 rank（并列跳号）；D2/B3：场终快照与 match_end 必须同源，故独立成函数。
std::array<int, kSeatCount> RiichiMahjongGame::ComputeFinalRanks() const {
  // 日麻按终局持点（25000 起，不可与零和得失分混排）
  std::array<int, kSeatCount> ranks {};
  for (int seat = 0; seat < kSeatCount; ++seat) {
    int rank = 1;
    for (int other = 0; other < kSeatCount; ++other) {
      if (points_[static_cast<size_t>(other)] > points_[static_cast<size_t>(seat)]) ++rank;
    }
    ranks[static_cast<size_t>(seat)] = rank;
  }
  return ranks;
}

// ---------------------------------------------------------------------------------------
// Rule options: the riichi rules library plus the state it cannot see on its own —
// riichi declarations, furiten, and whether the hand has any yaku at all.
// ---------------------------------------------------------------------------------------

std::vector<Option> RiichiMahjongGame::DiscardOptionsFor(int seat, std::vector<Tile>* riichi_tiles) const {
  const std::vector<Tile>& hand = hands_[static_cast<size_t>(seat)];
  const std::vector<Meld>& melds = melds_[static_cast<size_t>(seat)];
  // 立直后只放行「不改变听牌」的暗杠：判据要刚摸到的那张，只有当前行动座位才有。
  const std::optional<Tile> drawn = seat == current_seat_ ? drawn_tile_ : std::nullopt;
  std::vector<Option> options = riichi::DiscardOptions(hand, melds, remaining_, RiichiDeclared(seat), drawn);
  const Tile winning = TsumoWinningTile(seat);
  if (FindOption(options, protocol::Action::kWin) != nullptr && !HasYakuForWin(seat, winning, /*tsumo=*/true, -1)) {
    // 日麻无役不能和：把 win 从窗口里摘掉（客户端因此不会看到「胡」按钮）
    options.erase(std::remove_if(options.begin(), options.end(),
                                 [](const Option& option) { return option.action == protocol::Action::kWin; }),
                  options.end());
  }
  if (riichi_tiles != nullptr) {
    // v1.21：立直选项与「宣言牌候选」必须同时成立 —— 候选为空（振听 / 打了都不听）
    // 就把选项一并摘掉，别让玩家点开一个注定被拒的按钮。
    std::vector<Tile> candidates = RiichiDiscardTilesFor(seat);
    const bool offer = FindOption(options, protocol::Action::kRiichi) != nullptr && !candidates.empty();
    if (!offer) {
      options.erase(std::remove_if(options.begin(), options.end(),
                                   [](const Option& option) { return option.action == protocol::Action::kRiichi; }),
                    options.end());
    }
    *riichi_tiles = offer ? std::move(candidates) : std::vector<Tile> {};
  }
  return options;
}

std::vector<Option> RiichiMahjongGame::ClaimOptionsFor(int seat, Tile tile, bool is_next_player, int from_seat) const {  // 振听按**这张牌**判（`IsFuritenFor`），不是整手：同巡振听只挡住放过的那个牌面。
  std::vector<Option> options =
      riichi::ClaimOptions(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)], tile, remaining_,
                           is_next_player, IsFuritenFor(seat, tile));
  if (RiichiDeclared(seat)) {
    // 立直后手牌锁定：吃/碰/杠一律不可（只保留 win / pass）
    options.erase(std::remove_if(options.begin(), options.end(),
                                 [](const Option& option) {
                                   return option.action != protocol::Action::kWin &&
                                          option.action != protocol::Action::kPass;
                                 }),
                  options.end());
  }
  if (FindOption(options, protocol::Action::kWin) != nullptr &&
      !HasYakuForWin(seat, tile, /*tsumo=*/false, from_seat)) {
    options.erase(std::remove_if(options.begin(), options.end(),
                                 [](const Option& option) { return option.action == protocol::Action::kWin; }),
                  options.end());
  }
  return options;
}

// ---------------------------------------------------------------------------------------
// 喰い替え禁止（v2.5，PROTOCOL §12.15）
// ---------------------------------------------------------------------------------------

// 生效口径：`kuikae_faces_` 与手牌取交集后再判「会不会把窗口卡死」——
// 手里每一张都被禁时必须放开（否则那个打牌窗口永远等不到合法打牌，只能靠超时打出一张
// 非法牌，规则库反过来又拒它）。这是日本网络麻将的通行处理（天凤：整手都被禁时不给鸣）。
std::vector<Face> RiichiMahjongGame::KuikaeFacesFor(int seat) const {
  const std::vector<Face>& banned = kuikae_faces_[static_cast<size_t>(seat)];
  if (banned.empty()) {
    return {};
  }
  const std::vector<Tile>& hand = hands_[static_cast<size_t>(seat)];
  bool any_allowed = false;
  for (Tile tile : hand) {
    if (std::find(banned.begin(), banned.end(), FaceOf(tile)) == banned.end()) {
      any_allowed = true;
      break;
    }
  }
  if (!any_allowed) {
    return {};  // 整手都被禁 ⇒ 这条规则在此局面下无法执行，放开
  }
  std::vector<Face> faces;
  for (Face face : banned) {
    if (HandHasFace(hand, face)) {
      faces.push_back(face);
    }
  }
  std::sort(faces.begin(), faces.end());
  return faces;
}

bool RiichiMahjongGame::IsKuikaeForbidden(int seat, Tile tile) const {
  const std::vector<Face> faces = KuikaeFacesFor(seat);
  return std::find(faces.begin(), faces.end(), FaceOf(tile)) != faces.end();
}

// ---------------------------------------------------------------------------------------
// 立直 / 振听 / 役 / 王牌
// ---------------------------------------------------------------------------------------

std::vector<Tile> RiichiMahjongGame::RiichiDiscardTilesFor(int seat) const {
  std::vector<Tile> candidates;
  if (RiichiDeclared(seat)) {
    return candidates;
  }
  const std::vector<Tile>& hand = hands_[static_cast<size_t>(seat)];
  const std::vector<Meld>& melds = melds_[static_cast<size_t>(seat)];
  // 与提交时的 `CanDeclareRiichiNow` 同口径：门前、点数、牌山、至少一张能听的打出牌。
  if (!riichi::CanDeclareRiichi(hand, melds, remaining_)) {
    return candidates;
  }
  if (points_[static_cast<size_t>(seat)] < riichi::kRiichiStickPoints) {
    return candidates;
  }
  if (furiten_ever_[static_cast<size_t>(seat)]) {
    return candidates;  // 立直后见逃：本局内不许再立直（与 CanDeclareRiichiNow 一致）
  }
  // 同巡振听同样不许立直（PROTOCOL §12.4「未处于振听」）。这里用整手口径：
  // 只要本巡放过了任何一个等待张的牌面，这一手就处于振听。
  const std::array<bool, kFaceCount>& temporary = temporary_furiten_[static_cast<size_t>(seat)];
  const std::vector<Tile> current_waits = riichi::RiichiWaitingTiles(hand, melds);
  for (const Tile& wait : current_waits) {
    if (temporary[FaceOf(wait)]) {
      return candidates;
    }
  }
  for (Face face : riichi::RiichiDiscardFaces(hand, melds)) {
    std::vector<Tile> rest = hand;
    RemoveOneFace(&rest, face);
    if (riichi::IsPermanentFuriten(rest, melds, discards_[static_cast<size_t>(seat)])) {
      continue;  // 这张打出去正好落舍て牌振听 ⇒ 不是合法宣言牌，不给前端标记
    }
    candidates.push_back(MakeTileFromFace(face));
  }
  return candidates;
}

bool RiichiMahjongGame::HasYakuForWin(int seat, Tile winning_tile, bool tsumo, int from_seat, bool is_chankan) const {
  std::vector<Tile> hand = hands_[static_cast<size_t>(seat)];
  if (!tsumo) {
    hand.push_back(winning_tile);  // 荣和：和牌张还在对方牌河里
  }
  SortTiles(hand);
  const riichi::WinContext ctx = BuildRiichiContext(seat, from_seat, tsumo, winning_tile, is_chankan);
  return !riichi::DetectYaku(hand, melds_[static_cast<size_t>(seat)], winning_tile, ctx).empty();
}

bool RiichiMahjongGame::IsWinShape(const std::vector<Tile>& concealed, const std::vector<Meld>& melds,
                                   Tile winning_tile, bool tsumo) const {
  return riichi::IsRiichiWinningHand(concealed, melds, winning_tile, tsumo);
}

bool RiichiMahjongGame::IsFuriten(int seat) const {
  if (furiten_ever_[static_cast<size_t>(seat)]) {
    return true;  // 立直后见逃：本局内不可再荣和
  }
  if (HasTemporaryFuriten(seat)) {
    return true;  // 同巡振听
  }
  return riichi::IsPermanentFuriten(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)],
                                    discards_[static_cast<size_t>(seat)]);
}

bool RiichiMahjongGame::HasTemporaryFuriten(int seat) const {
  const std::array<bool, kFaceCount>& temporary = temporary_furiten_[static_cast<size_t>(seat)];
  return std::any_of(temporary.begin(), temporary.end(), [](bool flag) { return flag; });
}

bool RiichiMahjongGame::IsFuritenFor(int seat, Tile tile) const {
  if (furiten_ever_[static_cast<size_t>(seat)]) {
    return true;  // 立直后见逃：整局都不能再荣和（任何牌面）
  }
  if (temporary_furiten_[static_cast<size_t>(seat)][FaceOf(tile)]) {
    return true;  // 同巡振听：这一巡放过这个牌面，下一张同面也不能和
  }
  return riichi::IsPermanentFuriten(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)],
                                    discards_[static_cast<size_t>(seat)]);
}

bool RiichiMahjongGame::CanDeclareRiichiNow(int seat, Tile discard) const {
  if (RiichiDeclared(seat)) {
    return false;
  }
  const std::vector<Tile>& hand = hands_[static_cast<size_t>(seat)];
  const std::vector<Meld>& melds = melds_[static_cast<size_t>(seat)];
  if (!riichi::IsClosedHand(melds)) {
    return false;
  }
  if (points_[static_cast<size_t>(seat)] < riichi::kRiichiStickPoints) {
    return false;
  }
  if (remaining_ < riichi::kMinRiichiWall) {
    return false;
  }
  if (!HandHasFace(hand, FaceOf(discard))) {
    return false;
  }
  // 打出这张之后必须处于听牌（用门前手牌重算，不信任客户端）
  std::vector<Tile> rest = hand;
  RemoveOneFace(&rest, FaceOf(discard));
  if (riichi::RiichiWaitingTiles(rest, melds).empty()) {
    return false;
  }
  // 振听状态不能宣言立直（舍て牌振听 / 见逃振听 / 同巡振听同罪）。判定要用**打完这张之后的
  // 13 张**：此刻手里是 14 张，`WaitingTiles` 口径不成立，直接算会把「听牌」判成「没听」。
  if (furiten_ever_[static_cast<size_t>(seat)]) {
    return false;
  }
  if (riichi::IsPermanentFuriten(rest, melds, discards_[static_cast<size_t>(seat)])) {
    return false;
  }
  // 同巡振听：本巡放过的那张若正是打完后的等待张，这一手就是振听，不许立直。
  const std::array<bool, kFaceCount>& temporary = temporary_furiten_[static_cast<size_t>(seat)];
  for (const Tile& wait : riichi::RiichiWaitingTiles(rest, melds)) {
    if (temporary[FaceOf(wait)]) {
      return false;
    }
  }
  return true;
}

void RiichiMahjongGame::DeclareRiichi(GameStep& step, int seat) {
  riichi_[static_cast<size_t>(seat)] = true;
  ippatsu_[static_cast<size_t>(seat)] = true;        // 一发窗口：下一次摸牌前有效，鸣牌即清
  ippatsu_armed_[static_cast<size_t>(seat)] = true;  // 本次打牌不算「跨过下一摸」
  points_[static_cast<size_t>(seat)] -= riichi::kRiichiStickPoints;
  kyoutaku_ += 1;
  spdlog::info("room {}: seat {} declares riichi (kyoutaku={})", room_id_, seat, kyoutaku_);
  // 广播「发生了立直」：立直棒与横置是公开信息，前端据此起动画；权威状态在
  // state_sync.seats[].riichi（紧随其后的打牌广播会带上）。tiles 恒空、不带 from_seat。
  EmitAll(step, BuildActionEventFrame(seat, protocol::Action::kRiichi, {}, /*from_seat=*/-1, seq_));
}

bool RiichiMahjongGame::BotShouldRiichi(int seat) const {
  if (RiichiDeclared(seat)) {
    return false;
  }
  if (!riichi::CanDeclareRiichi(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)], remaining_)) {
    return false;
  }
  return points_[static_cast<size_t>(seat)] >= riichi::kRiichiStickPoints;
}

riichi::WinContext RiichiMahjongGame::BuildRiichiContext(int winner_seat, int from_seat, bool tsumo, Tile winning_tile,
                                                         bool is_chankan) const {
  riichi::WinContext ctx;
  ctx.winner_seat = winner_seat;
  ctx.from_seat = tsumo ? -1 : from_seat;
  ctx.dealer_seat = dealer_;
  ctx.honba = honba_;
  ctx.round_wind = round_wind_;
  ctx.seat_wind = riichi::SeatWindOf(round_wind_, winner_seat, dealer_);
  ctx.is_riichi = riichi_[static_cast<size_t>(winner_seat)];
  ctx.is_ippatsu = ippatsu_[static_cast<size_t>(winner_seat)];
  ctx.is_rinshan = tsumo && kong_draw_tile_.has_value() && FaceOf(*kong_draw_tile_) == FaceOf(winning_tile);
  ctx.is_chankan = is_chankan;
  // 天和 / 地和（v1.17）：第一巡自摸。用「牌河/副露是否为空」判定，不另维护回合计数 ——
  // 一个座位打牌一定早于它的下一次摸牌，所以「本家牌河为空」就等价于「这仍是它的第一巡」。
  if (tsumo) {
    const bool no_discards =
        std::all_of(discards_.begin(), discards_.end(), [](const std::vector<Tile>& river) { return river.empty(); });
    const bool no_melds =
        std::all_of(melds_.begin(), melds_.end(), [](const std::vector<Meld>& list) { return list.empty(); });
    if (winner_seat == dealer_) {
      ctx.is_tenhou = no_discards && no_melds;
    } else {
      ctx.is_chiihou = no_melds && discards_[static_cast<size_t>(winner_seat)].empty();
    }
  }
  ctx.is_haitei = tsumo && remaining_ == 0;
  ctx.is_houtei = !tsumo && remaining_ == 0;
  ctx.dora_indicators = dora_indicators_;
  if (ctx.is_riichi) {
    // 里宝指示牌与已翻开的宝牌指示牌一一对应（王牌 9..13）
    const size_t count = std::min<size_t>(dora_indicators_.size(), 5);
    for (size_t i = 0; i < count; ++i) {
      ctx.ura_indicators.push_back(dead_wall_[9 + i]);
    }
  }
  return ctx;
}

void RiichiMahjongGame::ApplyDeltaToPoints(const std::array<int, kSeatCount>& delta) {
  for (int seat = 0; seat < kSeatCount; ++seat) {
    points_[static_cast<size_t>(seat)] += delta[static_cast<size_t>(seat)];
  }
}

Tile RiichiMahjongGame::DrawReplacementTile() {
  // 岭上补牌：从王牌头部取一张，并用牌山山顶牌补满王牌（王牌恒 14 张）。
  // 王牌 0..3 是岭上牌，翻出 4 张之后退回普通摸牌路径（不越界到宝牌指示牌）。
  if (dead_wall_.size() < 14 || dead_draw_index_ >= 4 || next_index_ >= drawable_end_) {
    return DrawFromWall();
  }
  const size_t index = static_cast<size_t>(dead_draw_index_);
  const Tile drawn = dead_wall_[index];
  dead_wall_[index] = wall_[static_cast<size_t>(next_index_)];
  ++dead_draw_index_;
  ++next_index_;
  remaining_ = drawable_end_ - next_index_;
  return drawn;
}

void RiichiMahjongGame::FlipDoraIndicator() {
  if (dead_wall_.size() < 14) {
    return;
  }
  const size_t index = 4 + dora_indicators_.size();
  if (index >= 9) {
    return;  // 最多 5 张指示牌（4..8）
  }
  dora_indicators_.push_back(dead_wall_[index]);
}

// 抢杠窗口（v1.15）：加杠者之外的三家各拿一份「和 / 过」。只有真能和的家才收窗，
// 否则每一次加杠都要等三家把「过」点完（人类玩家会白等一整个超时）。
void RiichiMahjongGame::OpenChankanWindow(GameStep& step, int kan_seat, Tile tile) {
  std::array<bool, kSeatCount> recipients {};
  std::array<std::vector<Option>, kSeatCount> options {};
  bool any_can_rob = false;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (seat == kan_seat) {
      continue;
    }
    const riichi::WinContext ctx = BuildRiichiContext(seat, kan_seat, /*tsumo=*/false, tile);
    options[static_cast<size_t>(seat)] = riichi::ChankanOptions(
        hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)], tile, ctx, IsFuritenFor(seat, tile));
    if (FindOption(options[static_cast<size_t>(seat)], protocol::Action::kWin) != nullptr) {
      recipients[static_cast<size_t>(seat)] = true;
      any_can_rob = true;
    }
  }
  if (!any_can_rob) {
    ApplySelfKan(step, kan_seat, protocol::Action::kKakan, std::vector<Tile> {tile});
    return;
  }
  // 换 seq 再开窗：加杠者的打牌窗口刚被关掉，而它的超时令牌带的是**同一个** seq。
  // 不换的话，那条超时消息会落进抢杠窗口里，把「没人答」当成「没人抢」。
  Transition();
  OpenClaimWindow(step, kan_seat, tile, recipients, options, /*chankan=*/true);
}

std::optional<Tile> RiichiMahjongGame::DefaultDiscardFor(int seat) const {
  const std::vector<Tile>& hand = hands_[static_cast<size_t>(seat)];
  const std::optional<Tile> fallback = DefaultDiscard(hand, drawn_tile_);
  // 喰い替え禁止（v2.5）：冻结的默认动作**不因为新规则而变**，除非它正好落在禁区 ——
  // 那时才按「排序后从大到小、跳过禁区」取第一张（确定性，规则库不参与）。
  // 房间层的 `KuikaeFacesFor` 已保证不会整手都被禁，所以这里一定找得到一张。
  if (!fallback.has_value() || !IsKuikaeForbidden(seat, *fallback)) {
    return fallback;
  }
  const std::vector<Tile> sorted = SortedTiles(hand);
  for (auto it = sorted.rbegin(); it != sorted.rend(); ++it) {
    if (!IsKuikaeForbidden(seat, *it)) {
      return *it;
    }
  }
  return fallback;
}

std::optional<Tile> RiichiMahjongGame::ChooseAiDiscard(int seat) const {
  const size_t index = static_cast<size_t>(seat);
  // 立直能不能治无役，取决于点数/牌山/振听 —— 这些是房间状态，规则库不自己读。
  // 振听用整手口径（`IsFuriten`），与 `CanDeclareRiichiNow` 一致。
  const bool can_riichi = melds_[index].empty() && points_[index] >= riichi::kRiichiStickPoints &&
                          remaining_ >= riichi::kMinRiichiWall && !IsFuriten(seat) && !riichi_declared(seat);
  // 判役用得到的上下文：场风/自风/立直/宝牌指示牌。荣和口径（from_seat >= 0）——
  // 「役」在这里要回答的是「别人打出来我能不能和」。
  const riichi::WinContext ctx =
      BuildRiichiContext(seat, (seat + 1) % kSeatCount, /*tsumo=*/false, TsumoWinningTile(seat));
  return riichi::ChooseDiscard(hands_[index], melds_[index], discards_[index],
                               temporary_furiten_[index], ctx, can_riichi, drawn_tile_, KuikaeFacesFor(seat));
}

// The tile that completes the current seat's hand on its own draw (a tsumo). The dealer's
// 天和 has no draw record, so the largest tile of the hand stands in for it.
Tile RiichiMahjongGame::TsumoWinningTile(int seat) const {
  const std::vector<Tile>& hand = hands_[static_cast<size_t>(seat)];
  if (drawn_tile_.has_value()) {
    return *drawn_tile_;
  }
  const std::vector<Tile> sorted = SortedTiles(hand);
  return sorted.empty() ? Tile {} : sorted.back();
}

Tile RiichiMahjongGame::DrawFromWall() {
  Tile tile = wall_[static_cast<size_t>(next_index_)];
  ++next_index_;
  remaining_ = drawable_end_ - next_index_;
  return tile;
}

// ---------------------------------------------------------------------------------------
// AI pacing (GAME_DESIGN v1.4 §5.4)
// ---------------------------------------------------------------------------------------

bool RiichiMahjongGame::ScheduleAiAction(GameStep& step, int64_t delay_ms) {
  if (!deadlines_supported_) {
    spdlog::error(
        "room {}: no window timeout scheduler installed for the AI delay; "
        "deciding inline instead",
        room_id_);
    return false;
  }
  // Same mechanism as OpenWindowTimer(), different tag: the token carries the current seq
  // plus kAiPacingTokenBit, so the handler can tell the two apart and still drop a message
  // whose window moved on (PROTOCOL.md §0.5).
  Arm(step, delay_ms, MakeAiPacingToken(seq_));
  return true;
}

void RiichiMahjongGame::OpenWindowTimer(GameStep& step) {
  if (!deadlines_supported_) {
    spdlog::error("room {}: no window timeout scheduler installed", room_id_);
    return;
  }
  // The scheduled message carries the window it belongs to; if that window already moved on
  // when it arrives, OnTimeout drops it (seq invalidation, PROTOCOL.md §0.5).
  Arm(step, action_timeout_ms_, seq_);
}

bool RiichiMahjongGame::BotDeclaresTsumo(GameStep& step, int seat) {
  if (!discard_window_win_check_) {
    return false;
  }
  const std::vector<Tile>& hand = hands_[static_cast<size_t>(seat)];
  const std::vector<Meld>& melds = melds_[static_cast<size_t>(seat)];
  const std::vector<Option> options = DiscardOptionsFor(seat);
  const Tile winning = TsumoWinningTile(seat);
  if (FindOption(options, protocol::Action::kWin) == nullptr || !IsWinShape(hand, melds, winning, /*is_tsumo=*/true)) {
    return false;
  }
  discard_window_open_ = false;
  discard_window_win_check_ = false;
  drawn_tile_.reset();
  EndRoundWithWin(step, seat, /*from_seat=*/-1, winning, /*tsumo=*/true);
  return true;
}

bool RiichiMahjongGame::PlayAutomatedDiscard(GameStep& step) {
  const int seat = current_seat_;
  ai_discard_pending_ = false;
  if (!discard_window_open_ || NeedsPlayerReply(seat)) {
    return false;  // the window moved on while the decision was pending
  }
  if (PlaysWithAi(seat)) {
    // A bot must declare the win it is offered instead of discarding it (t24).
    if (BotDeclaresTsumo(step, seat)) {
      return false;  // the game is over
    }
    // 立直后手牌锁定：只能摸切（`ValidateDiscard` 对真人就是这么判的，但 AI 不走 Validate）。
    // 少了这一条，机器人会在立直之后继续按牌效率换牌 —— 那是日麻不允许的，而且会和客户端
    // 「立直后只剩摸切」的显示打架。
    if (RiichiDeclared(seat) && drawn_tile_.has_value()) {
      discard_window_open_ = false;
      // 不手动清 `drawn_tile_`：ApplyDiscard 负责清它，并用它判一发是否失效。
      ApplyDiscard(step, seat, *drawn_tile_);
      return true;
    }
    // 门清听牌且点数够 ⇒ 宣言立直。宣言牌从 `RiichiDiscardTilesFor` 里取（它已经滤掉了
    // 「打出去就振听」的牌面），不能只挑「打完仍听牌」的最小牌 —— 那张可能正好落振听，
    // 白白放弃一次立直。
    if (BotShouldRiichi(seat)) {
      std::optional<Tile> riichi_discard;
      for (Tile candidate : RiichiDiscardTilesFor(seat)) {
        if (CanDeclareRiichiNow(seat, candidate)) {
          riichi_discard = candidate;
          break;
        }
      }
      if (riichi_discard.has_value()) {
        discard_window_open_ = false;
        DeclareRiichi(step, seat);  // 先收棒、再打那张（宣言牌横置，打牌广播随后）
        ApplyDiscard(step, seat, *riichi_discard);
        return true;
      }
    }
    std::optional<Tile> tile = ChooseAiDiscard(seat);
    if (!tile.has_value()) {
      EndRoundWithDraw(step);
      return false;
    }
    discard_window_open_ = false;
    ApplyDiscard(step, seat, *tile);
    return true;
  }
  // A human seat with no wire keeps the frozen §5.6.2 default action, immediately. A
  // disconnected human never reaches this branch: its window stays open until the deadline.
  std::optional<Tile> tile = DefaultDiscardFor(seat);
  if (!tile.has_value()) {
    EndRoundWithDraw(step);
    return false;
  }
  discard_window_open_ = false;
  ApplyDiscard(step, seat, *tile);
  return true;
}

void RiichiMahjongGame::ResolveAutomatedClaimAnswers() {
  ai_claim_pending_ = false;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (!claim_recipient_[static_cast<size_t>(seat)] || claim_answers_[static_cast<size_t>(seat)].answered ||
        NeedsPlayerReply(seat)) {
      continue;
    }
    ClaimAnswer answer;
    if (PlaysWithAi(seat)) {
      const Option choice = ChooseClaimAnswer(hands_[static_cast<size_t>(seat)], melds_[static_cast<size_t>(seat)],
                                              claim_options_[static_cast<size_t>(seat)], claim_tile_);
      answer.action = choice.action;
      answer.tiles = choice.tiles;
    } else if (const Option* pass = FindOption(claim_options_[static_cast<size_t>(seat)], protocol::Action::kPass);
               pass != nullptr) {
      answer.action = pass->action;
    } else {
      answer.action = protocol::Action::kPass;
    }
    answer.answered = true;
    claim_answers_[static_cast<size_t>(seat)] = std::move(answer);
  }
}

bool RiichiMahjongGame::HasPendingHumanWin() const {
  for (int seat = 0; seat < kSeatCount; ++seat) {
    const ClaimAnswer& answer = claim_answers_[static_cast<size_t>(seat)];
    if (answer.answered && answer.action == protocol::Action::kWin && !PlaysWithAi(seat)) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------------------
// Deadlines
// ---------------------------------------------------------------------------------------

GameStep RiichiMahjongGame::OnTimeout(uint64_t token) {
  GameStep step;
  // Three kinds of scheduled messages arrive here: a real window deadline (plain envelope seq),
  // an AI pacing deadline (kAiPacingTokenBit), and the v1.6 round-advance deadline
  // (kRoundAdvanceTokenBit). **判据顺序固定（C6，不得调换）**：
  const bool ai_pacing = IsAiPacingToken(token);
  const bool round_advance = IsRoundAdvanceToken(token);
  const bool match_end = IsMatchEndToken(token);
  const uint64_t window_seq = WindowSeqOfToken(token);
  // ① seq 已作废 ⇒ 一律丢弃（含上一局最后窗口的 timeout —— 它带旧 seq，永远进不了后面）。
  if (window_seq != seq_) {
    spdlog::debug("room {}: stale {} for window {} (current {}) dropped", room_id_,
                  ai_pacing ? "AI pacing deadline"
                            : (round_advance ? "round advance" : (match_end ? "match-end delay" : "timeout")),
                  window_seq, seq_);
    return step;
  }
  // ② AI pacing → 既有分支
  if (ai_pacing) {
    // GAME_DESIGN v1.4 §5.4: the bot decision that was paced. A human reply, a window
    // deadline, or the end of the round all bump seq_, which is why the check above is
    // enough to keep the two mechanisms from ever interleaving.
    if (phase_ == Phase::kDiscardWindow && discard_window_open_ && ai_discard_pending_) {
      if (!PlayAutomatedDiscard(step)) {
        return step;
      }
      Pump(step);
      return step;
    }
    if (phase_ == Phase::kClaimWindow && claim_open_ && ai_claim_pending_) {
      ResolveAutomatedClaimAnswers();
      Pump(step);
      return step;
    }
    return step;
  }
  // ③ 局间推进令牌：仅 kRoundEnd 时恰好推进一局（C5：一次消息只推进一局）；否则丢弃。
  if (round_advance) {
    if (phase_ == Phase::kRoundEnd && round_advance_pending_) {
      round_advance_pending_ = false;
      spdlog::info("room {}: inter-hand delay elapsed, starting round {} (dealer={})", room_id_, round_index_, dealer_);
      StartRound(step);
    }
    return step;
  }
  // ③′ 场终延迟令牌（v1.8 / RULINGS §2.1 C8）：播报窗走完才下发 match_end。相位与局间推进
  // **相同**（kRoundEnd），只能靠各自的 pending 位区分——用 round-advance 位会静默失效。
  if (match_end) {
    if (phase_ == Phase::kRoundEnd && match_end_pending_) {
      match_end_pending_ = false;
      spdlog::info("room {}: broadcast window elapsed, finishing match (rounds={}, hands_played={})", room_id_,
                   round_index_, hands_played_);
      FinishMatch(step);
    }
    return step;
  }
  // ④ 窗口超时
  if (phase_ == Phase::kDiscardWindow && discard_window_open_) {
    const int seat = current_seat_;
    if (!NeedsPlayerReply(seat)) {
      return step;  // a bot / no-wire window never owns this timer
    }
    // A disconnected human owns this deadline too: the seat was not auto-played when its
    // socket closed, so this message is the one that plays the §5.6.2 default action.
    std::optional<Tile> tile = DefaultDiscardFor(seat);
    if (!tile.has_value()) {
      return step;
    }
    // Deliberately NOT ChooseDiscard: "play the tile just drawn (or the largest tile when
    // there is no draw record)" is the deterministic fallback frozen by GAME_DESIGN §5.6.2,
    // and both the t17/t22 statistics and the timeout unit test depend on its predictability.
    // A disconnected seat uses the same default action. Consequence by design: an AI builds
    // its hand, a timed-out human seat does not. 超时永不选杠（PROTOCOL v1.6 §6.4）。
    if (disconnected_[static_cast<size_t>(seat)]) {
      spdlog::info("room {}: seat {} disconnected window expired, default discard {}", room_id_, seat,
                   FormatTile(*tile));
    } else {
      spdlog::info("room {}: seat {} timed out, default discard {}", room_id_, seat, FormatTile(*tile));
    }
    discard_window_open_ = false;
    ApplyDiscard(step, seat, *tile);
    Pump(step);
    return step;
  }
  if (phase_ == Phase::kClaimWindow && claim_open_ && claim_pending_humans_ > 0) {
    for (int seat = 0; seat < kSeatCount; ++seat) {
      if (claim_recipient_[static_cast<size_t>(seat)] && !claim_answers_[static_cast<size_t>(seat)].answered) {
        ClaimAnswer answer;
        answer.answered = true;
        answer.action = protocol::Action::kPass;
        claim_answers_[static_cast<size_t>(seat)] = std::move(answer);
      }
    }
    claim_pending_humans_ = 0;
    spdlog::info("room {}: claim window {} timed out, unanswered seats pass", room_id_, window_seq);
    Pump(step);
  }
  return step;
}

// ---------------------------------------------------------------------------------------
// Sync / reconnect
// ---------------------------------------------------------------------------------------

GameStep RiichiMahjongGame::Sync(int seat) {
  GameStep step;
  Emit(step, seat, BuildStateSync(seat));
  // v1.7 §5.16 T3：`tenpai` 不在 state_sync 里，`sync` 之后必须补发一次——
  // 否则客户端听牌条会永久停在旧值。
  DeliverTenpai(step, seat);
  return step;
}

GameStep RiichiMahjongGame::Reconnect(int seat) {
  GameStep step;
  // PROTOCOL v2.3 §8.5: replay the low-frequency round metadata first, then the authoritative
  // board. The restored client treats game_start as a full local reset. An unanswered window
  // still owns its original deadline, so the replayed request grants no extra time.
  Emit(step, seat, BuildGameStart(seat));
  if (phase_ == Phase::kRoundEnd || phase_ == Phase::kMatchEnd) {
    EmitPerSeat(step, StateSyncFramesRevealed());
  } else {
    EmitPerSeat(step, StateSyncFrames());
  }

  if (phase_ == Phase::kRoundEnd) {
    if (last_settlement_.has_value()) {
      Emit(step, seat, BuildGameEnd(*last_settlement_));
    }
    return step;
  }
  if (phase_ == Phase::kMatchEnd) {
    Emit(step, seat, BuildMatchEnd());
    return step;
  }

  // Playing phases: the listening bar is not part of state_sync (v1.7 §5.16 T3).
  DeliverTenpai(step, seat);
  const int64_t now = clock::EpochMs();
  if (phase_ == Phase::kDiscardWindow && discard_window_open_ && current_seat_ == seat && window_deadline_ms_ > now) {
    // 重放的请求必须与首发逐字节等价：立直宣言牌候选一并带上，否则重连后客户端
    // 就没有「哪几张可以作宣言牌」的提示了（v1.21 §4.4）。
    Emit(step, seat,
         BuildActionRequestFrame(seq_, window_deadline_ms_, protocol::Window::kDiscard, discard_options_, std::nullopt,
                                 discard_riichi_tiles_, KuikaeFacesFor(seat)));
  } else if (phase_ == Phase::kClaimWindow && claim_open_ && claim_recipient_[static_cast<size_t>(seat)] &&
             !claim_answers_[static_cast<size_t>(seat)].answered && window_deadline_ms_ > now) {
    // 抢杠窗口与普通鸣牌窗口是**两种 window**，重放时必须还原成原来那种，并且带上被抢的那张牌
    // （它不在任何人的牌河里，客户端推不出来）。
    Emit(step, seat,
         BuildActionRequestFrame(seq_, window_deadline_ms_,
                                 claim_chankan_ ? protocol::Window::kChankan : protocol::Window::kClaim,
                                 claim_options_[static_cast<size_t>(seat)],
                                 claim_chankan_ ? std::optional<Tile>(claim_tile_) : std::nullopt));
  }
  return step;
}

// ---------------------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------------------

std::array<std::string, kSeatCount> RiichiMahjongGame::StateSyncFrames() const {
  // v1.6（N1 两版缓存）：共享**打码版**（他家 ankan.tiles = []）；仅暗杠持有者额外渲染一份
  // 替换其自身项（额外次数 = 持有者数，实战 ≤2；无暗杠退化 1 份）。禁止每座位重建 4 份，
  // 禁止对所有人下发真实牌面。
  std::array<std::string, kSeatCount> frames;
  std::string masked_json;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (!seats_[static_cast<size_t>(seat)].wired) {
      continue;  // bots have no wire
    }
    if (SeatHoldsAnkan(seat)) {
      // 暗杠持有者：只此座位看到自己的 4 张真实牌面。
      // ⚠️ 视图**必须按持有者逐个构建**，不可跨座位复用（v1.7 修复）：若把第 1 个持有者的
      // 视图缓存后发给第 2 个持有者，后者既看不到自己的暗杠牌面，又能看到前者的暗杠牌面
      // —— 一次复用同时造成「本家信息缺失」与「他家暗杠泄露」两个 P0。
      frames[static_cast<size_t>(seat)] = BuildStateSync(seat, BuildSeatsArray(seat).Dump());
      continue;
    }
    if (masked_json.empty()) {
      masked_json = BuildSeatsArray(kSeatsMaskAll).Dump();  // a rendered 4-entry array is never empty
    }
    frames[static_cast<size_t>(seat)] = BuildStateSync(seat, masked_json);
  }
  return frames;
}

std::array<std::string, kSeatCount> RiichiMahjongGame::StateSyncFramesRevealed() const {
  // v1.6（§5.9 局终揭示）：game_end 前最后一份 state_sync，把**所有** ankan 补全为 4 张真实牌面，
  // 供结算页画完整手牌。复用既有帧，不新增帧、不新增字段。
  std::array<std::string, kSeatCount> frames;
  const std::string revealed = BuildSeatsArray(kSeatsRevealAll).Dump();
  for (int seat = 0; seat < kSeatCount; ++seat) {
    if (!seats_[static_cast<size_t>(seat)].wired) {
      continue;
    }
    frames[static_cast<size_t>(seat)] = BuildStateSync(seat, revealed);
  }
  return frames;
}

bool RiichiMahjongGame::SeatHoldsAnkan(int seat) const {
  for (const Meld& meld : melds_[static_cast<size_t>(seat)]) {
    if (meld.type == MeldType::kAnkan) return true;
  }
  return false;
}

json::Value RiichiMahjongGame::BuildSeatsArray(int viewer) const {
  json::Value seat_array = json::Value::Arr(kSeatCount);
  for (const GameSeat& game_seat : seats_) {
    const size_t seat = static_cast<size_t>(game_seat.seat);
    json::Value entry = json::Value::Obj(8);
    entry.Set("seat", json::Value::Int(game_seat.seat));
    entry.Set("name", json::Value::Str(game_seat.name));
    entry.Set("is_bot", json::Value::Bool(game_seat.is_bot));
    // H6: the public per-seat view MUST NOT contain `hand`, only the tile count.
    entry.Set("hand_count", json::Value::Int(static_cast<int64_t>(hands_[seat].size())));
    entry.Set("discards", TilesToJson(discards_[seat]));
    // v1.6（§5.9 暗杠三态）：本家的 ankan 带 4 张真实牌面；他家恒为 []；局终揭示版全部补全。
    const bool reveal = viewer == kSeatsRevealAll || viewer == game_seat.seat;
    json::Value melds = json::Value::Arr(melds_[seat].size());
    for (const Meld& meld : melds_[seat]) {
      json::Value item = json::Value::Obj(2);  // type, tiles
      item.Set("type", json::Value::Int(protocol::WireOf(protocol::ToWire(meld.type))));
      if (meld.type == MeldType::kAnkan && !reveal) {
        item.Set("tiles", json::Value::Arr(0));  // 他家视角：恒空数组
      } else {
        item.Set("tiles", TilesToJson(meld.tiles));
      }
      melds.Append(std::move(item));
    }
    entry.Set("melds", std::move(melds));
    entry.Set("disconnected", json::Value::Bool(disconnected_[seat]));
    // Omitted when false so a seat that is not entrusted keeps the frozen frame bytes.
    // A bot never carries it: is_bot is not a toggle.
    if (autoplay_[seat] && !game_seat.is_bot) {
      entry.Set("autoplay", json::Value::Bool(true));
    }
    // 立直是公开信息（立直棒与横置），每个收件人都看得到
    entry.Set("riichi", json::Value::Bool(riichi_[seat]));
    seat_array.Append(std::move(entry));
  }
  return seat_array;
}

std::string RiichiMahjongGame::BuildStateSync(int for_seat) const {
  return BuildStateSync(for_seat, BuildSeatsArray(for_seat).Dump());
}

std::string RiichiMahjongGame::BuildStateSync(int for_seat, const std::string& seats_json) const {
  // PROTOCOL v1.7 §5.16 裁决：听牌数据走**独立的 `tenpai` 帧**（仅本家、不塞 state_sync），
  // 因此 state_sync 的字段集与 v1.6 完全一致（§11.2 的「state_sync.waits」是同一文档里的
  // 过期草案，§5.16/§8.1/§11.3 与客户端契约均以 tenpai 帧为准）。
  json::Value frame = json::Value::Obj(9);  // + points / dora_indicators（日麻专有）
  frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kStateSync)));
  frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq_)));
  frame.Set("remaining", json::Value::Int(remaining_));
  frame.Set("current_seat", json::Value::Int(current_seat_));
  frame.Set("hand", TilesToJson(hands_[static_cast<size_t>(for_seat)]));
  frame.Set("melds", MeldsToJson(melds_[static_cast<size_t>(for_seat)]));
  // Spliced verbatim; |seats_json| is exactly what building the array in here would have written
  // (BuildSeatsArray().Dump()), which is why the frame bytes are unchanged (§5.9).
  frame.Set("seats", json::Value::RawJson(seats_json));
  // 持点是公开信息，四家都给全量；宝牌指示牌也是公开的（暗杠后新翻的那张照样公开）。
  json::Value scores = json::Value::Arr(kSeatCount);
  for (int seat = 0; seat < kSeatCount; ++seat) {
    scores.Append(json::Value::Int(points_[static_cast<size_t>(seat)]));
  }
  frame.Set("points", std::move(scores));
  frame.Set("dora_indicators", TilesToJson(dora_indicators_));
  return frame.Dump();
}

// PROTOCOL v1.7 §5.16 / v1.13 §12.8：`tenpai` 帧 = **本家私有的听牌全量快照**（不是增量、不参与 room.seq）。
//   * `tenpai_seq`：每座位一条独立单调计数器，从 1 起、**跨局不重置**（新场次才归零）；
//   * `is_tenpai`：等待态（concealed + 3×melds == 13）且存在至少一张可和牌面；
//   * `waits[]`：§2.2 牌序、同面至多一条；`is_tenpai == false` 时恒为 `[]`（MUST 下发清空信号）；
//   * `waits[].remaining` = **可见枚数** = 4 −（本家手牌 + 四家牌河 + 四家副露）中该牌面的张数，
//     **他家暗杠的 4 张不扣减**（扣了就能反推出暗杠牌面，§5.16 冻结口径）；**牌山真值永不入帧**。
json::Value RiichiMahjongGame::BuildTenpaiFrame(int seat) const {
  const std::vector<Tile>& concealed = hands_[static_cast<size_t>(seat)];
  const std::vector<Meld>& melds = melds_[static_cast<size_t>(seat)];
  json::Value waits = json::Value::Arr(0);
  const bool waiting_state = concealed.size() + 3 * melds.size() == 13;
  std::vector<Tile> faces;
  if (waiting_state) {
    // 日麻多认一种等待：国士无双（共享的 `WaitingTiles` 只认标准型 / 七对子）
    faces = riichi::RiichiWaitingTiles(concealed, melds);
  }
  // `is_tenpai` 仍是**形式听牌**（流局罚符与立直资格都看它），
  // 但提示必须区分「形听」与「能和」——副露无役时形听却永远和不了。这里逐张按荣和口径
  // 判役（已立直时立直本身是役，故恒全部有役）。
  std::vector<Tile> winning;
  bool no_yaku = false;
  if (!faces.empty()) {
    const riichi::WinContext hint_ctx =
        BuildRiichiContext(seat, (seat + 1) % kSeatCount, /*tsumo=*/false, faces.front());
    // `faces` 已经算过等待张，传进去让 `WinningWaits` 不必再跑一遍 `RiichiWaitingTiles`
    // （实测：九面听下一遍就是 79 µs Release / 472 µs Debug，占整帧的一半）。
    winning = riichi::WinningWaits(concealed, melds, hint_ctx, faces);
    no_yaku = winning.empty();
  }
  // 整手是否振听（任一等待张被挡住）。`no_yaku` 与 `furiten` 是**两个独立的否决位**：
  // 无役和振听都会让「形听」变成「和不了」，但原因不同、解法也不同（无役可立直/自摸，振听只能等解禁）。
  bool furiten = false;
  for (Tile face : faces) {
    if (IsFuritenFor(seat, face)) {
      furiten = true;
      break;
    }
  }
  if (!faces.empty()) {
    FaceCounts visible {};
    visible.fill(0);
    for (Tile tile : concealed) ++visible[FaceOf(tile)];
    for (int other = 0; other < kSeatCount; ++other) {
      for (const Meld& meld : melds_[static_cast<size_t>(other)]) {
        // 他家暗杠：牌面对本家不可见 ⇒ **不扣减**（否则 4 − 可见 会反推出暗杠牌面）。
        if (meld.type == MeldType::kAnkan && other != seat) continue;
        for (Tile tile : meld.tiles) ++visible[FaceOf(tile)];
      }
      for (Tile tile : discards_[static_cast<size_t>(other)]) ++visible[FaceOf(tile)];
    }
    for (Tile face : faces) {
      const int seen = visible[FaceOf(face)];
      const int left = seen >= kCopiesPerFace ? 0 : kCopiesPerFace - seen;
      json::Value item = json::Value::Obj(4);  // tile, remaining, yaku, furiten
      item.Set("tile", json::Value::Str(FormatTile(face)));
      item.Set("remaining", json::Value::Int(left));
      const bool has_yaku = std::find(winning.begin(), winning.end(), face) != winning.end();
      item.Set("yaku", json::Value::Bool(has_yaku));  // 荣和成立（含已立直）；宝牌不算役
      // v1.24 逐张振听：这张牌**现在**能不能荣和由振听决定（舍て牌 / 见逃 / 同巡）。
      // 与 `yaku` 是**两个独立的否决位**：有役但振听 ⇒ 仍然和不了。
      item.Set("furiten", json::Value::Bool(IsFuritenFor(seat, face)));
      waits.Append(std::move(item));
    }
  }
  json::Value frame = json::Value::Obj(7);  // + furiten / waits[].furiten（v1.24）
  frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kTenpai)));
  frame.Set("tenpai_seq", json::Value::Int(static_cast<int64_t>(tenpai_seq_[static_cast<size_t>(seat)])));
  frame.Set("is_tenpai", json::Value::Bool(!faces.empty()));
  frame.Set("waits", std::move(waits));
  // true = 形听但**所有**等待张都荣和不了（副露无役 = 死听；门清则还有立直/自摸两条路）
  frame.Set("no_yaku", json::Value::Bool(no_yaku));
  // v1.24：振听是**本家私有**信息（他家算不出来：同巡振听记的是「我放过了什么」，
  // 而舍て牌振听要看的是「我自己的等待张 vs 我自己的牌河」），所以随这个仅本家的帧下发。
  // `furiten` = 整手是否处于振听（舍て牌 / 见逃 / 同巡任一）；`waits[].furiten` 是逐张细口径。
  frame.Set("furiten", json::Value::Bool(furiten));
  return frame;
}

// §5.16 下发（T1 打牌后 / T2 副露后 / T3 sync 补发）：只发本家、一份；bot 无 wire 不发。
// PB-2：构建耗时超 300µs 打一次 warn（日志巡检；纯测量、不进任何帧字段）。
void RiichiMahjongGame::DeliverTenpai(GameStep& step, int seat) {
  if (seat < 0 || seat >= kSeatCount || !seats_[static_cast<size_t>(seat)].wired) {
    return;
  }
  ++tenpai_seq_[static_cast<size_t>(seat)];  // 每座位独立计数，跨局不重置
  const auto started = std::chrono::steady_clock::now();
  const std::string frame = BuildTenpaiFrame(seat).Dump();
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
  // 日麻要比 classic 多做一次「逐张判役」（winning 那张要按荣和口径判役是否有役），
  // 九面听这类宽听最坏约 600 µs，故门槛放到 1000 µs。
  if (us > 1000.0) {
    spdlog::warn("slow tenpai build: {:.0f} us (seat {}, room {})", us, seat, room_id_);
  }
  Emit(step, seat, frame);
}

std::string RiichiMahjongGame::BuildGameStart(int for_seat) const {
  return BuildGameStartFrame(for_seat, dealer_, round_index_, round_count_, hands_[static_cast<size_t>(for_seat)],
                             points_, dora_indicators_, round_wind_, match_totals_, renchan_count_);
}

std::string RiichiMahjongGame::BuildMatchEnd() const {
  // PROTOCOL v1.6 §5.15：场终帧，无 seq。rank = 1 + 严格大于本座位分数的人数（并列跳号）；
  // 排序 final_score 降序、同分 seat 升序。
  struct Ranked {
    int seat;
    int score;
    int rank;
  };
  std::array<Ranked, kSeatCount> standings {};
  for (int seat = 0; seat < kSeatCount; ++seat) {
    // 日麻按终局持点排名（25000 起），不是按累计得失分——两者差一个常数与烧鸟/返点，
    // 排序结果在「同分」边界上会不同（§5.15 的 final_score 在日麻下就是持点）。
    const int score = points_[static_cast<size_t>(seat)];
    standings[static_cast<size_t>(seat)] = Ranked {.seat = seat, .score = score, .rank = 1};
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
  frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kMatchEnd)));
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

std::string RiichiMahjongGame::BuildGameEnd(const Settlement& settlement) const {
  json::Value frame = json::Value::Obj(11);  // type, reason, round_index, dealer,
                                             // renchan_count, match_will_continue, interlude_ms,
                                             // results, points_after, tenpai, kyoutaku_award, win
  frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kGameEnd)));
  frame.Set("reason", json::Value::Int(protocol::WireOf(settlement.reason)));
  // v1.6 新增顶层字段：场次进度（game_end 是低频帧，state_sync 不带这些，§5.9 性能注）
  frame.Set("round_index", json::Value::Int(settlement.round_index));
  frame.Set("dealer", json::Value::Int(settlement.dealer));
  frame.Set("renchan_count", json::Value::Int(settlement.renchan_count));
  frame.Set("match_will_continue", json::Value::Bool(settlement.match_will_continue));
  // v1.8（SETTLEMENT_BROADCAST_RULINGS §2.1）：本房间生效的局间窗口（播报窗 + 阅读窗）。
  // **每一份 game_end 都带**——荒庄与场终那一局也不例外（客户端按它算播报预算与倒计时）。
  frame.Set("interlude_ms", json::Value::Int(inter_hand_delay_ms_));

  json::Value results = json::Value::Arr(kSeatCount);
  for (int seat = 0; seat < kSeatCount; ++seat) {
    json::Value entry = json::Value::Obj(5);  // seat, delta_score, match_total, hand, win_type
    entry.Set("seat", json::Value::Int(seat));
    entry.Set("delta_score", json::Value::Int(settlement.delta[static_cast<size_t>(seat)]));
    // v1.6：本场累计得失分（含本局），四家之和恒为 0
    entry.Set("match_total", json::Value::Int(settlement.totals[static_cast<size_t>(seat)]));
    // v4.5：四家都下发扁平手牌（门前 + 副露），供结算播报摊牌；仍只存在于 game_end。
    entry.Set("hand", TilesToJson(settlement.flat_hands[static_cast<size_t>(seat)]));
    if (seat == settlement.winner_seat) {
      json::Value types = json::Value::Arr(settlement.winner_types.size());
      for (protocol::YakuId type : settlement.winner_types) {
        types.Append(json::Value::Int(protocol::WireOf(type)));
      }
      entry.Set("win_type", std::move(types));
    } else {
      entry.Set("win_type", json::Value::Arr());
    }
    results.Append(std::move(entry));
  }
  frame.Set("results", std::move(results));

  // 日麻专有：本局结束后的四家持点 + 荒庄时谁听了牌 + 场终那次供托归属
  json::Value scores = json::Value::Arr(kSeatCount);
  json::Value tenpai = json::Value::Arr(kSeatCount);
  for (int seat = 0; seat < kSeatCount; ++seat) {
    scores.Append(json::Value::Int(settlement.points_after[static_cast<size_t>(seat)]));
    tenpai.Append(json::Value::Bool(settlement.tenpai[static_cast<size_t>(seat)]));
  }
  frame.Set("points_after", std::move(scores));
  frame.Set("tenpai", std::move(tenpai));  // 荒庄时四家是否听牌（和牌局恒 false）
  // 场终那一局把剩下的立直棒归了一位 —— 这笔已计入 results[].delta_score /
  // points_after，这里只把「归了谁、几根」单独报出来，供结算页写「场供 2 根 → 一位」。
  // 非场终局恒 null（供托还在台面上等下一次和牌）。
  if (settlement.kyoutaku_award > 0) {
    json::Value award = json::Value::Obj(3);  // seat, sticks, points
    award.Set("seat", json::Value::Int(settlement.kyoutaku_award_seat));
    award.Set("sticks", json::Value::Int(settlement.kyoutaku_award));
    award.Set("points", json::Value::Int(settlement.kyoutaku_award * riichi::kRiichiStickPoints));
    frame.Set("kyoutaku_award", std::move(award));
  } else {
    frame.Set("kyoutaku_award", json::Value::Null());
  }

  if (settlement.winner_seat < 0) {
    frame.Set("win", json::Value::Null());
  } else {
    json::Value win = json::Value::Obj(22);
    // seat, tile, from_seat, tsumo, tags, multiplier, per_payer, total,
    // yaku, han, han_breakdown, dealer_bonus, rinshan,
    // fu, points, limit, dora, ura_dora, riichi, aka_dora, dora_indicators, kyoutaku, honba
    win.Set("seat", json::Value::Int(settlement.winner_seat));
    win.Set("tile", json::Value::Str(FormatTile(settlement.winning_tile, /*red=*/true)));
    if (settlement.tsumo) {
      win.Set("from_seat", json::Value::Null());  // the field must exist for a tsumo (§5.10)
    } else {
      win.Set("from_seat", json::Value::Int(settlement.from_seat));
    }
    win.Set("tsumo", json::Value::Bool(settlement.tsumo));
    json::Value tags = json::Value::Arr(settlement.tags.size());
    for (protocol::YakuId tag : settlement.tags) {
      tags.Append(json::Value::Int(protocol::WireOf(tag)));
    }
    win.Set("tags", std::move(tags));
    win.Set("multiplier", json::Value::Int(settlement.multiplier));
    win.Set("per_payer", json::Value::Int(settlement.per_payer));
    win.Set("total", json::Value::Int(settlement.total));
    // v1.6（B1 番数制）：win.yaku[] 按冻结判定序，每 id 至多一条
    json::Value yaku = json::Value::Arr(settlement.yaku.size());
    for (const Yaku& item : settlement.yaku) {
      json::Value entry = json::Value::Obj(2);  // id, han
      entry.Set("id", json::Value::Int(protocol::WireOf(item.id)));
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
    // 日麻载荷：符 / 点数 / 满贯分档 / 宝牌 / 立直 / 供托与本场
    win.Set("fu", json::Value::Int(settlement.fu));
    win.Set("points", json::Value::Int(settlement.points));
    win.Set("limit", json::Value::Int(protocol::WireOf(ToWire(settlement.limit))));
    win.Set("dora", json::Value::Int(settlement.dora));
    win.Set("ura_dora", json::Value::Int(settlement.ura_dora));
    win.Set("riichi", json::Value::Bool(settlement.riichi));
    win.Set("aka_dora", json::Value::Int(settlement.aka_dora));
    win.Set("dora_indicators", TilesToJson(settlement.dora_indicators));
    win.Set("kyoutaku", json::Value::Int(settlement.kyoutaku));
    win.Set("honba", json::Value::Int(settlement.honba));
    frame.Set("win", std::move(win));
  }
  return frame.Dump();
}

int RiichiRoundCountOf(int length_option) { return length_option == kHanchanLengthOption ? 8 : 4; }

void RiichiMahjongGame::BroadcastSettlement(GameStep& step, const Settlement& settlement) {
  // PROTOCOL v2.2 §8.5: keep the last settlement so a reconnect during kRoundEnd/kMatchEnd can
  // replay the same game_end the other seats already received.
  last_settlement_ = settlement;
  EmitAll(step, BuildGameEnd(settlement));
}

}  // namespace mahjong
