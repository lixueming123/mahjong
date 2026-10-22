// t4 unit tests for the RoomActor state machine.
//
// The suite drives the actor directly (it is a plain class; every public method runs to
// completion inside one "message"), with a capturing seat sink instead of a socket and a
// capturing timeout scheduler instead of the real WindowTimer. Turn driving, the frozen
// seq invalidation scheme, illegal-operation protection, the AI seats and the settlement
// are therefore fully deterministic and free of real time.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "actor_scope.h"
#include "json.h"
#include "mahjong_rules.h"
#include "player.h"
#include "protocol.h"
#include "room.h"
#include "seat_endpoint.h"

#include <spdlog/spdlog.h>

namespace mahjong {
namespace {

constexpr int kSeat0 = 0;
constexpr int64_t kDefaultTimeoutMs = 15000;

// Records every frame a seat receives and parses it back into JSON. It is no longer a sink the
// room pushes into (the room messages actors now); a CapturingSeatActor below is what receives
// the messages and writes here, which keeps every assertion in this file unchanged.
class CapturingSink {
 public:
  void Push(std::string frame) {
    std::lock_guard lock(mutex_);
    frames_.push_back(std::move(frame));
  }

  std::vector<json::Value> Drain() {
    std::lock_guard lock(mutex_);
    std::vector<json::Value> parsed;
    parsed.reserve(frames_.size());
    for (const std::string& frame : frames_) {
      std::string error;
      std::optional<json::Value> value = json::Value::Parse(frame, &error);
      if (value.has_value()) {
        parsed.push_back(std::move(*value));
      } else {
        ADD_FAILURE() << "seat sink received a non-JSON frame: " << frame;
      }
    }
    frames_.clear();
    return parsed;
  }

  size_t size() const {
    std::lock_guard lock(mutex_);
    return frames_.size();
  }

  // PROTOCOL §7.2 accounting, as the real PlayerActor does for its own connection.
  void RecordBadFrame() { ++bad_frames_; }
  void RecordIllegalOp() { ++illegal_ops_; }

  int bad_frames() const { return bad_frames_; }
  int illegal_ops() const { return illegal_ops_; }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> frames_;
  int bad_frames_ = 0;
  int illegal_ops_ = 0;
};

// The seat endpoint a room delivers to in these tests: it plays the part PlayerActor plays
// in production, minus the wire. Because the room *awaits* its deliveries, `sync_wait(Dispatch)`
// returning means the frames are already recorded — the synchronous assertion style of this
// file survives the switch to actor messaging.
class CapturingSeatActor : public SeatEndpoint {
 public:
  explicit CapturingSeatActor(std::shared_ptr<CapturingSink> sink) : sink_(std::move(sink)) {}

  exec::task<void> Dispatch(PlayerEnvelope envelope) override {
    if (auto* m = std::get_if<PlayerEnvelope::SeatFrameMsg>(&envelope.msg)) {
      sink_->Push(std::move(m->frame));
    } else if (auto* e = std::get_if<PlayerEnvelope::SeatErrorMsg>(&envelope.msg)) {
      sink_->Push(std::move(e->frame));
      if (e->quota == QuotaKind::kBadFrame) {
        sink_->RecordBadFrame();
      } else if (e->quota == QuotaKind::kIllegalOp) {
        sink_->RecordIllegalOp();
      }
    }
    // Everything else — Close and the connection's own kinds — never comes from a room.
    co_return;
  }

