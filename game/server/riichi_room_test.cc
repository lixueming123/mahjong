// ============================================================================================
// FROZEN — 这个测试跟着 LegacyRoomActor（room.h / room.cc）一起冻住了：不在 all 目标里、
// 不注册 ctest、ctest 不会跑它。日麻侧现在的语义覆盖在 `mahjong_riichi_game_test`
// （三条 golden 转录逐字节比对，6034/5297/5255 帧）。
// 要单独跑：cmake --build build --target riichi_room_test && ./game/riichi_room_test
// ============================================================================================
// 日麻房间（mode == kRiichi）的冒烟测试。
//
// 只验「房间有没有把玩法接对」：牌山切分（可摸区 122 / 剩 69）、持点起点、宝牌指示牌、
// classic 帧字节不受影响、以及 classic 房间拒绝立直。算分正确性由 riichi_rules_test 负责。

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "actor_scope.h"
#include "json.h"
#include "player.h"
#include "protocol.h"
#include "riichi_rules.h"
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

// The wire type is now an integer (PROTOCOL v2.0 §1.3.1); an unknown value parses back to
// kInvalid, which no assertion below ever matches.
protocol::FrameType TypeOf(const json::Value& frame) {
  const json::Value* type = frame.Find("type");
  if (type == nullptr || !type->IsInteger()) {
    return protocol::FrameType::kInvalid;
  }
  const std::optional<protocol::FrameType> parsed = protocol::ParseFrameType(type->AsInt());
  return parsed.value_or(protocol::FrameType::kInvalid);
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
  void BuildRoom(GameMode mode, uint64_t seed, GameLength length = GameLength::kTonpu, bool entrust = false) {
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
    room_ = std::make_unique<LegacyRoomActor>("r-riichi", seed, protocol::kDefaultActionTimeoutMs, std::move(seats), nullptr,
                                        /*ai_delay_ms=*/0, /*inter_hand_delay_ms=*/0, ex_actor::ActorRef<RoomOwner> {},
                                        TimeoutScheduler {}, RoomFinishedCallback {}, mode, length);
  }

  void Send(RoomEnvelope envelope) { stdexec::sync_wait(room_->Dispatch(std::move(envelope))); }

  // Ready 之后把 sink 里的帧排空，返回第一份 game_start。
  std::optional<json::Value> ReadyAndTakeGameStart() {
    Send(RoomEnvelope::Ready(human_));
    std::optional<json::Value> start;
    for (json::Value& frame : sink_->Drain()) {
      if (TypeOf(frame) == protocol::FrameType::kGameStart && !start.has_value()) {
        start = std::move(frame);
      }
    }
    return start;
  }

  std::unique_ptr<LegacyRoomActor> room_;
  std::shared_ptr<FrameSink> sink_;
  PlayerInfo human_;
};

// 日麻开局：王牌 14 张（可摸区 122 − 发牌 53 = 剩 69）、持点 25000、一张宝牌指示牌
TEST_F(RiichiRoomTest, DealsDeadWallAndStartsAt25000) {
  BuildRoom(GameMode::kRiichi, /*seed=*/20260101);
  const std::optional<json::Value> start = ReadyAndTakeGameStart();
  ASSERT_TRUE(start.has_value());
  EXPECT_EQ(room_->mode(), GameMode::kRiichi);
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
  action.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kAction)));
  action.Set("action", json::Value::Int(protocol::WireOf(protocol::Action::kRiichi)));
  json::Value tiles = json::Value::Arr(1);
  tiles.Append(json::Value::Str(hand->AsArray().front().AsString()));
  action.Set("tiles", std::move(tiles));
  action.Set("seq", json::Value::Int(static_cast<int64_t>(room_->seq())));
  Send(RoomEnvelope::GameFrame(human_, std::move(action)));

  bool saw_error = false;
  for (json::Value& frame : sink_->Drain()) {
    if (TypeOf(frame) != protocol::FrameType::kError) {
      continue;
    }
    saw_error = true;
    const json::Value* code = frame.Find("code");
    ASSERT_NE(code, nullptr);
    EXPECT_EQ(code->AsInt(), protocol::WireOf(protocol::ErrorCode::kIllegalAction));
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
  action.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kAction)));
  action.Set("action", json::Value::Int(protocol::WireOf(protocol::Action::kRiichi)));
  json::Value tiles = json::Value::Arr(1);
  tiles.Append(json::Value::Str(*absent));
  action.Set("tiles", std::move(tiles));
  action.Set("seq", json::Value::Int(static_cast<int64_t>(room_->seq())));
  Send(RoomEnvelope::GameFrame(human_, std::move(action)));

  bool saw_error = false;
  for (json::Value& frame : sink_->Drain()) {
    if (TypeOf(frame) != protocol::FrameType::kError) {
      continue;
    }
    saw_error = true;
    const json::Value* code = frame.Find("code");
    ASSERT_NE(code, nullptr);
    EXPECT_EQ(code->AsInt(), protocol::WireOf(protocol::ErrorCode::kIllegalAction));
  }
  EXPECT_TRUE(saw_error);
}

