// 日麻房间（mode == kRiichi）的冒烟测试。
//
// 只验「房间有没有把玩法接对」：牌山切分（可摸区 122 / 剩 69）、持点起点、宝牌指示牌、
// classic 帧字节不受影响、以及 classic 房间拒绝立直。算分正确性由 riichi_rules_test 负责。

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <memory>
#include <utility>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "actor_scope.h"
#include "json.h"
#include "player.h"
#include "protocol.h"
#include "room.h"
#include "seat_endpoint.h"

namespace mahjong {
namespace {

// 测试用出牌策略：打掉最孤立的那张（留对子与邻牌 ⇒ 更容易吃碰杠，把整场推得更「满」）。
std::string MostIsolatedTile(const std::vector<std::string>& hand) {
  auto score = [&hand](const std::string& tile) {
    int value = 0;
    for (const std::string& other : hand) {
      if (other == tile) {
        value += 10;
        continue;
      }
      if (tile.size() != 2 || other.size() != 2 || tile[1] != other[1] || tile[1] == 'z') {
        continue;
      }
      const int delta = std::abs(tile[0] - other[0]);
      if (delta == 1) {
        value += 6;
      } else if (delta == 2) {
        value += 2;
      }
    }
    return value;
  };
  const std::string* best = &hand.front();
  int best_score = score(*best);
  for (const std::string& tile : hand) {
    const int current = score(tile);
    if (current < best_score) {
      best = &tile;
      best_score = current;
    }
  }
  return *best;
}

class FrameSink {
 public:
  void Push(std::string frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string error;
    std::optional<json::Value> parsed = json::Value::Parse(frame, &error);
    if (parsed.has_value()) {
      frames_.push_back(std::move(*parsed));
    }
  }
  std::vector<json::Value> Drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<json::Value> out;
    out.swap(frames_);
    return out;
  }

 private:
  std::mutex mutex_;
  std::vector<json::Value> frames_;
};

class CapturingSeat : public SeatEndpoint {
 public:
  explicit CapturingSeat(std::shared_ptr<FrameSink> sink) : sink_(std::move(sink)) {}
  stdexec::task<void> Dispatch(PlayerEnvelope envelope) override {
    if (auto* frame = std::get_if<PlayerEnvelope::SeatFrameMsg>(&envelope.msg)) {
      sink_->Push(std::move(frame->frame));
    } else if (auto* error = std::get_if<PlayerEnvelope::SeatErrorMsg>(&envelope.msg)) {
      sink_->Push(std::move(error->frame));
    }
    co_return;
  }

 private:
  std::shared_ptr<FrameSink> sink_;
};

std::string TypeOf(const json::Value& frame) {
  const json::Value* type = frame.Find("type");
  return type != nullptr && type->IsString() ? type->AsString() : std::string("<none>");
}

class RiichiRoomTest : public ::testing::Test {
 protected:
  void SetUp() override { ex_actor::Init(/*thread_pool_size=*/2); }
  void TearDown() override {
    stdexec::sync_wait(ActorSendScope().on_empty());
    ex_actor::Shutdown();
  }