 private:
  std::shared_ptr<CapturingSink> sink_;
};

std::string TypeOf(const json::Value& frame) {
  const json::Value* type = frame.Find("type");
  return type != nullptr && type->IsString() ? type->AsString() : std::string("<none>");
}

int64_t IntOf(const json::Value& frame, const std::string& key) {
  const json::Value* value = frame.Find(key);
  return value != nullptr && value->IsNumber() ? value->AsInt() : 0;
}

std::string StrOf(const json::Value& frame, const std::string& key) {
  const json::Value* value = frame.Find(key);
  return value != nullptr && value->IsString() ? value->AsString() : std::string();
}

const json::Value* OptionOf(const json::Value& request, const std::string& action) {
  const json::Value* options = request.Find("options");
  if (options == nullptr || !options->IsArray()) {
    return nullptr;
  }
  for (const json::Value& option : options->AsArray()) {
    if (StrOf(option, "action") == action) {
      return &option;
    }
  }
  return nullptr;
}

std::vector<Tile> TilesOf(const json::Value& frame, const std::string& key) {
  std::vector<Tile> tiles;
  const json::Value* array = frame.Find(key);
  if (array == nullptr || !array->IsArray()) {
    return tiles;
  }
  for (const json::Value& entry : array->AsArray()) {
    std::optional<Tile> tile = ParseTile(entry.AsString());
    if (tile.has_value()) {
      tiles.push_back(*tile);
    }
  }
  return tiles;
}

std::vector<Tile> FlattenMelds(const std::vector<Meld>& melds) {
  std::vector<Tile> tiles;
  for (const Meld& meld : melds) {
    tiles.insert(tiles.end(), meld.tiles.begin(), meld.tiles.end());
  }
  return tiles;
}

int KongCount(const std::vector<Meld>& melds) { return KanCountOf(melds); }

// A frame's tiles come back through ParseTile, which keeps the face only — the physical copy
// index is deliberately not on the wire (PROTOCOL.md §4.6 / RULESET §1.2) — while the room's own
// vectors hold real tiles. Comparing them therefore has to go through faces, not Tile::operator==.
bool SameFaces(const std::vector<Tile>& lhs, const std::vector<Tile>& rhs) {
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

// A room with one human seat (seat 0, captured by a sink) and three AI seats.
// The room's seat endpoint is a real spawned actor, so these tests need the ex_actor runtime.
class RoomActorTest : public ::testing::Test {
 protected:
  void SetUp() override { ex_actor::Init(/*thread_pool_size=*/2); }
  void TearDown() override { stdexec::sync_wait(ActorSendScope().on_empty());
    ex_actor::Shutdown(); }
};

class TestRoom {
 public:
  // |ai_delay_ms| defaults to 0 here: the pre-t31 behaviour is fully synchronous, which keeps
  // the flow-driven fixtures below fast and unchanged. The tests that assert the pacing pass a
  // non-zero value explicitly.
  // |smart_discard| makes the human keep its own hand efficient with the very same public
  // policy the bots use (ChooseDiscard). It only changes the fixture's *input* so a human
  // claim/win window is reachable in a reasonable number of games; no assertion depends on
  // which tile the human seat played.
  // |install_scheduler| = false models a room whose timeout scheduler was never installed
  // (impossible in production, where lobby.cc installs it before the room is reachable).
  // |all_human| wires a human on every seat instead of one human and three bots. This is the
  // only shape that exercises the multi-recipient broadcast path — four state_sync frames per
  // broadcast, four-way fan-out of discard_event/action_event. With a single wired human those
  // loops build exactly one frame, so whatever they share is invisible to the rest of this file.
  explicit TestRoom(uint64_t seed, int action_timeout_ms = kDefaultTimeoutMs, int ai_delay_ms = 0,
                    bool smart_discard = false, bool install_scheduler = true, bool all_human = false,
                    int inter_hand_delay_ms = 0) {
    smart_discard_ = smart_discard;
    human_sink_ = std::make_shared<CapturingSink>();
    std::vector<RoomSeat> seats;
    for (int seat = 0; seat < kSeatCount; ++seat) {
      RoomSeat slot;
      slot.seat = seat;
      slot.is_bot = seat != kSeat0 && !all_human;
      if (slot.is_bot) {
        slot.player.player_id = "bot-" + std::to_string(seat);
        slot.player.name = "AI-" + std::to_string(seat);
      } else {
        // Seat 0 keeps the frozen "p-human"/"Human" identity: the one-human shape and the
        // histories the rest of this file asserts on stay byte-identical.
        slot.player.player_id = seat == kSeat0 ? "p-human" : "p-human-" + std::to_string(seat);
        slot.player.name = seat == kSeat0 ? "Human" : "Human-" + std::to_string(seat);
      }
      seats.push_back(std::move(slot));
    }
    if (all_human) {
      for (int seat = 0; seat < kSeatCount; ++seat) {
        sinks_[static_cast<size_t>(seat)] = std::make_shared<CapturingSink>();
        seats[static_cast<size_t>(seat)].endpoint =
            std::get<0>(stdexec::sync_wait(ex_actor::Spawn<CapturingSeatActor>(sinks_[static_cast<size_t>(seat)]))
                            .value());
      }
      human_sink_ = sinks_[kSeat0];
    } else {
      // A real seat endpoint: the room only reaches a seat by messaging one of these.
      ex_actor::ActorRef<CapturingSeatActor> endpoint =
          std::get<0>(stdexec::sync_wait(ex_actor::Spawn<CapturingSeatActor>(human_sink_)).value());
      seats.front().endpoint = endpoint;
      sinks_[kSeat0] = human_sink_;
    }
    human_ = seats.front().player;
    room_ = std::make_unique<RoomActor>("r-0001", seed, action_timeout_ms, std::move(seats), nullptr, ai_delay_ms,
                                        inter_hand_delay_ms);
    if (install_scheduler) {
      Send(RoomEnvelope::InstallTimeoutScheduler([this](int64_t delay_ms, uint64_t window_seq) {
        scheduled_.emplace_back(delay_ms, window_seq);
      }));
    }
  }

  RoomActor* room() { return room_.get(); }
  CapturingSink* sink() { return human_sink_.get(); }
  // Per-seat sink; only the |all_human| shape wires more than seat 0.
  CapturingSink* sink_of(int seat) { return sinks_[static_cast<size_t>(seat)].get(); }

  // RoomActor::Dispatch is a coroutine (it takes the actor's gate), so a direct caller has to
  // drive it: the fixture never has anything else in flight, so sync_wait runs it to completion
  // on this thread — the same synchronous behaviour the old direct handler call had.
  void Send(RoomEnvelope envelope) { stdexec::sync_wait(room_->Dispatch(std::move(envelope))); }

  // Models the callback LobbyActor installs after Spawn (PROTOCOL §1.4.5).
  void InstallFinishedCallback(RoomFinishedCallback callback) {
    Send(RoomEnvelope::InstallFinishedCallback(std::move(callback)));
  }

  const PlayerInfo& human() const { return human_; }
  const std::vector<std::pair<int64_t, uint64_t>>& scheduled() const { return scheduled_; }

  // Scheduled messages that are AI pacing deadlines (as opposed to window deadlines).
  std::vector<std::pair<int64_t, uint64_t>> AiPacingScheduled() const {
    std::vector<std::pair<int64_t, uint64_t>> out;
    for (const std::pair<int64_t, uint64_t>& entry : scheduled_) {
      if (IsAiPacingToken(entry.second)) {
        out.push_back(entry);
      }
    }
    return out;
  }

  // Removes and returns the newest pending AI pacing token (0 when there is none). Tests never
  // sleep: they deliver the message themselves, exactly like the timer thread would.
  uint64_t TakeAiPacing() {
    for (size_t i = scheduled_.size(); i > 0; --i) {
      if (IsAiPacingToken(scheduled_[i - 1].second)) {
        const uint64_t token = scheduled_[i - 1].second;
        scheduled_.erase(scheduled_.begin() + static_cast<std::ptrdiff_t>(i - 1));
        return token;
      }
    }
    return 0;
  }

  bool FireAiPacing() {
    const uint64_t token = TakeAiPacing();
    if (token == 0) {
      return false;
    }
    Send(RoomEnvelope::WindowTimeout(token));
    return true;
  }

  // v1.6（C6）：局间推进令牌的测试驱动（与 TakeAiPacing/FireAiPacing 同型）。
  uint64_t TakeRoundAdvance() {
    for (size_t i = scheduled_.size(); i > 0; --i) {
      if (IsRoundAdvanceToken(scheduled_[i - 1].second)) {
        const uint64_t token = scheduled_[i - 1].second;
        scheduled_.erase(scheduled_.begin() + static_cast<std::ptrdiff_t>(i - 1));
        return token;
      }
    }
    return 0;
  }

  bool RoundAdvanceScheduled() const {
    for (const std::pair<int64_t, uint64_t>& entry : scheduled_) {
      if (IsRoundAdvanceToken(entry.second)) {
        return true;
      }
    }
    return false;
  }

  bool FireRoundAdvance() {
    const uint64_t token = TakeRoundAdvance();
    if (token == 0) {
      return false;
    }
    Send(RoomEnvelope::WindowTimeout(token));
    return true;
  }

  int DiscardsBy(int seat) const {
    int count = 0;
    for (const json::Value& frame : history_) {
      if (TypeOf(frame) == "discard_event" && IntOf(frame, "seat") == seat) {
        ++count;
      }
    }
    return count;
  }
  const std::vector<json::Value>& history() const { return history_; }
  std::optional<Tile> drawn() const { return drawn_; }

  void Ready() { Send(RoomEnvelope::Ready(human_)); }

  void SendDiscard(Tile tile, uint64_t seq) {
    json::Value frame = json::Value::Obj();
    frame.Set("type", json::Value::Str("discard"));
    frame.Set("tile", json::Value::Str(FormatTile(tile)));
    frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
    Send(RoomEnvelope::GameFrame(human_, std::move(frame)));
  }

  void SendAction(const std::string& action, const std::vector<Tile>& tiles, uint64_t seq) {
    json::Value frame = json::Value::Obj();
    frame.Set("type", json::Value::Str("action"));
    frame.Set("action", json::Value::Str(action));
    json::Value tile_array = json::Value::Arr();
    for (Tile tile : tiles) {
      tile_array.Append(json::Value::Str(FormatTile(tile)));
    }
    frame.Set("tiles", std::move(tile_array));
    frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
    Send(RoomEnvelope::GameFrame(human_, std::move(frame)));
  }

  // Drains the sink into the history and returns the request that still belongs to the open
  // window (if any).
  std::optional<json::Value> PendingRequest() {
    std::vector<json::Value> frames = human_sink_->Drain();
    std::optional<json::Value> pending;
    for (json::Value& frame : frames) {
      const std::string type = TypeOf(frame);
      if (type == "draw") {
        drawn_ = ParseTile(StrOf(frame, "tile"));
      }
      if (type == "action_request" && static_cast<uint64_t>(IntOf(frame, "seq")) == room_->seq()) {
        pending = frame;
      }
      history_.push_back(std::move(frame));
    }
    return pending;
  }

  // Answers the open window for the human seat with a deterministic, always-legal policy.
  // It never drains the sink itself: |request| must be the outcome of the caller's single
  // PendingRequest() call, so a window request can never be consumed before the policy sees
  // it (draining twice used to make the fixture give up on a claim window).
  // Returns false when there is nothing the human may answer.
  bool AnswerWindow(const std::optional<json::Value>& request) {
    const std::string window = room_->CurrentWindow();
    const uint64_t seq = room_->seq();
    if (window == "discard" && room_->current_seat() == kSeat0) {
      if (request.has_value() && OptionOf(*request, "win") != nullptr) {
        SendAction("win", {}, seq);
      } else {
        std::vector<Tile> hand = SortedTiles(room_->hand_of(kSeat0));
        Tile tile;
        if (smart_discard_) {
          std::optional<Tile> chosen = ChooseDiscard(room_->hand_of(kSeat0), room_->melds_of(kSeat0), drawn_);
          tile = chosen.has_value() ? *chosen : hand.back();
        } else {
          tile = drawn_.value_or(hand.back());
        }
        SendDiscard(tile, seq);
        drawn_.reset();
      }
      return true;
    }
    if (!request.has_value() || StrOf(*request, "window") != "claim") {
      return false;
    }
    if (OptionOf(*request, "win") != nullptr) {
      SendAction("win", {}, seq);
    } else if (const json::Value* pong = OptionOf(*request, "pong"); pong != nullptr) {
      SendAction("pong", TilesOf(*pong, "tiles"), seq);
    } else {
      SendAction("pass", {}, seq);
    }
    return true;
  }

  // Convenience overload for callers that want drain-then-answer in one call.
  bool AnswerWindow() { return AnswerWindow(PendingRequest()); }

  // When the human has no window to answer, the room must still be able to move on. This
  // delivers the frozen §5.6.2 timeout message for the window that is currently open, which
  // resolves it with the default action (discard the drawn tile / pass). Returns true when
  // the room actually advanced.
  bool AdvanceOpenWindow() {
    if (room_->game_finished()) {
      return false;
    }
    const uint64_t before = room_->seq();
    Send(RoomEnvelope::WindowTimeout(before));
    return room_->game_finished() || room_->seq() != before;
  }

  // One fixture-driven step: answer the human's window if there is one, otherwise let the
  // open window expire. Returns false when the game is over or nothing could be advanced, so
  // no caller can spin forever.
  bool StepGame() {
    if (room_->game_finished()) {
      return false;
    }
    // v1.6（C5/C6）：局终后由推进令牌驱动下一局（一次一步，从不循环推进）。
    if (room_->round_finished()) {
      return FireRoundAdvance();
    }
    if (AnswerWindow()) {
      return true;
    }
    if (FireAiPacing()) {
      return true;  // the bot's paced decision (only present when ai_delay_ms > 0)
    }
    return AdvanceOpenWindow();
  }

  // Plays a whole match（v1.6：一场 4..8 局）with the policy above; returns the number of driven steps.
  int PlayFullGame(int max_windows = 20000) {
    int answered = 0;
    while (!room_->game_finished() && answered < max_windows) {
      if (!StepGame()) {
        break;
      }
      ++answered;
    }
    PendingRequest();  // flush the tail
    return answered;
  }

  // v1.6：只打**一局**就停（不交付推进令牌）。返回 false 表示未能在步数内走到局终。
  bool PlayFullRound(int max_windows = 6000) {
    int answered = 0;
    while (!room_->round_finished() && answered < max_windows) {
      if (room_->game_finished()) {
        return true;  // 场终当然也算局终
      }
      if (AnswerWindow()) {
        ++answered;
        continue;
      }
      if (FireAiPacing()) {
        ++answered;
        continue;
      }
      if (!AdvanceOpenWindow()) {
        break;
      }
      ++answered;
    }
    PendingRequest();  // flush the tail
    return room_->round_finished();
  }

 private:
  bool smart_discard_ = false;
  PlayerInfo human_;
  std::shared_ptr<CapturingSink> human_sink_;
  std::array<std::shared_ptr<CapturingSink>, kSeatCount> sinks_;
  std::unique_ptr<RoomActor> room_;
  std::vector<std::pair<int64_t, uint64_t>> scheduled_;
  std::optional<Tile> drawn_;
  std::vector<json::Value> history_;
};

std::vector<const json::Value*> FramesOfType(const std::vector<json::Value>& frames, const std::string& type) {
  std::vector<const json::Value*> matches;
  for (const json::Value& frame : frames) {
    if (TypeOf(frame) == type) {
      matches.push_back(&frame);
    }
  }
  return matches;
}

int CountOfType(const std::vector<json::Value>& frames, const std::string& type) {
  return static_cast<int>(FramesOfType(frames, type).size());
}

// ---------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------

}  // namespace
}  // namespace mahjong

int main(int argc, char** argv) {
  // The room logs a lot while 40 full games are simulated; keep gtest output readable.
  spdlog::set_level(spdlog::level::warn);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

namespace mahjong {
namespace {

TEST_F(RoomActorTest, GatewayTileGateFollowsTheRulesLibrary) {
  // Regression guard for the tile-set expansion: the gateway's syntax gate must accept
  // exactly what the rules library can parse, so that honour tiles do not get rejected with
  // UNKNOWN_TILE before they ever reach the room.
  for (const char* code : {"1m", "9s", "1z", "7z", "8z", "5x", "10z", "", "0m"}) {
    EXPECT_EQ(protocol::IsValidTileCode(code), ParseTile(code).has_value()) << "tile code: " << code;
  }
}

// Builds a hand from tile codes, keeping the fixtures readable.
std::vector<Tile> HandOf(std::initializer_list<const char*> codes) {
  std::vector<Tile> hand;
  for (const char* code : codes) {
    std::optional<Tile> tile = ParseTile(code);
    EXPECT_TRUE(tile.has_value()) << "bad tile code in fixture: " << code;
    if (tile.has_value()) {
      hand.push_back(*tile);
    }
  }
  return hand;
}

// The gate must refuse a pong that makes the hand worse, even though the pong is legal.
TEST_F(RoomActorTest, AiClaimGateRejectsAPongThatWorsensTheHand) {
  const std::vector<Tile> hand =
      HandOf({"4m", "4m", "6m", "3p", "3p", "6p", "2s", "5s", "8s", "5z", "6z", "7z", "7z"});
  const Tile claimed = *ParseTile("4m");
  const std::vector<Option> options = {Option {"pong", {claimed}}, Option {"pass", {}}};

  // Fixture precondition: the pong is legal, but the shanten after the meld (and its
  // mandatory discard) is worse than today's.
  ASSERT_TRUE(CanPong(hand, claimed));
  const int before = Shanten(hand, {});
  const std::optional<int> after = ShantenAfterMeld(hand, {}, claimed, {claimed, claimed}, MeldType::kPong);
  ASSERT_TRUE(after.has_value());
  ASSERT_GT(*after, before) << "fixture must be harmful, got " << before << " -> " << *after;

  const Option answer = RoomActor::ChooseClaimAnswer(hand, {}, options, claimed);
  EXPECT_EQ(answer.action, "pass") << "an AI must not break its hand for a pong";
  EXPECT_TRUE(answer.tiles.empty());
}

// ... and accept one that improves it (the gate is not "never claim").
TEST_F(RoomActorTest, AiClaimGateAcceptsAPongThatImprovesTheHand) {
  const std::vector<Tile> hand =
      HandOf({"2m", "6m", "7m", "1p", "3p", "4p", "5p", "9p", "9p", "3s", "3s", "4s", "5s"});
  const Tile claimed = *ParseTile("9p");
  const std::vector<Option> options = {Option {"pong", {claimed}}, Option {"pass", {}}};

  const int before = Shanten(hand, {});
  const std::optional<int> after = ShantenAfterMeld(hand, {}, claimed, {claimed, claimed}, MeldType::kPong);
  ASSERT_TRUE(after.has_value());
  ASSERT_LT(*after, before) << "fixture must be beneficial, got " << before << " -> " << *after;

  const Option answer = RoomActor::ChooseClaimAnswer(hand, {}, options, claimed);
  EXPECT_EQ(answer.action, "pong");
  ASSERT_EQ(answer.tiles.size(), 1u);
  EXPECT_EQ(FaceOf(answer.tiles[0]), FaceOf(claimed));
}

// The same gate applies to every chow combination, and the best combination is the one taken.
TEST_F(RoomActorTest, AiClaimGateGatesEveryChowCombination) {
  // (a) a chow that worsens the hand is rejected
  const std::vector<Tile> harmful =
      HandOf({"4m", "4m", "5m", "5p", "5p", "8p", "9p", "9p", "2s", "1z", "4z", "4z", "5z"});
  const Tile claimed_six_m = *ParseTile("6m");
  const std::vector<Tile> harmful_pair = HandOf({"4m", "5m"});
  const int harmful_before = Shanten(harmful, {});
  const std::optional<int> harmful_after =
      ShantenAfterMeld(harmful, {}, claimed_six_m, harmful_pair, MeldType::kChow);
  ASSERT_TRUE(harmful_after.has_value());
  ASSERT_GT(*harmful_after, harmful_before) << "fixture must be harmful";
  const std::vector<Option> harmful_options = {Option {"chow", harmful_pair}, Option {"pass", {}}};
  EXPECT_EQ(RoomActor::ChooseClaimAnswer(harmful, {}, harmful_options, claimed_six_m).action, "pass");

  // (b) two non-worsening combinations: the lowest shanten wins and ties keep the frozen order
  const std::vector<Tile> tie =
      HandOf({"3m", "4m", "4m", "6m", "1p", "1p", "2p", "2p", "3p", "8s", "6z", "6z", "7z"});
  const Tile claimed_five_m = *ParseTile("5m");
  const std::vector<Tile> pair_a = HandOf({"3m", "4m"});
  const std::vector<Tile> pair_b = HandOf({"4m", "6m"});
  const int tie_before = Shanten(tie, {});
  const std::optional<int> after_a = ShantenAfterMeld(tie, {}, claimed_five_m, pair_a, MeldType::kChow);
  const std::optional<int> after_b = ShantenAfterMeld(tie, {}, claimed_five_m, pair_b, MeldType::kChow);
  ASSERT_TRUE(after_a.has_value() && after_b.has_value());
  ASSERT_LE(*after_a, tie_before);
  ASSERT_LE(*after_b, tie_before);
  const std::vector<Option> tie_options = {Option {"chow", pair_a}, Option {"chow", pair_b}, Option {"pass", {}}};
  const Option answer = RoomActor::ChooseClaimAnswer(tie, {}, tie_options, claimed_five_m);
  EXPECT_EQ(answer.action, "chow");
  ASSERT_EQ(answer.tiles.size(), pair_a.size());
  const Option& expected = (*after_a <= *after_b) ? tie_options[0] : tie_options[1];
  for (size_t i = 0; i < expected.tiles.size(); ++i) {
    EXPECT_EQ(FaceOf(answer.tiles[i]), FaceOf(expected.tiles[i]));
  }
}

// Regression for the defect t23 surfaced: an automated seat used to ignore the `win` option
// of its own discard window, so a bot holding a complete hand discarded instead of winning
// (t23's 200 games: 187 ron / 0 tsumo). The table is driven to its end here and every tsumo
// settlement is checked against the three-payer arithmetic of RULESET §6.
TEST_F(RoomActorTest, BotDeclaresTsumoWhenItsOwnDrawCompletesTheHand) {
  int tsumo_rounds = 0;
  int bot_tsumo_rounds = 0;
  int ron_rounds = 0;
  int draw_rounds = 0;
  for (uint64_t seed = 1; seed <= 60; ++seed) {
    TestRoom test_room(seed);
    test_room.Ready();
    test_room.PlayFullGame();
    ASSERT_TRUE(test_room.room()->game_finished()) << "seed " << seed << ": the match must finish";
    const std::vector<json::Value>& frames = test_room.history();

    // v1.6：一场 4..8 个 game_end（连庄各一次）；至少 4 个。
    const std::vector<const json::Value*> ends = FramesOfType(frames, "game_end");
    ASSERT_GE(ends.size(), 4u) << "seed " << seed;
    ASSERT_LE(ends.size(), 8u) << "seed " << seed << " (C15 双闸：4 局 + 4 连庄)";

    // 逐局检查；自摸局的 win 事件必须无 from_seat（PROTOCOL.md §5.8）。
    // 帧序 = 时间序：每个 game_end 的 win 事件 = 它之前最后一个 action=="win" 的事件。
    std::vector<size_t> end_positions;
    for (size_t i = 0; i < frames.size(); ++i) {
      if (TypeOf(frames[i]) == "game_end") end_positions.push_back(i);
    }
    ASSERT_EQ(end_positions.size(), ends.size());
    size_t win_cursor = 0;
    std::vector<size_t> win_positions;
    for (size_t i = 0; i < frames.size(); ++i) {
      if (TypeOf(frames[i]) == "action_event" && StrOf(frames[i], "action") == "win") {
        win_positions.push_back(i);
      }
    }
    for (size_t round = 0; round < ends.size(); ++round) {
      const json::Value* end = ends[round];
      while (win_cursor < win_positions.size() && win_positions[win_cursor] < end_positions[round]) {
        ++win_cursor;
      }
      const json::Value* last_win_event =
          win_cursor > 0 ? &frames[win_positions[win_cursor - 1]] : nullptr;
      const std::string reason = StrOf(*end, "reason");
      if (reason == "draw_exhausted") {
        ++draw_rounds;
        continue;
      }
      if (reason == "win_ron") {
        ++ron_rounds;
        continue;
      }
      ASSERT_EQ(reason, "win_tsumo") << "seed " << seed;
      ++tsumo_rounds;

      // The announced event must be a self-draw win: no from_seat (PROTOCOL.md §5.8).
      ASSERT_NE(last_win_event, nullptr) << "seed " << seed;
      EXPECT_EQ(last_win_event->Find("from_seat"), nullptr) << "a tsumo win must not carry from_seat";

      // Settlement: tsumo is the three-payer path — per_payer == 2R, total == 6R, each of the
      // other three seats pays per_payer, and the four delta_score values still sum to zero.
      const json::Value* win = end->Find("win");
      ASSERT_NE(win, nullptr);
      ASSERT_TRUE(win->IsObject()) << "seed " << seed;
      const int winner = static_cast<int>(IntOf(*win, "seat"));
      EXPECT_TRUE(win->Find("tsumo")->AsBool());
      EXPECT_TRUE(win->Find("from_seat")->IsNull());
      const int multiplier = static_cast<int>(IntOf(*win, "multiplier"));
      const int per_payer = static_cast<int>(IntOf(*win, "per_payer"));
      const int total = static_cast<int>(IntOf(*win, "total"));
      // B1 番数制：multiplier = 2^han ∈ {1,2,4,8,16,32,64}。
      EXPECT_TRUE(multiplier >= 1 && multiplier <= 64 && (multiplier & (multiplier - 1)) == 0)
          << seed << ": multiplier 必须是 2 的幂且 ∈ [1,64]";
      EXPECT_EQ(per_payer, 2 * multiplier) << seed;
      EXPECT_EQ(total, 6 * multiplier) << seed;
      const json::Value* results = end->Find("results");
      ASSERT_NE(results, nullptr);
      int64_t sum = 0;
      for (const json::Value& entry : results->AsArray()) {
        const int seat = static_cast<int>(IntOf(entry, "seat"));
        const int64_t delta = IntOf(entry, "delta_score");
        sum += delta;
        if (seat == winner) {
          EXPECT_EQ(delta, total) << seed;
        } else {
          EXPECT_EQ(delta, -per_payer) << seed;
        }
      }
      EXPECT_EQ(sum, 0) << "seed " << seed << ": tsumo must conserve the score";
      if (winner != kSeat0) {
        ++bot_tsumo_rounds;  // taken by the automated-seat branch that this task repairs
      }
    }
  }
  std::printf("tsumo rounds: %d (bot-declared: %d), ron rounds: %d, drawn rounds: %d\n", tsumo_rounds,
              bot_tsumo_rounds, ron_rounds, draw_rounds);
  EXPECT_GT(tsumo_rounds, 0) << "a self-draw win must happen now that bots read their own `win` option";
  EXPECT_GT(bot_tsumo_rounds, 0) << "the repaired automated-seat branch must be the one declaring tsumo";
}

// t27: the wall seed of a room must depend on (server seed, room index) in a way that is
// pure (reproducible) and collision-free between the rooms of one server.
TEST_F(RoomActorTest, RoomWallSeedsArePureAndPerRoom) {
  const uint64_t base = 199;  // the seed from the bug report
  EXPECT_EQ(DeriveRoomSeed(base, 0), base) << "room 0 must keep the server seed (single-room behaviour unchanged)";

  // Rooms of one server must not share a wall seed...
  std::vector<uint64_t> seeds;
  for (uint32_t index = 0; index < 8; ++index) {
    seeds.push_back(DeriveRoomSeed(base, index));
  }
  for (size_t i = 0; i < seeds.size(); ++i) {
    for (size_t j = i + 1; j < seeds.size(); ++j) {
      EXPECT_NE(seeds[i], seeds[j]) << "room indices " << i << " and " << j << " share a wall seed";
    }
  }
  // ... the mapping is a pure function (same input, same output, no clock / RNG / std::hash)...
  for (uint32_t index = 0; index < 8; ++index) {
    EXPECT_EQ(DeriveRoomSeed(base, index), seeds[index]) << "derivation must be deterministic for index " << index;
  }
  // ... and the server seed is never ignored.
  for (uint32_t index = 0; index < 8; ++index) {
    EXPECT_NE(DeriveRoomSeed(base, index), DeriveRoomSeed(base + 1, index)) << "index " << index;
    EXPECT_NE(DeriveRoomSeed(base, index), DeriveRoomSeed(0, index)) << "index " << index;
  }
}

// The observable consequence: three rooms of one server do not deal the same hand, while the
// same (base seed, index) pair always deals the same hand — that is what the e2e statistics
// rely on (fixed --seed reproduces the run byte for byte).
TEST_F(RoomActorTest, RoomsOfOneServerDealDifferentHandsAndStayReproducible) {
  const uint64_t base = 199;
  std::vector<std::vector<Tile>> first_pass;
  std::vector<std::vector<Tile>> second_pass;
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<std::vector<Tile>>& hands = (pass == 0) ? first_pass : second_pass;
    for (uint32_t index = 0; index < 3; ++index) {
      RoomSeat human;
      human.seat = kSeat0;
      human.player.player_id = "p-human";
      human.player.name = "Human";
      human.is_bot = false;
      std::vector<RoomSeat> seats;
      seats.push_back(std::move(human));
      for (int seat = 1; seat < kSeatCount; ++seat) {
        RoomSeat bot;
        bot.seat = seat;
        bot.player.player_id = "bot-" + std::to_string(seat);
        bot.player.name = "AI-" + std::to_string(seat);
        bot.is_bot = true;
        seats.push_back(std::move(bot));
      }
      // Seat 0 needs a real endpoint, or the room would treat it as an entrusted seat and play
      // its first discard for it — which is exactly what this test asserts does NOT happen.
      ex_actor::ActorRef<CapturingSeatActor> seat0 =
          std::get<0>(stdexec::sync_wait(ex_actor::Spawn<CapturingSeatActor>(std::make_shared<CapturingSink>())).value());
      seats.front().endpoint = seat0;
      // Exactly what LobbyActor does: room index -> derived wall seed.
      // Deal-only fixture: keep the pre-t31 synchronous path (ai_delay 0).
      RoomActor room("r-" + std::to_string(index + 1), DeriveRoomSeed(base, index), kDefaultTimeoutMs, std::move(seats),
                     nullptr, /*ai_delay_ms=*/0);
      // Dispatch is a coroutine (it takes the room gate), so drive it like the actor world would.
      stdexec::sync_wait(room.Dispatch(RoomEnvelope::Ready(PlayerInfo {.player_id = "p-human", .name = "Human"})));
      const std::vector<Tile>& hand = room.hand_of(kSeat0);
      ASSERT_EQ(hand.size(), 14u) << "pass " << pass << " room " << index;
      hands.push_back(SortedTiles(hand));
    }
  }
  ASSERT_EQ(first_pass.size(), 3u);
  ASSERT_EQ(second_pass.size(), 3u);

  // (1) the three rooms of one server do not deal the same hand
  for (size_t i = 0; i < first_pass.size(); ++i) {
    for (size_t j = i + 1; j < first_pass.size(); ++j) {
      EXPECT_NE(first_pass[i], first_pass[j]) << "rooms " << i + 1 << " and " << j + 1 << " dealt identical hands";
    }
  }
  // (2) same base seed + same creation order => the same wall, pass after pass
  for (size_t i = 0; i < first_pass.size(); ++i) {
    EXPECT_EQ(first_pass[i], second_pass[i]) << "room index " << i << " is not reproducible";
  }
}

// t31 (GAME_DESIGN v1.4 §5.4): a bot's own decision is paced by ai_delay_ms instead of being
// executed inside the message that opened its window.
TEST_F(RoomActorTest, AiDecisionIsPacedByAiDelayMs) {
  TestRoom test_room(20251005, kDefaultTimeoutMs, /*ai_delay_ms=*/900);
  test_room.Ready();
  std::optional<json::Value> request = test_room.PendingRequest();
  ASSERT_TRUE(request.has_value());
  ASSERT_EQ(StrOf(*request, "window"), "discard");
  EXPECT_TRUE(test_room.AiPacingScheduled().empty()) << "the human's own window is never paced";

  ASSERT_TRUE(test_room.AnswerWindow(request));
  test_room.PendingRequest();  // the human's own discard_event (plus any state_sync it triggered)
  const size_t frames_after_human = test_room.history().size();
  const uint64_t seq_after_human = test_room.room()->seq();

  // The next window belongs to a bot: its decision must be scheduled, not executed inline.
  const std::vector<std::pair<int64_t, uint64_t>> pacing = test_room.AiPacingScheduled();
  ASSERT_EQ(pacing.size(), 1u) << "exactly one paced decision must be scheduled";
  EXPECT_EQ(pacing[0].first, 900) << "the bot waits ai_delay_ms";
  EXPECT_EQ(WindowSeqOfToken(pacing[0].second), seq_after_human) << "the token carries the window seq";

  test_room.PendingRequest();
  EXPECT_EQ(test_room.history().size(), frames_after_human) << "nothing may happen before the pacing message";
  EXPECT_EQ(test_room.room()->seq(), seq_after_human);

  // Delivering it lets the bot act, exactly like the timer thread would have.
  ASSERT_TRUE(test_room.FireAiPacing());
  test_room.PendingRequest();
  EXPECT_GT(test_room.history().size(), frames_after_human) << "the bot acts once its delay elapsed";
  EXPECT_NE(test_room.room()->seq(), seq_after_human);
}

// t31: `--ai-delay-ms 0` must keep the fully synchronous decision path (the bot acts inside
// the very message that opened its window, with nothing scheduled).
TEST_F(RoomActorTest, AiDelayZeroKeepsTheSynchronousPath) {
  TestRoom test_room(20251005, kDefaultTimeoutMs, /*ai_delay_ms=*/0);
  test_room.Ready();
  std::optional<json::Value> request = test_room.PendingRequest();
  ASSERT_TRUE(request.has_value());
  ASSERT_TRUE(test_room.AnswerWindow(request));
  test_room.PendingRequest();  // everything the human's discard triggered, drained
  EXPECT_TRUE(test_room.AiPacingScheduled().empty()) << "a zero delay must not schedule anything";
  // The bots decided inline: the room ran on until it needed the human again, so at least one
  // bot has already discarded (or the round already ended) without any scheduled message.
  int bot_discards = 0;
  for (int seat = 1; seat < kSeatCount; ++seat) {
    bot_discards += test_room.DiscardsBy(seat);
  }
  EXPECT_TRUE(test_room.room()->game_finished() || bot_discards > 0)
      << "with a zero delay the bots play inside the human's own message";
}

// t31: while the bots' claim answers are still pending, a human win must settle at once — the
// top claim priority cannot be changed by any later bot answer, so the human never waits out
// the pacing (GAME_DESIGN v1.4 §5.4 "人类合法回复优先立即生效").
TEST_F(RoomActorTest, HumanWinDuringTheAiDelayResolvesImmediately) {
  // Seed 49 reaches the scenario deterministically (the human may ron a discard that a bot is
  // also a claim recipient of, so a paced bot answer really is outstanding). The sweep behind
  // it keeps the test from depending on that single magic seed.
  std::vector<uint64_t> seeds = {49};
  for (uint64_t seed = 1; seed <= 120; ++seed) {
    seeds.push_back(seed);
  }
  for (uint64_t seed : seeds) {
    TestRoom test_room(seed, kDefaultTimeoutMs, /*ai_delay_ms=*/900, /*smart_discard=*/true);
    test_room.Ready();
    int steps = 0;
    while (!test_room.room()->game_finished() && steps < 4000) {
      ++steps;
      std::optional<json::Value> request = test_room.PendingRequest();
      const bool human_ron = request.has_value() && StrOf(*request, "window") == "claim" &&
                             OptionOf(*request, "win") != nullptr && !test_room.AiPacingScheduled().empty();
      if (human_ron) {
        const std::vector<std::pair<int64_t, uint64_t>> pacing = test_room.AiPacingScheduled();
        ASSERT_EQ(pacing.size(), 1u);
        EXPECT_EQ(pacing[0].first, 900);
        const uint64_t in_flight = test_room.TakeAiPacing();  // keep it, do not deliver it yet
        ASSERT_NE(in_flight, 0u);
        ASSERT_TRUE(test_room.AnswerWindow(request));
        // The win must be settled without ever delivering that pacing message.
        // v1.6：荣和结算的是**局终**（game_end），不是场终。
        ASSERT_TRUE(test_room.room()->round_finished()) << "a human win must not wait for the AI pacing";
        test_room.PendingRequest();
        const std::vector<const json::Value*> ends = FramesOfType(test_room.history(), "game_end");
        ASSERT_EQ(ends.size(), 1u);
        EXPECT_EQ(StrOf(*ends.front(), "reason"), "win_ron");
        const json::Value* results = ends.front()->Find("results");
        ASSERT_NE(results, nullptr);
        int64_t delta_sum = 0;
        for (const json::Value& entry : results->AsArray()) {
          delta_sum += IntOf(entry, "delta_score");
        }
        EXPECT_EQ(delta_sum, 0) << "the ron settlement must conserve the score";
        // The pacing message that was in flight is stale now: delivering it changes nothing.
        const uint64_t seq_before = test_room.room()->seq();
        const size_t frames_before = test_room.history().size();
        test_room.Send(RoomEnvelope::WindowTimeout(in_flight));
        EXPECT_EQ(test_room.room()->seq(), seq_before) << "a stale pacing message must be dropped";
        EXPECT_EQ(test_room.history().size(), frames_before);
        std::printf("human win during the AI delay: seed %llu\n", static_cast<unsigned long long>(seed));
        return;
      }
      if (!test_room.AnswerWindow(request) && !test_room.FireAiPacing() && !test_room.AdvanceOpenWindow()) {
        break;
      }
    }
  }
  FAIL() << "no human ron window with a pending paced bot claim was found";
}

// t31: the pacing is for AI decisions only. A timed-out window and an entrusted seat both
// represent "already waited", so they act immediately even when ai_delay_ms > 0.
TEST_F(RoomActorTest, TimeoutAndEntrustedStayImmediateUnderAiDelay) {
  {
    TestRoom test_room(20251005, kDefaultTimeoutMs, /*ai_delay_ms=*/900);
    test_room.Ready();
    test_room.PendingRequest();
    const int before = test_room.DiscardsBy(kSeat0);
    const uint64_t acted_window = test_room.room()->seq();
    test_room.Send(RoomEnvelope::PlayerDisconnected(test_room.human()));
    test_room.PendingRequest();
    EXPECT_EQ(test_room.DiscardsBy(kSeat0), before + 1) << "an entrusted seat discards immediately";
    for (const std::pair<int64_t, uint64_t>& entry : test_room.AiPacingScheduled()) {
      EXPECT_NE(WindowSeqOfToken(entry.second), acted_window)
          << "the entrusted seat's own window must not be paced (a later bot window may be)";
    }
  }
  {
    TestRoom test_room(20251005, kDefaultTimeoutMs, /*ai_delay_ms=*/900);
    test_room.Ready();
    std::optional<json::Value> request = test_room.PendingRequest();
    ASSERT_TRUE(request.has_value());
    const uint64_t window_seq = test_room.room()->seq();
    const int before = test_room.DiscardsBy(kSeat0);
    // The plain window deadline (untagged token), exactly as OpenWindowTimer() schedules it.
    test_room.Send(RoomEnvelope::WindowTimeout(window_seq));
    test_room.PendingRequest();
    EXPECT_EQ(test_room.DiscardsBy(kSeat0), before + 1) << "the window deadline still plays the default action";
    for (const std::pair<int64_t, uint64_t>& entry : test_room.AiPacingScheduled()) {
      EXPECT_NE(WindowSeqOfToken(entry.second), window_seq) << "a deadline must not become a paced decision";
    }
  }
}

// t31: with AI pacing enabled every round must still converge — the state machine may only
// wait for a scheduled pacing message, never for a reply that will not come. This drives whole
// games at ai_delay_ms = 900 (the fixture delivers the scheduled message, exactly like the
// timer thread) and re-checks the settlement invariants.
TEST_F(RoomActorTest, PacedGamesAlwaysTerminateAndSettleCorrectly) {
  int finished = 0;
  for (uint64_t seed = 1; seed <= 30; ++seed) {
    TestRoom test_room(seed, kDefaultTimeoutMs, /*ai_delay_ms=*/900, /*smart_discard=*/true);
    test_room.Ready();
    const int steps = test_room.PlayFullGame();
    ASSERT_TRUE(test_room.room()->game_finished()) << "seed " << seed << " stalled after " << steps << " steps";
    ++finished;
    // v1.6：一场 = 4..8 局；game_end 次数 = round_index 终值（计入局数）+ 连庄数
    // （C15 的连庄闸可能让本场不足 4 局即结束，故不能用恒 4）。
    const std::vector<const json::Value*> ends = FramesOfType(test_room.history(), "game_end");
    ASSERT_GE(ends.size(), 4u) << "seed " << seed;
    ASSERT_LE(ends.size(), 8u) << "seed " << seed;
    const int renchan = static_cast<int>(IntOf(*ends.back(), "renchan_count"));
    ASSERT_EQ(ends.size(), test_room.room()->round_index() + renchan)
        << "seed " << seed << ": game_end 次数必须 = 计入局数 + 连庄数";
    for (const json::Value* end : ends) {
      const json::Value* results = end->Find("results");
      ASSERT_NE(results, nullptr);
      int64_t delta_sum = 0;
      for (const json::Value& entry : results->AsArray()) {
        delta_sum += IntOf(entry, "delta_score");
      }
      EXPECT_EQ(delta_sum, 0) << "seed " << seed << ": the settlement must conserve the score";
    }
    // 场终帧（§5.15）恰好一次；hands_played = game_end 下发手数（D2 终裁：平凡恒等，4..8）。
    const std::vector<const json::Value*> match_ends = FramesOfType(test_room.history(), "match_end");
    ASSERT_EQ(match_ends.size(), 1u) << "seed " << seed;
    EXPECT_EQ(IntOf(*match_ends.front(), "hands_played"), ends.size());
    EXPECT_GE(IntOf(*match_ends.front(), "hands_played"), 4);
    // Every paced decision must have been consumed — or, if the round ended under it (a human
    // win resolves ahead of paced bot answers), the leftover token must be stale, i.e. dropped
    // by the first criterion on delivery. A *live* decision may never be left waiting.
    for (const std::pair<int64_t, uint64_t>& entry : test_room.AiPacingScheduled()) {
      EXPECT_NE(WindowSeqOfToken(entry.second), test_room.room()->seq())
          << "seed " << seed << ": 遗留的 AI pacing 令牌必须是过期的";
    }
    EXPECT_FALSE(test_room.RoundAdvanceScheduled()) << "seed " << seed;
  }
  std::printf("paced matches finished: %d\n", finished);
}

// t31: a paced decision is only ever *waited for* when its message was really armed. Without
// a scheduler the room must fall back to deciding inline instead of stalling on a reply that
// can never arrive (the only "wait forever" branch this change could have introduced).
TEST_F(RoomActorTest, PacingWithoutASchedulerNeverStalls) {
  TestRoom test_room(20251005, kDefaultTimeoutMs, /*ai_delay_ms=*/900, /*smart_discard=*/false,
                     /*install_scheduler=*/false);
  test_room.Ready();
  const int steps = test_room.PlayFullGame();
  EXPECT_TRUE(test_room.room()->game_finished()) << "stalled after " << steps << " steps";
  EXPECT_TRUE(test_room.AiPacingScheduled().empty());
}

TEST_F(RoomActorTest, GameStartOpensTheDealerDiscardWindow) {
  TestRoom test_room(20251005);
  test_room.Ready();

  std::vector<json::Value> frames = test_room.sink()->Drain();
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(TypeOf(frames[0]), "game_start");
  EXPECT_EQ(IntOf(frames[0], "seat"), 0);
  EXPECT_EQ(IntOf(frames[0], "dealer"), 0);
  // v1.6（C9）：game_start 带场次进度
  EXPECT_EQ(IntOf(frames[0], "round_index"), 0);
  EXPECT_EQ(IntOf(frames[0], "round_count"), 4);
  const json::Value* hand = frames[0].Find("hand");
  ASSERT_NE(hand, nullptr);
  EXPECT_EQ(hand->AsArray().size(), 14u);  // the dealer starts with 14 tiles

  EXPECT_EQ(TypeOf(frames[1]), "action_request");
  EXPECT_EQ(IntOf(frames[1], "seq"), 1);  // the first window is seq 1 (GAME_DESIGN §4.2)
  EXPECT_EQ(StrOf(frames[1], "window"), "discard");
  EXPECT_GT(IntOf(frames[1], "deadline_ms"), 0);

  EXPECT_EQ(test_room.room()->seq(), 1u);
  EXPECT_EQ(test_room.room()->current_seat(), 0);
  EXPECT_EQ(test_room.room()->round_index(), 0);
  // Derived from the rules library, so it follows a tile-set change (108 -> 136) automatically.
  EXPECT_EQ(test_room.room()->remaining(), kWallSizeAfterDeal);
  EXPECT_EQ(test_room.room()->CurrentWindow(), "discard");
  EXPECT_EQ(CountOfType(frames, "state_sync"), 0) << "dealing is not a draw; no state_sync yet (§5.12)";
  ASSERT_EQ(test_room.scheduled().size(), 1u);
  EXPECT_EQ(test_room.scheduled()[0].first, kDefaultTimeoutMs);
  EXPECT_EQ(test_room.scheduled()[0].second, 1u);
}

TEST_F(RoomActorTest, DiscardEventCarriesTheClosedWindowSeq) {
  TestRoom test_room(20251005);
  test_room.Ready();
  test_room.sink()->Drain();

  const std::vector<Tile> hand = SortedTiles(test_room.room()->hand_of(kSeat0));
  const Tile discarded = hand.back();
  test_room.SendDiscard(discarded, 1);

  // The discard is acknowledged first, and it carries the seq of the window it closed.
  // (Whatever the bots do afterwards happens inside this same call; every later frame must
  // therefore live in a newer window.)
  std::vector<json::Value> frames = test_room.sink()->Drain();
  ASSERT_FALSE(frames.empty());
  EXPECT_EQ(TypeOf(frames[0]), "discard_event");
  EXPECT_EQ(IntOf(frames[0], "seat"), 0);
  EXPECT_EQ(StrOf(frames[0], "tile"), FormatTile(discarded));
  EXPECT_EQ(IntOf(frames[0], "seq"), 1);
  EXPECT_GT(test_room.room()->seq(), 1u);
  for (size_t i = 1; i < frames.size(); ++i) {
    const json::Value* seq = frames[i].Find("seq");
    if (seq != nullptr && seq->IsNumber()) {
      EXPECT_GE(seq->AsInt(), 2) << "frame " << i << " still belongs to the closed window: " << frames[i].Dump();
    }
  }
  // The discarded tile left the hand; it sits in the river unless a bot claimed it.
  EXPECT_LE(test_room.room()->hand_of(kSeat0).size(), 14u);
  EXPECT_LE(test_room.room()->discards_of(kSeat0).size(), 1u);
}

TEST_F(RoomActorTest, StaleSequencesAreDroppedSilently) {
  TestRoom test_room(20251005);
  test_room.Ready();
  test_room.sink()->Drain();

  const std::vector<Tile> hand_before = test_room.room()->hand_of(kSeat0);

  test_room.SendDiscard(SortedTiles(hand_before).back(), 0);    // seq 0: never valid
  test_room.SendDiscard(SortedTiles(hand_before).back(), 99);   // seq far in the future
  test_room.SendAction("pass", {}, 99);

  std::vector<json::Value> frames = test_room.sink()->Drain();
  EXPECT_TRUE(frames.empty()) << "a stale seq must not produce any frame (PROTOCOL.md §6.3)";
  EXPECT_EQ(test_room.room()->seq(), 1u);
  EXPECT_EQ(test_room.room()->discards_of(kSeat0).size(), 0u);
  EXPECT_EQ(test_room.room()->hand_of(kSeat0).size(), 14u);
  EXPECT_EQ(test_room.room()->CurrentWindow(), "discard");

  // The same reply with the current seq works, and a repeat of it is stale again.
  test_room.SendDiscard(SortedTiles(hand_before).back(), 1);
  test_room.sink()->Drain();
  const uint64_t seq_after = test_room.room()->seq();
  const size_t discards_after = test_room.room()->discards_of(kSeat0).size();
  test_room.SendDiscard(SortedTiles(hand_before).back(), 1);
  EXPECT_TRUE(test_room.sink()->Drain().empty());
  EXPECT_EQ(test_room.room()->seq(), seq_after);
  EXPECT_EQ(test_room.room()->discards_of(kSeat0).size(), discards_after);
}

TEST_F(RoomActorTest, IllegalOperationsChangeNothing) {
  TestRoom test_room(20251005);
  test_room.Ready();
  std::vector<json::Value> start_frames = test_room.sink()->Drain();
  const json::Value& request = start_frames[1];
  ASSERT_EQ(StrOf(request, "window"), "discard");
  if (OptionOf(request, "win") != nullptr) {
    GTEST_SKIP() << "this seed deals the dealer a winning hand; pick another seed";
  }

  const std::vector<Tile> hand = test_room.room()->hand_of(kSeat0);

  // H1: `pass` is not a legal reply in a discard window.
  test_room.SendAction("pass", {}, 1);
  std::vector<json::Value> frames = test_room.sink()->Drain();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(TypeOf(frames[0]), "error");
  EXPECT_EQ(StrOf(frames[0], "code"), "ILLEGAL_ACTION");

  // A fake win request (the hand is not a winner) is rejected too (H8).
  test_room.SendAction("win", {}, 1);
  frames = test_room.sink()->Drain();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(StrOf(frames[0], "code"), "ILLEGAL_ACTION");

  // A tile that is not in hand.
  const FaceCounts counts = CountFaces(hand);
  Face absent = 0;
  for (Face face = 0; face < kFaceCount; ++face) {
    if (counts[face] == 0) {
      absent = face;
      break;
    }
  }
  test_room.SendDiscard(MakeTileFromFace(absent), 1);
  frames = test_room.sink()->Drain();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(StrOf(frames[0], "code"), "UNKNOWN_TILE");

  // None of the rejected frames changed the state.
  EXPECT_EQ(test_room.room()->seq(), 1u);
  EXPECT_EQ(test_room.room()->current_seat(), 0);
  EXPECT_EQ(test_room.room()->CurrentWindow(), "discard");
  EXPECT_EQ(test_room.room()->discards_of(kSeat0).size(), 0u);
  EXPECT_EQ(test_room.room()->hand_of(kSeat0), hand);
}

// Runs the timeout scenario for one shuffle. It is a helper (not a fixture of one seed)
// because the wall width decides which windows show up on the way to the human's second
// turn: the 55-tile wall never reached a claim window here, the 83-tile wall does.
void RunTimeoutScenario(uint64_t seed) {
  TestRoom test_room(seed);
  test_room.Ready();
  test_room.sink()->Drain();

  // Dealer's first discard window: no draw record, so the default is the largest tile.
  const std::vector<Tile> hand = SortedTiles(test_room.room()->hand_of(kSeat0));
  const Tile expected_first = hand.back();
  test_room.Send(RoomEnvelope::WindowTimeout(1));

  std::vector<json::Value> frames = test_room.sink()->Drain();
  ASSERT_FALSE(frames.empty());
  EXPECT_EQ(TypeOf(frames[0]), "discard_event");
  EXPECT_EQ(IntOf(frames[0], "seat"), 0);
  EXPECT_EQ(StrOf(frames[0], "tile"), FormatTile(expected_first));

  // The timeout already consumed window 1: a second delivery of it is a no-op.
  const uint64_t seq_now = test_room.room()->seq();
  const size_t discards_now = test_room.room()->discards_of(kSeat0).size();
  test_room.Send(RoomEnvelope::WindowTimeout(1));
  EXPECT_TRUE(test_room.sink()->Drain().empty());
  EXPECT_EQ(test_room.room()->seq(), seq_now);
  EXPECT_EQ(test_room.room()->discards_of(kSeat0).size(), discards_now);

  // Drive the room until the human holds a discard window with a draw record; the default
  // discard is then exactly the tile just drawn (GAME_DESIGN §5.6.2).
  // Exactly one drain per step, and the target state is checked before answering it:
  //   * the human answers whatever window is addressed to them (discard or claim), and
  //   * when no window is addressed to them the fixture expires the open window instead of
  //     giving up, so the loop always advances the game (a claim window for the human used
  //     to be drained by the loop and then reported as "nothing to answer", which stopped
  //     the fixture before the human ever drew again).
  std::optional<Tile> drawn;
  for (int guard = 0; guard < 4000 && !test_room.room()->game_finished(); ++guard) {
    std::optional<json::Value> request = test_room.PendingRequest();
    if (test_room.room()->CurrentWindow() == "discard" && test_room.room()->current_seat() == kSeat0 &&
        test_room.drawn().has_value()) {
      drawn = test_room.drawn();
      break;
    }
    if (test_room.AnswerWindow(request)) {
      continue;
    }
    if (!test_room.AdvanceOpenWindow()) {
      break;
    }
  }
  ASSERT_TRUE(drawn.has_value()) << "the human never got a draw record in an open window (seed " << seed << ")";
  const uint64_t window_seq = test_room.room()->seq();
  test_room.Send(RoomEnvelope::WindowTimeout(window_seq));
  frames = test_room.sink()->Drain();
  ASSERT_FALSE(frames.empty());
  EXPECT_EQ(TypeOf(frames[0]), "discard_event");
  EXPECT_EQ(StrOf(frames[0], "tile"), FormatTile(*drawn));
}

TEST_F(RoomActorTest, TimeoutPlaysTheDefaultActionAndStaleTimeoutsAreIgnored) {
  // Three independent shuffles: the scenario must not depend on one particular wall.
  for (uint64_t seed : {static_cast<uint64_t>(20251005), static_cast<uint64_t>(7), static_cast<uint64_t>(1234567)}) {
    RunTimeoutScenario(seed);
  }
}

// v1.6（C6 专项）：局终后，旧窗口令牌与推进令牌的判据顺序与单次推进语义。
TEST_F(RoomActorTest, RoundAdvanceTokenIsTheOnlyWayAcrossRounds) {
  TestRoom test_room(20251005);
  test_room.Ready();
  test_room.sink()->Drain();

  // 打满一局（不交付推进令牌）
  ASSERT_TRUE(test_room.PlayFullRound()) << "the first round must reach its end";
  ASSERT_TRUE(test_room.room()->round_finished());
  ASSERT_FALSE(test_room.room()->game_finished());
  ASSERT_TRUE(test_room.RoundAdvanceScheduled()) << "局终必须武装推进令牌";
  const uint64_t end_seq = test_room.room()->seq();
  const int round0_dealer = test_room.room()->dealer();

  // ① 旧窗口令牌（上一局最后窗口的 seq、无标签）→ 零状态变更
  const auto snapshot = [&]() {
    return std::make_tuple(test_room.room()->seq(), test_room.room()->round_index(), test_room.room()->dealer(),
                           test_room.room()->CurrentWindow(), test_room.history().size());
  };
  const auto before = snapshot();
  test_room.Send(RoomEnvelope::WindowTimeout(end_seq - 1));
  test_room.sink()->Drain();
  EXPECT_EQ(snapshot(), before) << "旧窗口令牌在局终后必须被第一道判据丢弃";
  // （推进令牌未被消费，还在）
  ASSERT_TRUE(test_room.RoundAdvanceScheduled());

  // ② 推进令牌 → **恰好**推进一局
  const uint64_t token = test_room.TakeRoundAdvance();
  ASSERT_NE(token, 0u);
  EXPECT_EQ(WindowSeqOfToken(token), end_seq) << "推进令牌必须带局终后的新 seq（arm 在 Transition 之后）";
  test_room.Send(RoomEnvelope::WindowTimeout(token));
  std::vector<json::Value> frames = test_room.sink()->Drain();
  EXPECT_EQ(test_room.room()->round_index(), 1) << "round_index 必须恰好 +1";
  EXPECT_GT(test_room.room()->seq(), end_seq) << "新局的窗口 seq 继续单调递增";
  EXPECT_EQ(test_room.room()->CurrentWindow(), "discard");
  EXPECT_FALSE(test_room.room()->round_finished());
  // 新局的 game_start 帧带 round_index=1，且手牌 14 张的座位 = 新庄家
  const std::vector<const json::Value*> starts = FramesOfType(frames, "game_start");
  ASSERT_EQ(starts.size(), 1u);
  EXPECT_EQ(IntOf(*starts.front(), "round_index"), 1);
  EXPECT_EQ(IntOf(*starts.front(), "round_count"), 4);
  EXPECT_EQ(IntOf(*starts.front(), "dealer"), test_room.room()->dealer());
  // 连庄/轮庄至少产生了正确结果之一：dealer 不变（连庄）或 +1（轮庄）
  const int new_dealer = test_room.room()->dealer();
  EXPECT_TRUE(new_dealer == round0_dealer || new_dealer == (round0_dealer + 1) % kSeatCount);
  // 交给新局后座位 0 的手牌数 = 新庄家是否为 0 座
  const json::Value* hand = starts.front()->Find("hand");
  ASSERT_NE(hand, nullptr);
  EXPECT_EQ(hand->AsArray().size(), new_dealer == 0 ? 14u : 13u);

  // ③ 同一令牌再投一次 → 零状态变更（seq 已前移，第一道判据丢弃）
  const auto after = snapshot();
  test_room.Send(RoomEnvelope::WindowTimeout(token));
  test_room.sink()->Drain();
  EXPECT_EQ(snapshot(), after) << "推进令牌不得重复推进";
}

TEST_F(RoomActorTest, EntrustedSeatPlaysImmediately) {
  TestRoom test_room(20251005);
  test_room.Ready();
  test_room.sink()->Drain();

  const std::vector<Tile> hand = SortedTiles(test_room.room()->hand_of(kSeat0));
  test_room.Send(RoomEnvelope::PlayerDisconnected(test_room.human()));

  std::vector<json::Value> frames = test_room.sink()->Drain();
  ASSERT_FALSE(frames.empty());
  EXPECT_EQ(TypeOf(frames[0]), "discard_event");
  EXPECT_EQ(IntOf(frames[0], "seat"), 0);
  EXPECT_EQ(StrOf(frames[0], "tile"), FormatTile(hand.back()));

  // The entrusted seat is reported in the per-seat public view (PROTOCOL.md §8.5).
  const std::vector<const json::Value*> syncs = FramesOfType(frames, "state_sync");
  ASSERT_FALSE(syncs.empty());
  const json::Value* seats = syncs.front()->Find("seats");
  ASSERT_NE(seats, nullptr);
  ASSERT_EQ(seats->AsArray().size(), 4u);
  EXPECT_TRUE(seats->AsArray()[0].Find("disconnected")->AsBool());
}

TEST_F(RoomActorTest, FullGamesSatisfyProtocolInvariants) {
  int wins = 0;
  int draws = 0;
  int rounds = 0;
  int matches = 0;
  std::set<int> dealers_seen;
  for (uint64_t seed = 1; seed <= 40; ++seed) {
    TestRoom test_room(seed);
    test_room.Ready();
    const int answered = test_room.PlayFullGame();
    ASSERT_TRUE(test_room.room()->game_finished()) << "seed " << seed << " never finished (" << answered << " windows)";
    ++matches;

    const std::vector<json::Value>& frames = test_room.history();
    std::vector<const json::Value*> errors = FramesOfType(frames, "error");
    ASSERT_TRUE(errors.empty()) << "seed " << seed << " produced an error frame: "
                                << errors.front()->Dump();

    // ---- 帧计数（§5.10 v1.6 / §5.15）：game_start 每局恰一次、game_end = 4 + 连庄、match_end 恰 1 次
    const std::vector<const json::Value*> starts = FramesOfType(frames, "game_start");
    const std::vector<const json::Value*> ends = FramesOfType(frames, "game_end");
    const std::vector<const json::Value*> match_ends = FramesOfType(frames, "match_end");
    ASSERT_EQ(starts.size(), ends.size()) << "seed " << seed << ": 每局必须恰有一个 game_start";
    ASSERT_GE(ends.size(), 4u) << "seed " << seed;
    ASSERT_LE(ends.size(), 8u) << "seed " << seed << "（C15 双闸）";
    ASSERT_EQ(match_ends.size(), 1u) << "seed " << seed;
    // D2 终裁：hands_played = game_end 下发手数（平凡恒等，4..8；连庄由 renchan 单独承载）。
    EXPECT_EQ(IntOf(*match_ends.front(), "hands_played"), ends.size()) << "seed " << seed;
    EXPECT_FALSE(ends.back()->Find("match_will_continue")->AsBool()) << "最后一份 game_end 必须宣布本场结束";
    for (size_t i = 0; i + 1 < ends.size(); ++i) {
      EXPECT_TRUE(ends[i]->Find("match_will_continue")->AsBool()) << "seed " << seed << " round " << i;
    }

    // ---- state_sync 不变量（H6 + 张数公式 + 他家暗杠打码），逐帧成立
    std::vector<size_t> end_positions;
    for (size_t i = 0; i < frames.size(); ++i) {
      if (TypeOf(frames[i]) == "game_end") end_positions.push_back(i);
    }
    size_t next_end = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
      const json::Value& sync = frames[i];
      if (TypeOf(sync) != "state_sync") continue;
      while (next_end < end_positions.size() && end_positions[next_end] < i) ++next_end;
      const bool is_round_final =
          next_end < end_positions.size() &&
          [&] {  // 这份 state_sync 是 end_positions[next_end] 之前最后一份吗？
            for (size_t j = i + 1; j < end_positions[next_end]; ++j) {
              if (TypeOf(frames[j]) == "state_sync") return false;
            }
            return true;
          }();
      const json::Value* seats = sync.Find("seats");
      ASSERT_NE(seats, nullptr);
      ASSERT_EQ(seats->AsArray().size(), 4u);
      for (const json::Value& entry : seats->AsArray()) {
        EXPECT_EQ(entry.Find("hand"), nullptr) << "seats[] must not leak hands (H6)";
        const int hand_count = static_cast<int>(IntOf(entry, "hand_count"));
        const json::Value* melds = entry.Find("melds");
        ASSERT_NE(melds, nullptr);
        const int concealed = hand_count + 3 * static_cast<int>(melds->AsArray().size());
        EXPECT_TRUE(concealed == 13 || concealed == 14) << "seed " << seed << ": hand_count=" << hand_count
                                                        << " melds=" << melds->AsArray().size();
        // §5.9 暗杠三态：非局终的 state_sync 里，他家 ankan.tiles 恒 []；局终揭示版为 4 张同面。
        const int seat = static_cast<int>(IntOf(entry, "seat"));
        for (const json::Value& meld : melds->AsArray()) {
          if (StrOf(meld, "type") != "ankan") continue;
          const json::Value* tiles = meld.Find("tiles");
          ASSERT_NE(tiles, nullptr);
          if (seat != kSeat0 && !is_round_final) {
            EXPECT_EQ(tiles->AsArray().size(), 0u)
                << "seed " << seed << ": 他家的 ankan.tiles 必须恒为空数组（frame " << i << "）";
          } else {
            ASSERT_EQ(tiles->AsArray().size(), 4u) << "seed " << seed << " frame " << i;
            const std::string face = tiles->AsArray().front().AsString();
            for (const json::Value& tile : tiles->AsArray()) {
              EXPECT_EQ(tile.AsString(), face) << "ankan 必须是 4 张同面";
            }
          }
        }
      }
    }

    // ---- 牌数守恒：hands + melds + rivers + wall pool == kTileCount（荣和的河中牌可 +1，H4）。
    const int in_play = [&] {
      int total = test_room.room()->remaining();
      for (int seat = 0; seat < kSeatCount; ++seat) {
        total += static_cast<int>(test_room.room()->hand_of(seat).size());
        total += static_cast<int>(FlattenMelds(test_room.room()->melds_of(seat)).size());
        total += static_cast<int>(test_room.room()->discards_of(seat).size());
      }
      return total;
    }();
    EXPECT_TRUE(in_play == kTileCount || in_play == kTileCount + 1)
        << "seed " << seed << ": " << in_play << " tiles accounted for";

    // ---- seq 整场严格递增（C3：跨局单调不重置，§6.2）
    uint64_t previous_seq = 0;
    for (const json::Value* request : FramesOfType(frames, "action_request")) {
      const uint64_t seq = static_cast<uint64_t>(IntOf(*request, "seq"));
      EXPECT_GT(seq, previous_seq) << "seed " << seed;
      previous_seq = seq;
    }

    // ---- 逐局：dealer 轮转（C9）、delta 零和、win 字段一致性、match_total 累计
    int expected_round_index = 0;
    int expected_dealer = 0;
    std::array<int64_t, kSeatCount> running_totals {};
    for (size_t round = 0; round < ends.size(); ++round) {
      const json::Value& start = *starts[round];
      const json::Value& end = *ends[round];
      ++rounds;
      // C9：每局的 dealer 与连庄/轮庄推导一致；庄家的 game_start.hand 是 14 张（座位 0 可见时）。
      EXPECT_EQ(IntOf(start, "round_index"), expected_round_index) << "seed " << seed << " round " << round;
      EXPECT_EQ(IntOf(start, "round_count"), 4);
      EXPECT_EQ(IntOf(start, "dealer"), expected_dealer) << "seed " << seed << " round " << round;
      dealers_seen.insert(expected_dealer);
      if (expected_dealer == kSeat0) {
        const json::Value* hand = start.Find("hand");
        ASSERT_NE(hand, nullptr);
        EXPECT_EQ(hand->AsArray().size(), 14u) << "seed " << seed << " round " << round << ": 庄家必须 14 张";
      }
      EXPECT_EQ(IntOf(end, "round_index"), expected_round_index);
      EXPECT_EQ(IntOf(end, "dealer"), expected_dealer);

      const std::string reason = StrOf(end, "reason");
      ASSERT_TRUE(reason == "win_ron" || reason == "win_tsumo" || reason == "draw_exhausted");
      const int round_dealer = expected_dealer;  // 本局庄家（推进前捕获）
      // 连庄/轮庄（§3.3）：庄家和牌或荒庄 ⇒ 连庄；闲家和牌 ⇒ 轮庄。
      const bool dealer_won =
          reason != "draw_exhausted" && static_cast<int>(IntOf(*end.Find("win"), "seat")) == round_dealer;
      if (reason != "draw_exhausted" && !dealer_won) {
        ++expected_round_index;
        expected_dealer = (expected_dealer + 1) % kSeatCount;
      }

      const json::Value* results = end.Find("results");
      ASSERT_NE(results, nullptr);
      ASSERT_EQ(results->AsArray().size(), 4u);
      int64_t sum = 0;
      int64_t total_sum = 0;
      std::array<bool, kSeatCount> seen {};
      for (const json::Value& entry : results->AsArray()) {
        const int seat = static_cast<int>(IntOf(entry, "seat"));
        ASSERT_GE(seat, 0);
        ASSERT_LT(seat, kSeatCount);
        EXPECT_FALSE(seen[static_cast<size_t>(seat)]) << "duplicate seat " << seat;
        seen[static_cast<size_t>(seat)] = true;
        sum += IntOf(entry, "delta_score");
        running_totals[static_cast<size_t>(seat)] += IntOf(entry, "delta_score");
        // match_total = 本场累计（含本局），首局与 delta_score 相等，之后逐局分化（§5.10）
        EXPECT_EQ(IntOf(entry, "match_total"), running_totals[static_cast<size_t>(seat)]) << "seed " << seed;
        total_sum += IntOf(entry, "match_total");
      }
      EXPECT_EQ(sum, 0) << "seed " << seed << " round " << round << ": delta_score must sum to zero";
      EXPECT_EQ(total_sum, 0) << "seed " << seed << " round " << round << ": match_total must sum to zero";

      const json::Value* win = end.Find("win");
      ASSERT_NE(win, nullptr);
      if (reason == "draw_exhausted") {
        ++draws;
        EXPECT_TRUE(win->IsNull());
        for (const json::Value& entry : results->AsArray()) {
          EXPECT_EQ(IntOf(entry, "delta_score"), 0);
        }
        continue;
      }
      ++wins;
      ASSERT_TRUE(win->IsObject()) << "seed " << seed;
      const int winner = static_cast<int>(IntOf(*win, "seat"));
      const bool tsumo = win->Find("tsumo") != nullptr && win->Find("tsumo")->AsBool();
      EXPECT_EQ(tsumo, reason == "win_tsumo");
      const int multiplier = static_cast<int>(IntOf(*win, "multiplier"));
      const int per_payer = static_cast<int>(IntOf(*win, "per_payer"));
      const int total = static_cast<int>(IntOf(*win, "total"));
      EXPECT_TRUE(multiplier >= 1 && multiplier <= 64 && (multiplier & (multiplier - 1)) == 0);
      EXPECT_EQ(per_payer, tsumo ? 2 * multiplier : multiplier);
      EXPECT_EQ(total, tsumo ? 6 * multiplier : multiplier);
      EXPECT_EQ(IntOf(results->AsArray()[static_cast<size_t>(winner)], "delta_score"), total);
      if (tsumo) {
        for (const json::Value& entry : results->AsArray()) {
          if (static_cast<int>(IntOf(entry, "seat")) != winner) {
            EXPECT_EQ(IntOf(entry, "delta_score"), -per_payer);
          }
        }
        EXPECT_TRUE(win->Find("from_seat")->IsNull());
      } else {
        const int from_seat = static_cast<int>(IntOf(*win, "from_seat"));
        EXPECT_EQ(IntOf(results->AsArray()[static_cast<size_t>(from_seat)], "delta_score"), -total);
      }
      // v1.6 番数明细：yaku/han_breakdown/han/dealer_bonus/rinshan 的自洽（§5.10）
      const json::Value* yaku = win->Find("yaku");
      const json::Value* breakdown = win->Find("han_breakdown");
      ASSERT_NE(yaku, nullptr);
      ASSERT_NE(breakdown, nullptr);
      int base = 0;
      std::vector<std::string> yaku_ids;
      for (const json::Value& item : yaku->AsArray()) {
        yaku_ids.push_back(StrOf(item, "id"));
        base += static_cast<int>(IntOf(item, "han"));
      }
      EXPECT_EQ(static_cast<int>(IntOf(*breakdown, "base")), base);
      EXPECT_EQ(static_cast<int>(IntOf(*breakdown, "tsumo")), 0) << "B1.1：自摸不是番种，tsumo 恒 0";
      const int dealer_bonus = static_cast<int>(IntOf(*breakdown, "dealer"));
      const int total_han = static_cast<int>(IntOf(*breakdown, "total"));
      EXPECT_EQ(dealer_bonus, winner == round_dealer ? 1 : 0) << "庄家加成按 winner==dealer";
      EXPECT_EQ(total_han, std::min(base + dealer_bonus, 6));
      EXPECT_EQ(static_cast<int>(IntOf(*win, "han")), total_han);
      EXPECT_EQ(multiplier, 1 << total_han);
      EXPECT_EQ(win->Find("dealer_bonus")->AsBool(), dealer_bonus == 1);
      // tags ⊆ yaku id（tags 保持旧语义）
      std::vector<std::string> tags;
      for (const json::Value& tag : win->Find("tags")->AsArray()) tags.push_back(tag.AsString());
      for (const std::string& tag : tags) {
        EXPECT_NE(std::find(yaku_ids.begin(), yaku_ids.end(), tag), yaku_ids.end()) << "tags ⊆ yaku[].id";
      }

      // 最后一局可用房间实时状态做交叉验证（此前各局的状态已被下一局覆盖）。
      if (round + 1 == ends.size()) {
        const json::Value& winner_entry = results->AsArray()[static_cast<size_t>(winner)];
        std::vector<Tile> flat = TilesOf(winner_entry, "hand");
        const std::vector<Meld>& melds = test_room.room()->melds_of(winner);
        EXPECT_EQ(flat.size(), static_cast<size_t>(14 + KongCount(melds))) << "seed " << seed;
        EXPECT_TRUE(IsVerifiedHand(flat)) << "seed " << seed;
        const std::string winning_tile = StrOf(*win, "tile");
        EXPECT_NE(std::find_if(flat.begin(), flat.end(),
                               [&winning_tile](Tile tile) { return FormatTile(tile) == winning_tile; }),
                  flat.end());
        std::vector<std::string> types;
        for (const json::Value& entry : winner_entry.Find("win_type")->AsArray()) {
          types.push_back(entry.AsString());
        }
        ASSERT_FALSE(types.empty());
        EXPECT_EQ(types.back(), tsumo ? "tsumo" : "ron");
        types.pop_back();
        ScoreContext ctx;
        ctx.winner_seat = winner;
        ctx.from_seat = tsumo ? -1 : (winner + 1) % 4;
        ctx.winner_is_dealer = winner == round_dealer;
        ctx.is_rinshan = false;  // 杠上开花记录已随局终清空，这里只交叉验证非杠上开花口径
        const std::vector<std::string> expected_tags =
            WinTags(test_room.room()->hand_of(winner), melds, *ParseTile(winning_tile), ctx);
        if (std::find(types.begin(), types.end(), "rinshan") == types.end()) {
          EXPECT_EQ(types, expected_tags) << "seed " << seed << ": 最后一局的 tags 必须可由规则库复算";
        }
      }
    }
    // 一场必须走满 4 局 —— 除非 C15 连庄闸提前收场（renchan_count > 4）。
    const int last_renchan = static_cast<int>(IntOf(*ends.back(), "renchan_count"));
    EXPECT_TRUE(expected_round_index == 4 || last_renchan > 4)
        << "seed " << seed << ": 未走满 4 局也未触发连庄闸";
    EXPECT_EQ(ends.size(), expected_round_index + last_renchan)
        << "seed " << seed << ": game_end 次数 = 计入局数 + 连庄数";

    // ---- match_end（§5.15）：rankings 排序与并列跳号
    const json::Value* rankings = match_ends.front()->Find("rankings");
    ASSERT_NE(rankings, nullptr);
    ASSERT_EQ(rankings->AsArray().size(), 4u);
    std::array<int64_t, kSeatCount> final_scores {};
    for (const json::Value& entry : rankings->AsArray()) {
      final_scores[static_cast<size_t>(IntOf(entry, "seat"))] = IntOf(entry, "final_score");
    }
    for (const json::Value& entry : rankings->AsArray()) {
      const int seat = static_cast<int>(IntOf(entry, "seat"));
      EXPECT_EQ(IntOf(entry, "final_score"), running_totals[static_cast<size_t>(seat)])
          << "final_score 必须与最后一次 game_end.match_total 逐座位相等";
      int strictly_greater = 0;
      for (int other = 0; other < kSeatCount; ++other) {
        if (final_scores[static_cast<size_t>(other)] > final_scores[static_cast<size_t>(seat)]) ++strictly_greater;
      }
      EXPECT_EQ(static_cast<int>(IntOf(entry, "rank")), 1 + strictly_greater) << "rank = 1 + 严格大于人数（并列跳号）";
    }
    for (size_t i = 1; i < rankings->AsArray().size(); ++i) {
      const int64_t prev = IntOf(rankings->AsArray()[i - 1], "final_score");
      const int64_t cur = IntOf(rankings->AsArray()[i], "final_score");
      ASSERT_TRUE(prev > cur || (prev == cur && IntOf(rankings->AsArray()[i - 1], "seat") <
                                                      IntOf(rankings->AsArray()[i], "seat")))
          << "rankings 必须按 final_score 降序、同分 seat 升序";
    }
  }
  EXPECT_EQ(matches, 40);
  EXPECT_GE(wins, 1) << "no seed produced a win; the settlement path was never exercised";
  EXPECT_EQ(dealers_seen, (std::set<int>{0, 1, 2, 3})) << "C9：庄家必须走过 0/1/2/3（40 场足以覆盖）";
  std::printf("full matches: %d, rounds: %d, wins: %d, draws: %d\n", matches, rounds, wins, draws);
}

// v1.6 自理杠（PROTOCOL §4.6）：打牌窗口出现 ankan/kakan 选项时，动作必须走通 ——
// melds 更新（ankan 追加 / kakan 就地升级组序不变）、补摸 1 张（remaining 派生）、
// 重开打牌窗口、action_event 无 from_seat、张数公式逐字不变。
// ---------------------------------------------------------------------------
// M8 缺省值（A1_ACCEPTANCE §七）：局间隔冻结默认 5000 ms（--inter-hand-delay-ms 缺省口径）。
// 相对时序观察在 e2e（E2/M8 时序观察）里用短配值跑，这里只钉死缺省常量。
TEST(MahjongRules, InterHandDelayDefaultIsFrozen) {
  EXPECT_EQ(mahjong::kDefaultInterHandDelayMs, 5000);
  EXPECT_EQ(mahjong::kMaxInterHandDelayMs, 60000);
}

// ---------------------------------------------------------------------------
// R11（A1_ACCEPTANCE §七）：`remaining == 0`（本局最后一张）时，打牌/鸣牌窗口都**不得**
// 出现任何杠选项（ankan/kakan/kong）——RoomActor 接线层的守卫，规则库签名不动。
// 做法：整场驱动，凡人类窗口打开时 `room->remaining() == 0` 即断言无杠选项；
// 最后一张恰好发到人类座位的情形随 40 场必然出现（≈每局 25%）。
TEST_F(RoomActorTest, NoKongOptionsWhenWallExhausted) {
  int windows_at_zero = 0;
  for (uint64_t seed = 1; seed <= 40 && windows_at_zero < 3; ++seed) {
    TestRoom test_room(seed);
    test_room.Ready();
    test_room.sink()->Drain();
    int guard = 0;
    while (!test_room.room()->game_finished() && guard++ < 20000) {
      if (test_room.room()->round_finished()) {
        test_room.FireRoundAdvance();
        continue;
      }
      std::optional<json::Value> request = test_room.PendingRequest();
      if (test_room.room()->remaining() == 0 && request.has_value()) {
        ++windows_at_zero;
        for (const std::string& action : {"kong", "ankan", "kakan"}) {
          EXPECT_EQ(OptionOf(*request, action), nullptr)
              << "seed " << seed << ": remaining==0 的窗口不得出现 " << action << " 选项";
        }
      }
      if (test_room.AnswerWindow(request)) {
        continue;
      }
      if (test_room.FireAiPacing()) {
        continue;
      }
      if (!test_room.AdvanceOpenWindow()) {
        break;
      }
    }
    ASSERT_TRUE(test_room.room()->game_finished()) << "seed " << seed << " 未跑完整场";
  }
  EXPECT_GE(windows_at_zero, 3) << "40 场内必须观察到 ≥3 个 remaining==0 的人类窗口，否则本测试无证据力";
}

// ---------------------------------------------------------------------------
// M7（A1_ACCEPTANCE §七）：局间（kRoundEnd）**拒一切对局帧**回 BAD_REQUEST、无未决窗口、
// 不下发 state_sync、迟到超时静默丢弃。
TEST_F(RoomActorTest, RoundEndRejectsEveryGameFrame) {
  TestRoom test_room(20251005);
  test_room.Ready();
  test_room.sink()->Drain();
  ASSERT_TRUE(test_room.PlayFullRound()) << "第一局必须能走到局终";
  ASSERT_TRUE(test_room.room()->round_finished());
  ASSERT_FALSE(test_room.room()->game_finished());

  // 局间不下发 state_sync：game_end 之后（历史帧里 game_end 之后）不得再有任何 state_sync。
  // （PlayFullRound 内部的 PendingRequest 已把帧收进 history_，sink 尾部为空。）
  const std::vector<json::Value>& history = test_room.history();
  int last_end_index = -1;
  for (size_t i = 0; i < history.size(); ++i) {
    if (TypeOf(history[i]) == "game_end") last_end_index = static_cast<int>(i);
  }
  ASSERT_GE(last_end_index, 0) << "局终帧必须已到达";
  for (size_t i = static_cast<size_t>(last_end_index) + 1; i < history.size(); ++i) {
    ASSERT_NE(TypeOf(history[i]), "state_sync") << "局间不得下发 state_sync（帧 " << i << "）";
  }
  std::vector<json::Value> tail = test_room.sink()->Drain();
  for (const json::Value& frame : tail) {
    ASSERT_NE(TypeOf(frame), "state_sync") << "局间不得下发 state_sync：" << frame.Dump();
  }

  // 局间的一切对局帧（携带当前 seq 也一样）⇒ BAD_REQUEST，且状态零变更。
  const uint64_t seq = test_room.room()->seq();
  test_room.SendDiscard(SortedTiles(test_room.room()->hand_of(kSeat0)).front(), seq);
  std::vector<json::Value> frames = test_room.sink()->Drain();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(TypeOf(frames[0]), "error");
  EXPECT_EQ(StrOf(frames[0], "code"), "BAD_REQUEST");
  test_room.SendAction("win", {}, seq);
  frames = test_room.sink()->Drain();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(StrOf(frames[0], "code"), "BAD_REQUEST");
  EXPECT_EQ(test_room.room()->seq(), seq);
  EXPECT_TRUE(test_room.room()->round_finished()) << "BAD_REQUEST 不得改变局间状态";

  // 迟到的窗口超时在局间被第一道判据静默丢弃（C6 判据①的局间面）。
  test_room.Send(RoomEnvelope::WindowTimeout(seq));
  EXPECT_TRUE(test_room.sink()->Drain().empty());
  EXPECT_EQ(test_room.room()->seq(), seq);

  // 推进令牌之后一切如常（下一局开局）。
  ASSERT_TRUE(test_room.FireRoundAdvance());
  EXPECT_EQ(test_room.room()->CurrentWindow(), "discard");
}

TEST_F(RoomActorTest, SelfKanActionsRoundTripThroughTheDiscardWindow) {
  int ankan_hits = 0;
  int kakan_hits = 0;
  for (uint64_t seed = 1; seed <= 300 && (ankan_hits == 0 || kakan_hits == 0); ++seed) {
    TestRoom test_room(seed, kDefaultTimeoutMs, /*ai_delay_ms=*/0, /*smart_discard=*/true);
    test_room.Ready();
    test_room.sink()->Drain();
    for (int guard = 0; guard < 600 && !test_room.room()->round_finished(); ++guard) {
      std::optional<json::Value> request = test_room.PendingRequest();
      const bool human_discard =
          test_room.room()->CurrentWindow() == "discard" && test_room.room()->current_seat() == kSeat0;
      if (human_discard && request.has_value()) {
        const json::Value* ankan = OptionOf(*request, "ankan");
        const json::Value* kakan = OptionOf(*request, "kakan");
        const json::Value* chosen = ankan != nullptr ? ankan : kakan;
        if (chosen != nullptr) {
          const bool is_ankan = ankan != nullptr;
          const std::string action = is_ankan ? "ankan" : "kakan";
          const std::vector<Tile> option_tiles = TilesOf(*chosen, "tiles");
          ASSERT_EQ(option_tiles.size(), 1u) << "选项 tiles 必须恰好 1 个牌面元素";
          const std::vector<Tile> hand_before = test_room.room()->hand_of(kSeat0);      // 值拷贝：发送后房间状态会变
          const std::vector<Meld> melds_before = test_room.room()->melds_of(kSeat0);  // 同上（避免引用别名）
          const size_t logical_before = hand_before.size() + 3 * melds_before.size();
          ASSERT_EQ(logical_before, 14u) << "自理杠只在行动态";
          const int meld_count_before = MeldCountOf(melds_before);
          const int kans_before = KanCountOf(melds_before);
          // 就地升级定位（kakan）：记录被升级 pong 组的下标与组序
          int upgraded_index = -1;
          if (!is_ankan) {
            for (int index : KakanMeldIndices(hand_before, melds_before)) {
              if (!melds_before[static_cast<size_t>(index)].tiles.empty() &&
                  FaceOf(melds_before[static_cast<size_t>(index)].tiles.front()) == FaceOf(option_tiles.front())) {
                upgraded_index = index;
                break;
              }
            }
            ASSERT_GE(upgraded_index, 0);
          }

          test_room.SendAction(action, option_tiles, test_room.room()->seq());

          // —— 动作后的房间状态 ——
          const std::vector<Meld>& melds_after = test_room.room()->melds_of(kSeat0);
          const std::vector<Tile>& hand_after = test_room.room()->hand_of(kSeat0);
          EXPECT_EQ(MeldCountOf(melds_after), is_ankan ? meld_count_before + 1 : meld_count_before);
          EXPECT_EQ(KanCountOf(melds_after), kans_before + 1);
          if (is_ankan) {
            ASSERT_FALSE(melds_after.empty());
            const Meld& added = melds_after.back();
            EXPECT_EQ(added.type, MeldType::kAnkan);
            ASSERT_EQ(added.tiles.size(), 4u);
            for (Tile tile : added.tiles) {
              EXPECT_EQ(FaceOf(tile), FaceOf(option_tiles.front()));
            }
            // Send 返回时补牌已完成：hand = 14 − 4（扣牌）+ 1（补牌）相对变化 −3
            EXPECT_EQ(hand_after.size() + 3, hand_before.size());
          } else {
            const Meld& upgraded = melds_after[static_cast<size_t>(upgraded_index)];
            EXPECT_EQ(upgraded.type, MeldType::kKakan);  // 就地升级：组序不变
            ASSERT_EQ(upgraded.tiles.size(), 4u);
            for (Tile tile : upgraded.tiles) {
              EXPECT_EQ(FaceOf(tile), FaceOf(option_tiles.front()));
            }
            // 扣掉手中第 4 张（组内 3 张不动）再补 1 张 ⇒ 手牌张数不变
            EXPECT_EQ(hand_after.size(), hand_before.size());
          }
          EXPECT_FALSE(test_room.room()->round_finished());

          // —— 帧序：action_event（无 from_seat）→ draw（补牌）→ state_sync → action_request ——
          std::vector<json::Value> frames = test_room.sink()->Drain();
          bool saw_event = false;
          bool saw_draw = false;
          for (const json::Value& frame : frames) {
            if (TypeOf(frame) == "action_event" && StrOf(frame, "action") == action) {
              saw_event = true;
              EXPECT_EQ(frame.Find("from_seat"), nullptr) << "自理杠不得携带 from_seat";
              EXPECT_TRUE(SameFaces(TilesOf(frame, "tiles"), option_tiles));
            }
            if (TypeOf(frame) == "draw") {
              saw_draw = true;
            }
          }
          EXPECT_TRUE(saw_event) << "自理杠必须有 action_event";
          EXPECT_TRUE(saw_draw) << "自理杠之后必须补摸 1 张";
          // 补牌后逻辑张数回到 14（公式逐字不变：concealed + 3×melds）
          EXPECT_EQ(test_room.room()->hand_of(kSeat0).size() + 3u * MeldCountOf(melds_after), 14u);
          EXPECT_EQ(test_room.room()->CurrentWindow(), "discard") << "补牌后重开打牌窗口";
          if (is_ankan) {
            ++ankan_hits;
          } else {
            ++kakan_hits;
          }
          test_room.PendingRequest();
          continue;
        }
      }
      if (test_room.AnswerWindow(request)) {
        continue;
      }
      if (!test_room.AdvanceOpenWindow()) {
        break;
      }
    }
  }
  std::printf("self-kan round trips: ankan=%d, kakan=%d\n", ankan_hits, kakan_hits);
  ASSERT_GE(ankan_hits, 1) << "300 个种子必须至少出现一次暗杠窗口";
  ASSERT_GE(kakan_hits, 1) << "300 个种子必须至少出现一次加杠窗口";
}

// v1.6 确定性（任务书测试 6）：同 seed 跑整场两次，帧序列必须逐字节一致。
// `action_request.deadline_ms` 是唯一携带真实时钟的字段（§5.7），比对前剔除。
TEST_F(RoomActorTest, SameSeedReplaysTheWholeMatchFrameForFrame) {
  auto PlayAndDump = [](uint64_t seed) {
    TestRoom test_room(seed);
    test_room.Ready();
    test_room.PlayFullGame();
    EXPECT_TRUE(test_room.room()->game_finished());
    std::string dump;
    for (const json::Value& frame : test_room.history()) {
      dump += frame.Dump();
      dump += '\n';
    }
    // `deadline_ms` 是唯一携带真实时钟的字段（§5.7），比对前归零（Find 只有 const 重载，
    // 就地改写不可行，改用文本归一化）。
    return std::regex_replace(dump, std::regex("\"deadline_ms\":[0-9]+"), "\"deadline_ms\":0");
  };
  for (uint64_t seed : {static_cast<uint64_t>(20251005), static_cast<uint64_t>(7), static_cast<uint64_t>(424242)}) {
    const std::string first = PlayAndDump(seed);
    const std::string second = PlayAndDump(seed);
    EXPECT_EQ(first, second) << "seed " << seed << ": 同 seed 的整场帧序列必须一致";
  }
}

TEST_F(RoomActorTest, ClaimWindowRejectsDiscardAndUnknownAction) {
  bool probed = false;
  for (uint64_t seed = 1; seed <= 60 && !probed; ++seed) {
    TestRoom test_room(seed);
    test_room.Ready();
    for (int guard = 0; guard < 400 && !test_room.room()->game_finished(); ++guard) {
      // One drain per step; the request is handed to the answering policy instead of being
      // dropped on the floor (see RunTimeoutScenario for the full rationale).
      std::optional<json::Value> request = test_room.PendingRequest();
      if (test_room.room()->CurrentWindow() == "claim" && request.has_value() &&
          StrOf(*request, "window") == "claim") {
        const uint64_t seq = test_room.room()->seq();
        const std::vector<Tile> hand_before = test_room.room()->hand_of(kSeat0);
        // A discard during a claim window: NOT_YOUR_TURN, state untouched.
        test_room.SendDiscard(SortedTiles(hand_before).front(), seq);
        std::vector<json::Value> frames = test_room.sink()->Drain();
        ASSERT_EQ(frames.size(), 1u);
        EXPECT_EQ(TypeOf(frames[0]), "error");
        EXPECT_EQ(StrOf(frames[0], "code"), "NOT_YOUR_TURN");

        // An action outside the window's options: ILLEGAL_ACTION, state untouched.
        if (OptionOf(*request, "chow") == nullptr) {
          test_room.SendAction("chow", {}, seq);
          frames = test_room.sink()->Drain();
          ASSERT_EQ(frames.size(), 1u);
          EXPECT_EQ(StrOf(frames[0], "code"), "ILLEGAL_ACTION");
        }
        EXPECT_EQ(test_room.room()->seq(), seq);
        EXPECT_EQ(test_room.room()->hand_of(kSeat0), hand_before);
        EXPECT_EQ(test_room.room()->CurrentWindow(), "claim");

        // An unanswered claim window counts as a pass when the timeout message arrives
        // (GAME_DESIGN §5.6.2) and the game moves on without any error frame.
        test_room.Send(RoomEnvelope::WindowTimeout(seq));
        std::vector<json::Value> after = test_room.sink()->Drain();
        for (const json::Value& frame : after) {
          EXPECT_NE(TypeOf(frame), "error") << frame.Dump();
        }
        EXPECT_NE(test_room.room()->seq(), seq);
        probed = true;
        break;
      }
      if (test_room.AnswerWindow(request)) {
        continue;
      }
      if (!test_room.AdvanceOpenWindow()) {
        break;
      }
    }
  }
  ASSERT_TRUE(probed) << "no seed gave the human a claim window";
}

// ---------------------------------------------------------------------------------------
// Post-meld discard window (H1) and the §1.4.5 / §7.2 bookkeeping
// ---------------------------------------------------------------------------------------

// After a pong/chow the window offers `options: []`: the concealed hand is not a 14-tile draw
// form, so §6.1/§6.3 make a `win` there an ILLEGAL_ACTION. The window must be judged by what
// it offered, not by a recomputation — a hand that pongs into a complete hand used to be
// granted a tsumo (6M from everyone) for a tile it never drew.
//
// The situation only arises for a client that *declines* a win: a shanpon wait can both ron and
// pong the same tile, and ponging it leaves exactly the complete 11-tile concealed form. So the
// test takes the pong where both options exist, which is the exploit's precondition.
TEST_F(RoomActorTest, PostMeldDiscardWindowRejectsWinEvenWhenTheHandIsComplete) {
  bool probed = false;
  // A human that keeps its hand efficient reaches a winning wait sooner, so the probe stays
  // short; it stops at the first seed that shows the situation.
  for (uint64_t seed = 1; seed <= 40 && !probed; ++seed) {
    TestRoom test_room(seed, kDefaultTimeoutMs, /*ai_delay_ms=*/0, /*smart_discard=*/true);
    test_room.Ready();
    for (int guard = 0; guard < 300 && !test_room.room()->game_finished(); ++guard) {
      std::optional<json::Value> request = test_room.PendingRequest();

      const bool claimable_win_and_pong = test_room.room()->CurrentWindow() == "claim" && request.has_value() &&
                                          StrOf(*request, "window") == "claim" &&
                                          OptionOf(*request, "win") != nullptr && OptionOf(*request, "pong") != nullptr;
      if (claimable_win_and_pong) {
        const json::Value* pong = OptionOf(*request, "pong");
        const size_t melds_before = test_room.room()->melds_of(kSeat0).size();
        test_room.SendAction("pong", TilesOf(*pong, "tiles"), test_room.room()->seq());

        // The melder now holds a discard window of its own, and ponging the wait left a
        // complete form behind: `DiscardOptions` reports a win the window never offered.
        ASSERT_FALSE(test_room.room()->game_finished());
        ASSERT_EQ(test_room.room()->CurrentWindow(), "discard");
        ASSERT_EQ(test_room.room()->current_seat(), kSeat0);
        EXPECT_EQ(test_room.room()->melds_of(kSeat0).size(), melds_before + 1);
        const std::vector<Tile>& hand = test_room.room()->hand_of(kSeat0);
        ASSERT_TRUE(FindOption(DiscardOptions(hand, test_room.room()->melds_of(kSeat0)), "win") != nullptr)
            << "a pong on a winning wait must leave a complete concealed hand (seed " << seed << ")";

        for (const json::Value& frame : test_room.sink()->Drain()) {
          if (TypeOf(frame) == "action_request") {
            EXPECT_EQ(StrOf(frame, "window"), "discard");
            EXPECT_EQ(OptionOf(frame, "win"), nullptr) << "a post-meld window must offer no options";
          }
        }

        const uint64_t seq = test_room.room()->seq();
        test_room.SendAction("win", {}, seq);
        std::vector<json::Value> frames = test_room.sink()->Drain();
        ASSERT_EQ(frames.size(), 1u);
        ASSERT_EQ(TypeOf(frames[0]), "error");
        EXPECT_EQ(StrOf(frames[0], "code"), "ILLEGAL_ACTION");

        // §7.3 zero state change: no win, no settlement, the same window is still open.
        EXPECT_FALSE(test_room.room()->game_finished());
        EXPECT_EQ(test_room.room()->seq(), seq);
        EXPECT_EQ(test_room.room()->hand_of(kSeat0), hand);
        EXPECT_EQ(test_room.room()->CurrentWindow(), "discard");
        probed = true;
        break;
      }

      if (test_room.AnswerWindow(request)) {
        continue;
      }
      if (!test_room.AdvanceOpenWindow()) {
        break;
      }
    }
  }
  ASSERT_TRUE(probed) << "no seed offered the human both a win and a pong on the same discard";
}

// PROTOCOL §1.4.5 (v1.6) + ACCOUNT_DESIGN §4: the room tells its owner once per ROUND (C10),
// and only the last notification carries match_over — that flag is what arms the lobby's
// reclaim. The payload is what the lobby turns into match records, so its shape is contract.
TEST_F(RoomActorTest, FinishedCallbackFiresOncePerRoundAndFlagsTheMatchEnd) {
  TestRoom test_room(20251005);
  std::vector<GameSettlement> finished;
  test_room.InstallFinishedCallback(
      [&finished](const GameSettlement& settlement) { finished.push_back(settlement); });
  test_room.Ready();
  test_room.PlayFullGame();

  ASSERT_TRUE(test_room.room()->game_finished());
  const std::vector<const json::Value*> ends = FramesOfType(test_room.history(), "game_end");
  ASSERT_GE(ends.size(), 4u);
  ASSERT_EQ(finished.size(), ends.size()) << "回调必须逐局各一次（C10）";
  EXPECT_EQ(finished.front().room_id, "r-0001");
  for (size_t i = 0; i + 1 < finished.size(); ++i) {
    EXPECT_FALSE(finished[i].match_over) << "非最后一局的回调不得触发回收（C10）";
  }
  EXPECT_TRUE(finished.back().match_over) << "场终回调必须带 match_over";

  std::array<int, kSeatCount> running {};
  for (size_t round = 0; round < finished.size(); ++round) {
    const GameSettlement& settlement = finished[round];
    ASSERT_EQ(settlement.seats.size(), 4u);
    int delta_sum = 0;
    int winners = 0;
    for (size_t seat = 0; seat < settlement.seats.size(); ++seat) {
      const SettledSeat& settled = settlement.seats[seat];
      EXPECT_EQ(settled.player.player_id, seat == 0 ? "p-human" : "bot-" + std::to_string(seat));
      EXPECT_EQ(settled.is_bot, seat != 0);
      EXPECT_EQ(settled.player.account_id, 0u) << "these test seats are guests, so nothing is recorded";
      delta_sum += settled.delta_score;
      winners += settled.winner ? 1 : 0;
      running[seat] += settled.delta_score;
    }
    EXPECT_EQ(delta_sum, 0) << "the zero-sum rule of §6.4 must survive into the recorded payload";
    if (settlement.reason == "draw_exhausted") {
      EXPECT_EQ(winners, 0);
    } else {
      EXPECT_EQ(winners, 1) << "a finished round is either drawn or won by exactly one seat";
    }
    // 逐局 delta 与帧里的 match_total 对齐（同一份数据，不得两算）
    const json::Value* results = ends[round]->Find("results");
    ASSERT_NE(results, nullptr);
    for (const json::Value& entry : results->AsArray()) {
      EXPECT_EQ(IntOf(entry, "match_total"), running[static_cast<size_t>(IntOf(entry, "seat"))]);
    }
  }

  // ---- B3 场终快照（match_over 那次回调）：落库数据的房间侧契约 ----
  const GameSettlement& final_settlement = finished.back();
  ASSERT_TRUE(final_settlement.match_over);
  // final_score：零和（同 room 四行之和 = 0，ACCOUNT_DESIGN v1.3 §4 一致性约束）
  int64_t final_score_sum = 0;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    final_score_sum += final_settlement.totals[static_cast<size_t>(seat)];
  }
  EXPECT_EQ(final_score_sum, 0) << "四行 final_score 之和必须为 0";
  // hands_played（D2 终裁）= game_end 下发手数（平凡恒等）
  EXPECT_EQ(final_settlement.hands_played, ends.size());
  // dealer_hands：各座位坐庄手数之和 = 打过的手数（每局恰有一个庄家）
  int dealer_hands_sum = 0;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    EXPECT_GE(final_settlement.dealer_hands[static_cast<size_t>(seat)], 0);
    dealer_hands_sum += final_settlement.dealer_hands[static_cast<size_t>(seat)];
  }
  EXPECT_EQ(dealer_hands_sum, ends.size()) << "每局恰有一个庄家 ⇒ 坐庄手数之和 = game_end 次数";
  // final_rank 与 match_end.rankings 逐座位一致（同源，不得两算）
  const json::Value* rankings = FramesOfType(test_room.history(), "match_end").front()->Find("rankings");
  ASSERT_NE(rankings, nullptr);
  std::array<int, kSeatCount> ranking_by_seat {};
  for (const json::Value& entry : rankings->AsArray()) {
    ranking_by_seat[static_cast<size_t>(IntOf(entry, "seat"))] = static_cast<int>(IntOf(entry, "rank"));
    // final_score 与最后一次 game_end.match_total 逐座位相等（match_end 与快照同一份数据）
    const int seat = static_cast<int>(IntOf(entry, "seat"));
    EXPECT_EQ(IntOf(entry, "final_score"), final_settlement.totals[static_cast<size_t>(seat)]);
  }
  EXPECT_EQ(final_settlement.final_rank, ranking_by_seat);