// 日麻房（v1.21 §4.4）：给了 riichi 选项就必须同时给出「哪些牌能作宣言牌」，
// 且每一条候选都必须是真手牌里的牌面（客户端据此在手牌上打标记，玩家不用试错）。
TEST_F(RiichiRoomTest, RiichiTilesAccompanyRiichiOption) {
  BuildRoom(GameMode::kRiichi, /*seed=*/20260101);
  // 不用 ReadyAndTakeGameStart：它会把 game_start 之后的帧一并排空，
  // 第一局的 action_request 就在其中（本测试要靠这条窗口验证 riichi_tiles）。
  Send(RoomEnvelope::Ready(human_));

  auto send_discard = [this](std::string tile, uint64_t seq) {
    json::Value frame = json::Value::Obj(3);  // type, tile, seq
    frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kDiscard)));
    frame.Set("tile", json::Value::Str(std::move(tile)));
    frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
    Send(RoomEnvelope::GameFrame(human_, std::move(frame)));
  };
  auto send_action = [this](protocol::Action action, std::vector<std::string> tiles, uint64_t seq) {
    json::Value frame = json::Value::Obj(4);  // type, action, tiles, seq
    frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kAction)));
    frame.Set("action", json::Value::Int(protocol::WireOf(action)));
    json::Value list = json::Value::Arr(tiles.size());
    for (std::string& tile : tiles) {
      list.Append(json::Value::Str(std::move(tile)));
    }
    frame.Set("tiles", std::move(list));
    frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
    Send(RoomEnvelope::GameFrame(human_, std::move(frame)));
  };

  bool saw_riichi_window = false;
  bool need_answer = false;
  uint64_t pending_seq = 0;
  int pending_window = 0;
  int answered = 0;
  int step = 0;
  for (; step < 2000 && !saw_riichi_window && !room_->game_finished(); ++step) {
    need_answer = false;
    for (const json::Value& frame : sink_->Drain()) {
      if (TypeOf(frame) != protocol::FrameType::kActionRequest) {
        continue;
      }
      const json::Value* window = frame.Find("window");
      const json::Value* options = frame.Find("options");
      const json::Value* seq_value = frame.Find("seq");
      if (window == nullptr || options == nullptr || seq_value == nullptr) {
        continue;
      }
      need_answer = true;
      pending_seq = static_cast<uint64_t>(seq_value->AsInt());
      pending_window = static_cast<int>(window->AsInt());
      bool offers_riichi = false;
      for (const json::Value& option : options->AsArray()) {
        const json::Value* action = option.Find("action");
        if (action != nullptr && action->AsInt() == protocol::WireOf(protocol::Action::kRiichi)) {
          offers_riichi = true;
        }
      }
      const json::Value* riichi_tiles = frame.Find("riichi_tiles");
      if (offers_riichi) {
        saw_riichi_window = true;
        ASSERT_NE(riichi_tiles, nullptr) << "有 riichi 选项却没有 riichi_tiles";
        std::vector<std::string> expected;
        for (Face face : riichi::RiichiDiscardFaces(room_->hand_of(0), room_->melds_of(0))) {
          expected.push_back(FormatTile(MakeTileFromFace(face), /*red_fives=*/false));
        }
        ASSERT_FALSE(riichi_tiles->AsArray().empty()) << "候选为空就不该给 riichi 选项";
        for (const json::Value& item : riichi_tiles->AsArray()) {
          const std::string& code = item.AsString();
          EXPECT_NE(std::find(expected.begin(), expected.end(), code), expected.end())
              << "候选 " << code << " 不是这张手牌里能宣言的牌面";
        }
      } else {
        EXPECT_EQ(riichi_tiles, nullptr) << "没有 riichi 选项时不该带 riichi_tiles（classic 口径）";
      }
    }
    if (!need_answer) {
      stdexec::sync_wait(ActorSendScope().on_empty());
      continue;
    }
    ++answered;
    if (pending_window != protocol::WireOf(protocol::Window::kDiscard)) {
      send_action(protocol::Action::kPass, {}, pending_seq);  // 鸣牌窗口：过
      continue;
    }
    std::vector<std::string> hand_codes;
    for (Tile tile : room_->hand_of(0)) {
      hand_codes.push_back(FormatTile(tile, /*red_fives=*/false));
    }
    if (hand_codes.empty()) {
      break;
    }
    // 打一张最孤立的牌，把牌局往前推（玩家不会鸣牌，他的牌河只由这里写入）
    send_discard(MostIsolatedTile(hand_codes), pending_seq);
  }
  EXPECT_TRUE(saw_riichi_window) << "固定种子下应至少出现一次立直窗口（steps=" << step << ", answered=" << answered
                                 << ", round_index=" << room_->round_index() << ", finished=" << room_->game_finished()
                                 << ")";
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