  // 起一个「座位 0 是真人、其余是 bot」的房间。ai_delay_ms = 0：bot 在同一个消息里同步决策。
  // |entrust| = true 时座位 0 挂空 endpoint：房间按「托管」把它的每一手都自己打完，
  // 于是整场可以在单元测试里无交互跑完（半庄要跑到第 5 局才能看见南场）。
  void BuildRoom(GameMode mode, uint64_t seed, GameLength length = GameLength::kTonpu,
                 bool entrust = false) {
    sink_ = std::make_shared<FrameSink>();
    std::vector<RoomSeat> seats;
    for (int seat = 0; seat < kSeatCount; ++seat) {
      RoomSeat slot;
      slot.seat = seat;
      slot.is_bot = seat != 0;
      slot.player.player_id = seat == 0 ? "p-human" : "bot-" + std::to_string(seat);
      slot.player.name = seat == 0 ? "Human" : "AI-" + std::to_string(seat);
      seats.push_back(std::move(slot));
    }
    human_ = seats.front().player;
    if (!entrust) {
      ex_actor::ActorRef<CapturingSeat> endpoint =
          std::get<0>(stdexec::sync_wait(ex_actor::Spawn<CapturingSeat>(sink_)).value());
      seats.front().endpoint = endpoint;
    }
    room_ = std::make_unique<RoomActor>("r-riichi", seed, protocol::kDefaultActionTimeoutMs, std::move(seats),
                                        nullptr, /*ai_delay_ms=*/0, /*inter_hand_delay_ms=*/0,
                                        ex_actor::ActorRef<RoomOwner> {}, TimeoutScheduler {},
                                        RoomFinishedCallback {}, mode, length);
  }

  void Send(RoomEnvelope envelope) { stdexec::sync_wait(room_->Dispatch(std::move(envelope))); }

  // Ready 之后把 sink 里的帧排空，返回第一份 game_start。
  std::optional<json::Value> ReadyAndTakeGameStart() {
    Send(RoomEnvelope::Ready(human_));
    std::optional<json::Value> start;
    for (json::Value& frame : sink_->Drain()) {
      if (TypeOf(frame) == "game_start" && !start.has_value()) {
        start = std::move(frame);
      }
    }
    return start;
  }