  // Frames that arrive after the match end change nothing, so no further notification.
  const size_t notifications_before = finished.size();
  test_room.SendAction("win", {}, 1);
  test_room.sink()->Drain();
  EXPECT_EQ(finished.size(), notifications_before);
}

// PROTOCOL §7.2: an error the room answers must be accounted on the connection, otherwise the
// "10 errors → disconnect" quota could never be reached from inside a game.
TEST_F(RoomActorTest, RoomLevelErrorsCountTowardTheQuota) {
  TestRoom test_room(20251005);
  test_room.Ready();
  test_room.sink()->Drain();  // game_start + the dealer's discard window

  ASSERT_EQ(test_room.sink()->illegal_ops(), 0);
  ASSERT_EQ(test_room.sink()->bad_frames(), 0);

  // ILLEGAL_ACTION: `pass` is not a legal reply in a discard window (H1).
  test_room.SendAction("pass", {}, 1);
  test_room.sink()->Drain();
  EXPECT_EQ(test_room.sink()->illegal_ops(), 1);

  // UNKNOWN_TILE: a tile that is not in hand.
  const std::vector<Tile> hand = test_room.room()->hand_of(kSeat0);
  const FaceCounts counts = CountFaces(hand);
  Face absent = 0;
  for (Face face = 0; face < kFaceCount; ++face) {
    if (counts[face] == 0) {
      absent = face;
      break;
    }
  }
  test_room.SendDiscard(MakeTileFromFace(absent), 1);
  test_room.sink()->Drain();
  EXPECT_EQ(test_room.sink()->illegal_ops(), 2);
  EXPECT_EQ(test_room.sink()->bad_frames(), 0);
}