// 赤 5（v1.18）：日麻房会把 5 的第 4 张下发成 `0m`/`0p`/`0s`；classic 房永远只下发 `1-9` 开头。
// 发牌是种子确定的，所以这里换成不同种子把「至少见过一张赤牌」跑成确定性断言。
TEST_F(RiichiRoomTest, RedFivesAreDealtOnlyInRiichi) {
  bool saw_red = false;
  for (uint64_t seed = 1; seed <= 12 && !saw_red; ++seed) {
    BuildRoom(GameMode::kRiichi, seed);
    const std::optional<json::Value> start = ReadyAndTakeGameStart();
    ASSERT_TRUE(start.has_value());
    const json::Value* hand = start->Find("hand");
    ASSERT_NE(hand, nullptr);
    for (const json::Value& tile : hand->AsArray()) {
      const std::string text = tile.AsString();
      ASSERT_EQ(text.size(), 2u) << text;
      const bool red = text[0] == '0';
      if (red) {
        // 只有 5 才有赤牌：0m / 0p / 0s
        EXPECT_EQ(text[1] == 'z', false) << text;
        saw_red = true;
      }
      EXPECT_NE(std::string("0123456789").find(text[0]), std::string::npos) << text;
      EXPECT_NE(std::string("mpsz").find(text[1]), std::string::npos) << text;
    }
  }
  EXPECT_TRUE(saw_red) << "12 个种子都没发出赤 5，赤牌没有进牌山";

  BuildRoom(GameMode::kClassic, /*seed=*/1);
  const std::optional<json::Value> classic = ReadyAndTakeGameStart();
  ASSERT_TRUE(classic.has_value());
  for (const json::Value& tile : classic->Find("hand")->AsArray()) {
    const std::string text = tile.AsString();
    ASSERT_EQ(text.size(), 2u) << text;
    EXPECT_NE(text[0], '0') << "classic 帧不得出现 `0m`/`0p`/`0s`（逐字节不变）";
  }
}