  std::unique_ptr<RoomActor> room_;
  std::shared_ptr<FrameSink> sink_;
  PlayerInfo human_;
};

// 日麻开局：王牌 14 张（可摸区 122 − 发牌 53 = 剩 69）、持点 25000、一张宝牌指示牌
TEST_F(RiichiRoomTest, DealsDeadWallAndStartsAt25000) {
  BuildRoom(GameMode::kRiichi, /*seed=*/20260101);
  const std::optional<json::Value> start = ReadyAndTakeGameStart();
  ASSERT_TRUE(start.has_value());
  EXPECT_EQ(GameModeName(room_->mode()), std::string_view("riichi"));
  EXPECT_EQ(room_->remaining(), 69);

  const json::Value* points = start->Find("points");
  ASSERT_NE(points, nullptr);
  ASSERT_TRUE(points->IsArray());
  ASSERT_EQ(points->AsArray().size(), 4u);
  for (const json::Value& value : points->AsArray()) {
    EXPECT_EQ(value.AsInt(), 25000);
  }
  const json::Value* dora = start->Find("dora_indicators");
  ASSERT_NE(dora, nullptr);
  EXPECT_EQ(dora->AsArray().size(), 1u);
  EXPECT_EQ(start->Find("round_wind")->AsInt(), 0);
  EXPECT_EQ(start->Find("seat_wind")->AsInt(), 0);
}

// classic 房：帧字节不受影响（没有 points / dora 键），牌山仍是 83
TEST_F(RiichiRoomTest, ClassicFramesStayUnchanged) {
  BuildRoom(GameMode::kClassic, /*seed=*/20260101);
  const std::optional<json::Value> start = ReadyAndTakeGameStart();
  ASSERT_TRUE(start.has_value());
  EXPECT_EQ(room_->remaining(), 83);
  EXPECT_EQ(start->Find("points"), nullptr);
  EXPECT_EQ(start->Find("dora_indicators"), nullptr);
}

// classic 房拒绝立直（动作名合法，但本玩法不提供该窗口选项）
TEST_F(RiichiRoomTest, ClassicRoomRejectsRiichi) {
  BuildRoom(GameMode::kClassic, /*seed=*/20260101);
  const std::optional<json::Value> start = ReadyAndTakeGameStart();
  ASSERT_TRUE(start.has_value());
  const json::Value* hand = start->Find("hand");
  ASSERT_NE(hand, nullptr);
  ASSERT_FALSE(hand->AsArray().empty());

  json::Value action = json::Value::Obj(4);  // type, action, tiles, seq
  action.Set("type", json::Value::Str("action"));
  action.Set("action", json::Value::Str("riichi"));
  json::Value tiles = json::Value::Arr(1);
  tiles.Append(json::Value::Str(hand->AsArray().front().AsString()));
  action.Set("tiles", std::move(tiles));
  action.Set("seq", json::Value::Int(static_cast<int64_t>(room_->seq())));
  Send(RoomEnvelope::GameFrame(human_, std::move(action)));

  bool saw_error = false;
  for (json::Value& frame : sink_->Drain()) {
    if (TypeOf(frame) != "error") {
      continue;
    }
    saw_error = true;
    const json::Value* code = frame.Find("code");
    ASSERT_NE(code, nullptr);
    EXPECT_EQ(code->AsString(), protocol::kCodeIllegalAction);
  }
  EXPECT_TRUE(saw_error);
}

// 日麻房：拿着手里没有的牌宣言立直 ⇒ ILLEGAL_ACTION（不信任客户端）
TEST_F(RiichiRoomTest, RiichiRejectsTileNotInHand) {
  BuildRoom(GameMode::kRiichi, /*seed=*/20260101);
  const std::optional<json::Value> start = ReadyAndTakeGameStart();
  ASSERT_TRUE(start.has_value());
  const json::Value* hand = start->Find("hand");
  ASSERT_NE(hand, nullptr);

  // 挑一个手牌里没有的牌面（34 面里必然存在）
  std::array<bool, kFaceCount> present {};
  for (const json::Value& tile : hand->AsArray()) {
    const std::optional<Tile> parsed = ParseTile(tile.AsString());
    ASSERT_TRUE(parsed.has_value());
    present[static_cast<size_t>(FaceOf(*parsed))] = true;
  }
  std::optional<std::string> absent;
  for (int face = 0; face < kFaceCount && !absent.has_value(); ++face) {
    if (!present[static_cast<size_t>(face)]) {
      absent = FormatTile(MakeTileFromFace(static_cast<Face>(face)));
    }
  }
  ASSERT_TRUE(absent.has_value());

  json::Value action = json::Value::Obj(4);  // type, action, tiles, seq
  action.Set("type", json::Value::Str("action"));
  action.Set("action", json::Value::Str("riichi"));
  json::Value tiles = json::Value::Arr(1);
  tiles.Append(json::Value::Str(*absent));
  action.Set("tiles", std::move(tiles));
  action.Set("seq", json::Value::Int(static_cast<int64_t>(room_->seq())));
  Send(RoomEnvelope::GameFrame(human_, std::move(action)));

  bool saw_error = false;
  for (json::Value& frame : sink_->Drain()) {
    if (TypeOf(frame) != "error") {
      continue;
    }
    saw_error = true;
    const json::Value* code = frame.Find("code");
    ASSERT_NE(code, nullptr);
    EXPECT_EQ(code->AsString(), protocol::kCodeIllegalAction);
  }
  EXPECT_TRUE(saw_error);
}

// 半庄（v1.14）：8 局、东场入场；东场仍是 4 局且为缺省
TEST_F(RiichiRoomTest, LengthSelectsRoundCount) {
  BuildRoom(GameMode::kRiichi, /*seed=*/20260101, GameLength::kHanchan);
  const std::optional<json::Value> start = ReadyAndTakeGameStart();
  ASSERT_TRUE(start.has_value());
  EXPECT_EQ(room_->length(), GameLength::kHanchan);
  EXPECT_EQ(room_->round_count(), 8);
  EXPECT_EQ(room_->round_wind(), 0);  // 第一局仍是东场
  EXPECT_EQ(start->Find("round_count")->AsInt(), 8);

  BuildRoom(GameMode::kRiichi, /*seed=*/20260101);
  const std::optional<json::Value> tonpu = ReadyAndTakeGameStart();
  ASSERT_TRUE(tonpu.has_value());
  EXPECT_EQ(room_->round_count(), 4);
  EXPECT_EQ(tonpu->Find("round_count")->AsInt(), 4);
}

// 半庄真的会打到南场（v1.14）：托管整场，逐手采样「局号 → 场风」
TEST_F(RiichiRoomTest, HanchanPlaysThroughSouthRound) {
  BuildRoom(GameMode::kRiichi, /*seed=*/20260101, GameLength::kHanchan);
  Send(RoomEnvelope::Ready(human_));

  std::map<std::pair<int, int>, int> seen;  // (round_index, round_wind) -> 采样次数
  std::vector<std::string> my_hand;
  bool need_answer = false;
  uint64_t seq = 0;
  std::string window;

  for (int guard = 0; guard < 20000 && !room_->game_finished(); ++guard) {
    for (json::Value& frame : sink_->Drain()) {
      const std::string type = TypeOf(frame);
      if (type == "game_start" || type == "state_sync") {
        const json::Value* hand = frame.Find("hand");
        if (hand != nullptr && hand->IsArray()) {
          my_hand.clear();
          for (const json::Value& tile : hand->AsArray()) {
            my_hand.push_back(tile.AsString());
          }
        }
        continue;
      }
      if (type == "action_request") {
        const json::Value* seq_value = frame.Find("seq");
        const json::Value* window_value = frame.Find("window");
        need_answer = seq_value != nullptr && window_value != nullptr;
        if (need_answer) {
          seq = static_cast<uint64_t>(seq_value->AsInt());
          window = window_value->AsString();
        }
      }
    }

    ++seen[{room_->round_index(), room_->round_wind()}];
    if (!need_answer) {
      stdexec::sync_wait(ActorSendScope().on_empty());
      continue;
    }
    need_answer = false;

    json::Value action = json::Value::Obj(5);  // type, seq + (discard tile | action/tiles)
    action.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
    if (window == "discard" && !my_hand.empty()) {
      action.Set("type", json::Value::Str("discard"));
      action.Set("tile", json::Value::Str(MostIsolatedTile(my_hand)));
    } else {
      action.Set("type", json::Value::Str("action"));
      action.Set("action", json::Value::Str("pass"));
      action.Set("tiles", json::Value::Arr(0));
    }
    Send(RoomEnvelope::GameFrame(human_, std::move(action)));
  }

  ASSERT_TRUE(room_->game_finished()) << "半庄没能在自动应答下跑完";
  EXPECT_EQ(room_->round_count(), 8);
  EXPECT_GE(room_->round_index(), 8) << "要走满 8 局才场终";

  // 每一局的场风都必须由局号唯一决定：0..3 东场、4..7 南场
  for (const auto& [key, count] : seen) {
    EXPECT_EQ(key.second, key.first / riichi::kRoundsPerWind)
        << "round_index=" << key.first << " 的场风算错了";
  }
  EXPECT_NE(seen.find({0, 0}), seen.end()) << "第 1 局必须是东场";
  EXPECT_NE(seen.find({4, 1}), seen.end()) << "第 5 局必须进南场（场风 1）";
}

// classic 的局数由 C15 冻结为 4：length 传什么都不影响（入口层已拒：classic + hanchan ⇒ BAD_REQUEST）
TEST_F(RiichiRoomTest, ClassicIgnoresLength) {
  BuildRoom(GameMode::kClassic, /*seed=*/20260101, GameLength::kHanchan);
  const std::optional<json::Value> start = ReadyAndTakeGameStart();
  ASSERT_TRUE(start.has_value());
  EXPECT_EQ(room_->round_count(), 4);
  EXPECT_EQ(start->Find("round_count")->AsInt(), 4);
}

}  // namespace
}  // namespace mahjong