// t4: with several wired humans, one broadcast has to reach every seat carrying that seat's own
// private fields and one shared public block. This is the only shape in this file that exercises
// the multi-recipient path — four state_sync frames per broadcast, four-way fan-out of
// discard_event/action_event; every other test wires a single human, where those loops build
// exactly one frame and the sharing is unobservable. The `seats[]` hoist in StateSyncFrames and
// the single-build event broadcasts are meant to be behavior-preserving: what this test pins is
// that they stay so (the speedup itself is measured by the microbenchmark, not here).
TEST_F(RoomActorTest, MultiHumanBroadcastCarriesPrivateHandsAndOneSharedSeatsBlock) {
  TestRoom test_room(20251005, kDefaultTimeoutMs, /*ai_delay_ms=*/0, /*smart_discard=*/false,
                     /*install_scheduler=*/true, /*all_human=*/true);
  test_room.Ready();

  // Nobody answers anything: the frozen §6.4 default actions (default discard / pass) carry the
  // table to the first draw, and a draw is what broadcasts state_sync (§5.12).
  std::array<std::vector<json::Value>, kSeatCount> received;
  for (int step = 0; step < 8; ++step) {
    bool every_seat_has_one = true;
    for (int seat = 0; seat < kSeatCount; ++seat) {
      if (FramesOfType(received[static_cast<size_t>(seat)], "state_sync").empty()) {
        every_seat_has_one = false;
      }
    }
    if (every_seat_has_one) {
      break;
    }
    ASSERT_TRUE(test_room.AdvanceOpenWindow()) << "the room must keep moving while nobody answers";
    for (int seat = 0; seat < kSeatCount; ++seat) {
      std::vector<json::Value> frames = test_room.sink_of(seat)->Drain();
      for (json::Value& frame : frames) {
        received[static_cast<size_t>(seat)].push_back(std::move(frame));
      }
    }
  }

  // The melds shape of §5.9, rebuilt locally so the wire form is checked alongside the sharing.
  const auto expected_melds = [](const std::vector<Meld>& melds) {
    json::Value array = json::Value::Arr(melds.size());
    for (const Meld& meld : melds) {
      json::Value entry = json::Value::Obj(2);
      entry.Set("type", json::Value::Str(MeldTypeName(meld.type)));
      json::Value tiles = json::Value::Arr(meld.tiles.size());
      for (Tile tile : meld.tiles) {
        tiles.Append(json::Value::Str(FormatTile(tile)));
      }
      entry.Set("tiles", std::move(tiles));
      array.Append(std::move(entry));
    }
    return array;
  };

  std::string shared_seats;
  int64_t broadcast_seq = -1;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    const std::vector<const json::Value*> syncs = FramesOfType(received[static_cast<size_t>(seat)], "state_sync");
    ASSERT_FALSE(syncs.empty()) << "seat " << seat << " never received a state_sync";
    const json::Value& sync = *syncs.back();

    // Same broadcast for all four: one seq and one public block (the recipient-independent part
    // of §5.9, which the room renders once and splices into every frame).
    if (broadcast_seq < 0) {
      broadcast_seq = IntOf(sync, "seq");
    } else {
      EXPECT_EQ(IntOf(sync, "seq"), broadcast_seq) << "seat " << seat << " saw a different broadcast";
    }
    const json::Value* seats = sync.Find("seats");
    ASSERT_NE(seats, nullptr) << "seat " << seat;
    const std::string seats_dump = seats->Dump();
    if (shared_seats.empty()) {
      shared_seats = seats_dump;
    } else {
      EXPECT_EQ(seats_dump, shared_seats) << "seat " << seat << ": the public block must be one render";
    }

    // ...while hand/melds are this seat's own, not a neighbour's.
    EXPECT_TRUE(SameFaces(TilesOf(sync, "hand"), test_room.room()->hand_of(seat))) << "seat " << seat;
    const json::Value* melds = sync.Find("melds");
    ASSERT_NE(melds, nullptr) << "seat " << seat;
    EXPECT_EQ(melds->Dump(), expected_melds(test_room.room()->melds_of(seat)).Dump()) << "seat " << seat;

    // H6: the public per-seat view never carries a hand, only the count.
    ASSERT_EQ(seats->AsArray().size(), static_cast<size_t>(kSeatCount)) << "seat " << seat;
    for (const json::Value& entry : seats->AsArray()) {
      EXPECT_EQ(entry.Find("hand"), nullptr) << "seat " << seat << ": seats[] leaked a hand (H6)";
      EXPECT_NE(entry.Find("hand_count"), nullptr) << "seat " << seat;
    }
  }

  // The discard_event broadcast (§5.6) is one frame for the whole table too: same content at
  // every seat, delivered exactly once per seat.
  const std::vector<const json::Value*> reference = FramesOfType(received[0], "discard_event");
  ASSERT_FALSE(reference.empty());
  for (int seat = 0; seat < kSeatCount; ++seat) {
    const std::vector<const json::Value*> events = FramesOfType(received[static_cast<size_t>(seat)], "discard_event");
    ASSERT_EQ(events.size(), reference.size()) << "seat " << seat;
    for (size_t i = 0; i < events.size(); ++i) {
      EXPECT_EQ(events[i]->Dump(), reference[i]->Dump()) << "seat " << seat << " frame " << i;
    }
  }
}

}  // namespace
}  // namespace mahjong