// 日麻 game_end.win 的自洽性（v1.18）：`han` 必须等于 `Σ yaku[].han`（双倍役满记 26 也成立），
// 且新的 `aka_dora` 字段必须存在（没有赤牌就是 0）。
TEST_F(RiichiRoomTest, WinFrameCarriesAkaDoraAndConsistentHan) {
  BuildRoom(GameMode::kRiichi, /*seed=*/20260101);
  Send(RoomEnvelope::Ready(human_));

  std::vector<std::string> my_hand;
  bool need_answer = false;
  uint64_t seq = 0;
  int window = 0;
  std::optional<json::Value> win_frame;
  std::optional<json::Value> win_body;

  for (int guard = 0; guard < 4000 && !win_body.has_value() && !room_->game_finished(); ++guard) {
    for (json::Value& frame : sink_->Drain()) {
      const protocol::FrameType type = TypeOf(frame);
      if (type == protocol::FrameType::kGameStart || type == protocol::FrameType::kStateSync) {
        const json::Value* hand = frame.Find("hand");
        if (hand != nullptr && hand->IsArray()) {
          my_hand.clear();
          for (const json::Value& tile : hand->AsArray()) {
            my_hand.push_back(tile.AsString());
          }
        }
        continue;
      }
      if (type == protocol::FrameType::kActionRequest) {
        const json::Value* seq_value = frame.Find("seq");
        const json::Value* window_value = frame.Find("window");
        need_answer = seq_value != nullptr && window_value != nullptr;
        if (need_answer) {
          seq = static_cast<uint64_t>(seq_value->AsInt());
          window = static_cast<int>(window_value->AsInt());
        }
        continue;
      }
      if (type == protocol::FrameType::kGameEnd) {
        const json::Value* win = frame.Find("win");
        if (win != nullptr && win->IsObject()) {
          win_body = *win;  // json::Value 是可拷贝的轻量视图
        }
      }
    }
    if (!need_answer) {
      stdexec::sync_wait(ActorSendScope().on_empty());
      continue;
    }
    need_answer = false;

    json::Value action = json::Value::Obj(5);
    action.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
    if (window == protocol::WireOf(protocol::Window::kDiscard) && !my_hand.empty()) {
      action.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kDiscard)));
      action.Set("tile", json::Value::Str(MostIsolatedTile(my_hand)));
    } else {
      action.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kAction)));
      action.Set("action", json::Value::Int(protocol::WireOf(protocol::Action::kPass)));
      action.Set("tiles", json::Value::Arr(0));
    }
    Send(RoomEnvelope::GameFrame(human_, std::move(action)));
  }

  ASSERT_TRUE(win_body.has_value()) << "一局都没和牌，取不到 game_end.win";
  const json::Value* aka = win_body->Find("aka_dora");
  ASSERT_NE(aka, nullptr) << "日麻的 win 必须带 aka_dora（v1.18 §12.5）";
  EXPECT_GE(aka->AsInt(), 0);

  const json::Value* han = win_body->Find("han");
  ASSERT_NE(han, nullptr);
  int sum = 0;
  const json::Value* yaku = win_body->Find("yaku");
  ASSERT_NE(yaku, nullptr);
  for (const json::Value& item : yaku->AsArray()) {
    const json::Value* value = item.Find("han");
    ASSERT_NE(value, nullptr);
    sum += value->AsInt();
  }
  EXPECT_EQ(han->AsInt(), sum) << "win.han 必须等于 Σ yaku[].han（双倍役满记 26）";
}

// 半庄真的会打到南场（v1.14）：托管整场，逐手采样「局号 → 场风」
TEST_F(RiichiRoomTest, HanchanPlaysThroughSouthRound) {
  BuildRoom(GameMode::kRiichi, /*seed=*/20260101, GameLength::kHanchan);
  Send(RoomEnvelope::Ready(human_));

  std::map<std::pair<int, int>, int> seen;  // (round_index, round_wind) -> 采样次数
  std::vector<std::string> my_hand;
  bool need_answer = false;
  uint64_t seq = 0;
  int window = 0;

  for (int guard = 0; guard < 20000 && !room_->game_finished(); ++guard) {
    for (json::Value& frame : sink_->Drain()) {
      const protocol::FrameType type = TypeOf(frame);
      if (type == protocol::FrameType::kGameStart || type == protocol::FrameType::kStateSync) {
        const json::Value* hand = frame.Find("hand");
        if (hand != nullptr && hand->IsArray()) {
          my_hand.clear();
          for (const json::Value& tile : hand->AsArray()) {
            my_hand.push_back(tile.AsString());
          }
        }
        continue;
      }
      if (type == protocol::FrameType::kActionRequest) {
        const json::Value* seq_value = frame.Find("seq");
        const json::Value* window_value = frame.Find("window");
        need_answer = seq_value != nullptr && window_value != nullptr;
        if (need_answer) {
          seq = static_cast<uint64_t>(seq_value->AsInt());
          window = static_cast<int>(window_value->AsInt());
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
    if (window == protocol::WireOf(protocol::Window::kDiscard) && !my_hand.empty()) {
      action.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kDiscard)));
      action.Set("tile", json::Value::Str(MostIsolatedTile(my_hand)));
    } else {
      action.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kAction)));
      action.Set("action", json::Value::Int(protocol::WireOf(protocol::Action::kPass)));
      action.Set("tiles", json::Value::Arr(0));
    }
    Send(RoomEnvelope::GameFrame(human_, std::move(action)));
  }

  ASSERT_TRUE(room_->game_finished()) << "半庄没能在自动应答下跑完";
  EXPECT_EQ(room_->round_count(), 8);
  EXPECT_GE(room_->round_index(), 8) << "要走满 8 局才场终";

  // 每一局的场风都必须由局号唯一决定：0..3 东场、4..7 南场
  for (const auto& [key, count] : seen) {
    EXPECT_EQ(key.second, key.first / riichi::kRoundsPerWind) << "round_index=" << key.first << " 的场风算错了";
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

// 击飞（v1.19，雀魂口径）：任一持点 < 0 ⇒ 本场立即终局，不再开下一局。
// 用 SetPointsForTest 把座位 3 直接打到 -100000（任何一手都救不回来），半庄本该打 8 局，
// 这里必须在第 1 局收场；负点照实保留（箱下不截断，截成 0 就说明算错了）。
// 「恰好 0 点不算击飞」由 `riichi_rules_test` 的 RiichiBust.OnlyNegativePointsBust 覆盖：
// 房间层拿不到「结算后仍恰好 0 点」的确定性局面（任何一局的收付都会把 0 点推走）。
TEST_F(RiichiRoomTest, BustEndsMatchImmediately) {
  BuildRoom(GameMode::kRiichi, /*seed=*/20260101, GameLength::kHanchan);
  Send(RoomEnvelope::Ready(human_));
  room_->SetPointsForTest({25000, 25000, 25000, -100000});

  std::vector<std::string> my_hand;
  bool need_answer = false;
  uint64_t seq = 0;
  int window = 0;
  std::optional<json::Value> last_end;
  int game_ends = 0;

  // 先排空再判终局：最后一局的 game_end 与 match_end 是在同一次 Send 里送达的，
  // 若把终局判定写进 for 条件，就会在 drain 之前跳出、拿不到那两份帧。
  for (int guard = 0; guard < 4000; ++guard) {
    for (json::Value& frame : sink_->Drain()) {
      const protocol::FrameType type = TypeOf(frame);
      if (type == protocol::FrameType::kGameStart || type == protocol::FrameType::kStateSync) {
        const json::Value* hand = frame.Find("hand");
        if (hand != nullptr && hand->IsArray()) {
          my_hand.clear();
          for (const json::Value& tile : hand->AsArray()) {
            my_hand.push_back(tile.AsString());
          }
        }
        continue;
      }
      if (type == protocol::FrameType::kActionRequest) {
        const json::Value* seq_value = frame.Find("seq");
        const json::Value* window_value = frame.Find("window");
        need_answer = seq_value != nullptr && window_value != nullptr;
        if (need_answer) {
          seq = static_cast<uint64_t>(seq_value->AsInt());
          window = static_cast<int>(window_value->AsInt());
        }
        continue;
      }
      if (type == protocol::FrameType::kGameEnd) {
        ++game_ends;
        last_end = frame;
      }
    }
    if (room_->game_finished()) {
      break;
    }
    if (!need_answer) {
      stdexec::sync_wait(ActorSendScope().on_empty());
      continue;
    }
    need_answer = false;

    json::Value action = json::Value::Obj(5);  // type, seq + (discard tile | action/tiles)
    action.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
    if (window == protocol::WireOf(protocol::Window::kDiscard) && !my_hand.empty()) {
      action.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kDiscard)));
      action.Set("tile", json::Value::Str(MostIsolatedTile(my_hand)));
    } else {
      action.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kAction)));
      action.Set("action", json::Value::Int(protocol::WireOf(protocol::Action::kPass)));
      action.Set("tiles", json::Value::Arr(0));
    }
    Send(RoomEnvelope::GameFrame(human_, std::move(action)));
  }

  ASSERT_TRUE(room_->game_finished()) << "持点已经是负的了，本场却没终局";
  EXPECT_EQ(game_ends, 1) << "击飞必须在被打穿的那一局收场，不能继续开下一局";
  EXPECT_EQ(room_->hands_played(), 1);
  EXPECT_EQ(room_->round_index(), 0) << "击飞不推进局号（连庄/轮庄一并跳过）";

  ASSERT_TRUE(last_end.has_value());
  const json::Value* cont = last_end->Find("match_will_continue");
  ASSERT_NE(cont, nullptr);
  EXPECT_FALSE(cont->AsBool());

  const json::Value* points = last_end->Find("points_after");
  ASSERT_NE(points, nullptr);
  ASSERT_EQ(points->AsArray().size(), 4u);
  const int busted = points->AsArray()[3].AsInt();
  EXPECT_LT(busted, 0) << "箱下：负点不得截断为 0，实际 " << busted;
}

// 场终剩余供托归一位（v1.20，雀魂口径）。
// 正常收场时台面上的立直棒只可能剩在「末局荒庄」这种局面里 —— 只要有人和牌，棒一定被收走。
// 种子 20260101 的东场末局正好荒庄，这里在东四局开局用测试缝塞 2 根棒，验四件事：
// ① game_end 报出 `kyoutaku_award`（座位 / 根数 / 点数）；② 这笔钱真的进了持点
//    （四家之和 = 起始 100000 + 注入的 2000 —— 注入不扣持点，见 SetKyoutakuForTest）；
// ③ 收棒的是**持点最高那家**；④ match_end 的 final_score / rank 与 points_after 一致，供托清零。
TEST_F(RiichiRoomTest, LeftoverKyoutakuGoesToFirstPlace) {
  BuildRoom(GameMode::kRiichi, /*seed=*/20260101, GameLength::kTonpu);
  Send(RoomEnvelope::Ready(human_));

  std::vector<std::string> my_hand;
  bool need_answer = false;
  uint64_t seq = 0;
  int window = 0;
  bool injected = false;
  std::optional<json::Value> last_end;
  std::optional<json::Value> match_end;

  for (int guard = 0; guard < 4000; ++guard) {
    for (json::Value& frame : sink_->Drain()) {
      const protocol::FrameType type = TypeOf(frame);
      if (type == protocol::FrameType::kGameStart || type == protocol::FrameType::kStateSync) {
        const json::Value* hand = frame.Find("hand");
        if (hand != nullptr && hand->IsArray()) {
          my_hand.clear();
          for (const json::Value& tile : hand->AsArray()) {
            my_hand.push_back(tile.AsString());
          }
        }
        continue;
      }
      if (type == protocol::FrameType::kActionRequest) {
        const json::Value* seq_value = frame.Find("seq");
        const json::Value* window_value = frame.Find("window");
        need_answer = seq_value != nullptr && window_value != nullptr;
        if (need_answer) {
          seq = static_cast<uint64_t>(seq_value->AsInt());
          window = static_cast<int>(window_value->AsInt());
        }
        continue;
      }
      if (type == protocol::FrameType::kGameEnd) {
        last_end = std::move(frame);
        continue;
      }
      if (type == protocol::FrameType::kMatchEnd) {
        match_end = std::move(frame);
      }
    }
    // 东四局（round_index 3 = 东场最后一局）开局塞 2 根棒：本场的收场局荒庄，棒一定留在台面。
    if (!injected && room_->round_index() == 3) {
      room_->SetKyoutakuForTest(2);
      injected = true;
    }
    if (room_->game_finished()) {
      break;
    }
    if (!need_answer) {
      stdexec::sync_wait(ActorSendScope().on_empty());
      continue;
    }
    need_answer = false;

    json::Value action = json::Value::Obj(5);  // type, seq + (discard tile | action/tiles)
    action.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
    if (window == protocol::WireOf(protocol::Window::kDiscard) && !my_hand.empty()) {
      action.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kDiscard)));
      action.Set("tile", json::Value::Str(MostIsolatedTile(my_hand)));
    } else {
      action.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kAction)));
      action.Set("action", json::Value::Int(protocol::WireOf(protocol::Action::kPass)));
      action.Set("tiles", json::Value::Arr(0));
    }
    Send(RoomEnvelope::GameFrame(human_, std::move(action)));
  }

  ASSERT_TRUE(injected) << "东四局根本没开始，测试前提没成立";
  ASSERT_TRUE(room_->game_finished());
  ASSERT_TRUE(last_end.has_value());
  ASSERT_TRUE(match_end.has_value());
  // 前提复核：只有当末局没人收供托时，才会有棒留给场终（这里断言的正是那句规则）。
  const json::Value* reason = last_end->Find("reason");
  ASSERT_NE(reason, nullptr);
  ASSERT_EQ(reason->AsInt(), protocol::WireOf(protocol::RoundEndReason::kDrawExhausted))
      << "种子 20260101 的东场末局本应荒庄，实际 reason=" << reason->AsInt();

  const json::Value* award = last_end->Find("kyoutaku_award");
  ASSERT_NE(award, nullptr) << "日麻的 game_end 必须带 kyoutaku_award（无则 null）";
  ASSERT_FALSE(award->IsNull()) << "场终还剩 2 根棒，却没有报出归属";
  const json::Value* award_seat_value = award->Find("seat");
  const json::Value* award_sticks = award->Find("sticks");
  const json::Value* award_points = award->Find("points");
  ASSERT_NE(award_seat_value, nullptr);
  ASSERT_NE(award_sticks, nullptr);
  ASSERT_NE(award_points, nullptr);
  const int award_seat = static_cast<int>(award_seat_value->AsInt());
  EXPECT_EQ(award_sticks->AsInt(), 2);
  EXPECT_EQ(award_points->AsInt(), 2000);

  const json::Value* points = last_end->Find("points_after");
  ASSERT_NE(points, nullptr);
  ASSERT_EQ(points->AsArray().size(), 4u);
  int sum = 0;
  int top = points->AsArray()[0].AsInt();
  for (const json::Value& value : points->AsArray()) {
    sum += static_cast<int>(value.AsInt());
    top = std::max(top, static_cast<int>(value.AsInt()));
  }
  // 起始 4 × 25000 = 100000；注入的 2 根棒没有从任何人身上扣过，所以验算要把它加回去。
  EXPECT_EQ(sum, 100000 + 2000) << "场供没有回到持点里（点数不守恒）";
  EXPECT_EQ(points->AsArray()[static_cast<size_t>(award_seat)].AsInt(), top) << "场供必须归持点最高的一家";

  // 最后一局的 delta 里要能看见这 2000（客户端按 delta_score 播报，不能只改总分）
  const json::Value* results = last_end->Find("results");
  ASSERT_NE(results, nullptr);
  ASSERT_EQ(results->AsArray().size(), 4u);
  const json::Value* winner_row = nullptr;
  for (const json::Value& row : results->AsArray()) {
    const json::Value* seat_value = row.Find("seat");
    if (seat_value != nullptr && seat_value->AsInt() == award_seat) {
      winner_row = &row;
    }
  }
  ASSERT_NE(winner_row, nullptr);
  const json::Value* delta = winner_row->Find("delta_score");
  ASSERT_NE(delta, nullptr);
  EXPECT_GE(delta->AsInt(), 2000) << "供托归一位的点数没有落进本局 delta";

  // match_end 与 game_end 必须同源：一位的 final_score 就是 points_after，且供托已清零
  const json::Value* rankings = match_end->Find("rankings");
  ASSERT_NE(rankings, nullptr);
  ASSERT_EQ(rankings->AsArray().size(), 4u);
  const json::Value& first = rankings->AsArray()[0];
  ASSERT_TRUE(first.Find("seat") != nullptr);
  EXPECT_EQ(first.Find("seat")->AsInt(), award_seat) << "场供归了一位，一位却没变 —— 两者对不上";
  EXPECT_EQ(first.Find("final_score")->AsInt(), points->AsArray()[static_cast<size_t>(award_seat)].AsInt());
  EXPECT_EQ(room_->kyoutaku(), 0) << "场终之后供托必须清零";
}

}  // namespace
}  // namespace mahjong
