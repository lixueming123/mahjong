// RiichiMahjongGame driven directly — no room, no actor, no socket.
//
// Two things are proven here and nowhere else:
//
//   1. **The game is actor-free.** This file never constructs a RoomActor, an ActorRef or a
//      mailbox: it calls Setup/Apply/OnTimeout and reads the frames out of the returned
//      GameStep. classic_mahjong_game_test proves the same for the other game; that the two
//      files exist independently is the point of the plugin split.
//   2. **The port is byte-exact.** The three hashes below are the riichi half of
//      mahjong_room_test's GoldenTranscriptIsFrozen, frozen *before* room.cc was split. They
//      were produced by the old monolithic room; reproducing them here means the split changed
//      no byte of any frame for any seat across a whole 4-round match — including every
//      riichi-only field (points / dora_indicators / fu / limit / ura_dora / kyoutaku_award).
//
// The driver mirrors the old fixture's step loop exactly (answer the human's window, otherwise
// let it expire; round advance and match end are fired from the recorded deadlines) because the
// transcript's cross-seat order is a property of that loop, not of the frames. In particular the
// human policy never declares riichi and never answers a chankan window — the frozen transcript
// was produced by a policy that did not, so changing it here would only change the hash.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "game_test_support.h"
#include "protocol.h"
#include "riichi/riichi_mahjong_game.h"

namespace mahjong {
namespace {

using game_test::Fnv1a64;
using game_test::IntOf;
using game_test::OptionOf;
using game_test::StrOf;
using game_test::TilesOf;
using game_test::TypeOf;

constexpr int kSeat0 = 0;
constexpr int kActionTimeoutMs = 15000;
// 東場（4 局）：冻结的那六条 golden 里日麻三场用的都是 tonpu。
constexpr char kTonpuOptions[] = R"({"length":0})";

// One whole match, played through the Game interface alone.
class Driver {
 public:
  explicit Driver(uint64_t seed) {
    GameContext context;
    context.room_id = "r-0001";
    context.seed = seed;
    context.action_timeout_ms = kActionTimeoutMs;
    context.inter_hand_delay_ms = 0;  // the golden was frozen with no interlude
    context.ai_delay_ms = 0;          // ... and with bots deciding inline
    context.deadlines_supported = true;
    context.options_json = kTonpuOptions;
    for (int seat = 0; seat < kSeatCount; ++seat) {
      // The frozen transcript carries these names (state_sync.seats[].name), so they are part of
      // the golden: seat 0 is the human, the rest are "AI-<seat>".
      context.seats.push_back(GameSeat {.seat = seat,
                                        .is_bot = seat != kSeat0,
                                        .wired = true,  // every seat's frames must be captured
                                        .player_id = seat == kSeat0 ? "p-human" : "bot-" + std::to_string(seat),
                                        .name = seat == kSeat0 ? "Human" : "AI-" + std::to_string(seat)});
    }
    Deliver(game_.Setup(context));
  }

  RiichiMahjongGame& game() { return game_; }

  int PlayFullGame(int max_steps = 20000) {
    int steps = 0;
    while (!game_.game_finished() && steps < max_steps) {
      if (!Step()) {
        break;
      }
      ++steps;
    }
    return steps;
  }

  // One seat's frames, in delivery order, as the exact bytes the room would have sent.
  const std::vector<std::string>& frames_of(int seat) const { return frames_[static_cast<size_t>(seat)]; }

  // What the room would have reported to its owner, one entry per settled round.
  const std::vector<GameResult>& results() const { return results_; }

 private:
  void Deliver(GameStep step) {
    for (const Outbound& outbound : step.outbounds) {
      if (outbound.recipients.empty()) {
        for (int seat = 0; seat < kSeatCount; ++seat) {
          frames_[static_cast<size_t>(seat)].push_back(outbound.frame);
        }
        continue;
      }
      for (const int seat : outbound.recipients) {
        frames_[static_cast<size_t>(seat)].push_back(outbound.frame);
      }
    }
    for (const Deadline& deadline : step.deadlines) {
      scheduled_.emplace_back(deadline.delay_ms, deadline.token);
    }
    for (const GameResult& result : step.results) {
      results_.push_back(result);
    }
  }

  // Drains everything seat 0 has received since the last call and returns the request that
  // still belongs to the open window (if any) — the same contract as the room fixture's
  // PendingRequest(), including tracking the draw record the discard policy needs.
  std::optional<json::Value> PendingRequest() {
    std::optional<json::Value> pending;
    for (size_t index = seat0_consumed_; index < frames_[kSeat0].size(); ++index) {
      const std::optional<json::Value> frame = json::Value::Parse(frames_[kSeat0][index], nullptr);
      if (!frame.has_value()) {
        continue;
      }
      const protocol::FrameType type = TypeOf(*frame);
      if (type == protocol::FrameType::kDraw) {
        drawn_ = ParseTile(StrOf(*frame, "tile"));
      }
      if (type == protocol::FrameType::kActionRequest && static_cast<uint64_t>(IntOf(*frame, "seq")) == game_.seq()) {
        pending = *frame;
      }
    }
    seat0_consumed_ = frames_[kSeat0].size();
    return pending;
  }

  void SendDiscard(Tile tile, uint64_t seq) {
    json::Value frame = json::Value::Obj();
    frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kDiscard)));
    frame.Set("tile", json::Value::Str(FormatTile(tile)));
    frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
    Deliver(game_.Apply(kSeat0, frame));
  }

  void SendAction(protocol::Action action, const std::vector<Tile>& tiles, uint64_t seq) {
    json::Value frame = json::Value::Obj();
    frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kAction)));
    frame.Set("action", json::Value::Int(protocol::WireOf(action)));
    json::Value tile_array = json::Value::Arr();
    for (Tile tile : tiles) {
      tile_array.Append(json::Value::Str(FormatTile(tile)));
    }
    frame.Set("tiles", std::move(tile_array));
    frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
    Deliver(game_.Apply(kSeat0, frame));
  }

  // The frozen policy: take any win on offer, otherwise discard by tile efficiency; in a claim
  // window take a win, else a pong, else pass. A chankan window is left to expire, exactly as
  // the room fixture did — see the file header.
  bool AnswerWindow() {
    const std::optional<json::Value> request = PendingRequest();
    const protocol::Window window = game_.CurrentWindow();
    const uint64_t seq = game_.seq();
    if (window == protocol::Window::kDiscard && game_.current_seat() == kSeat0) {
      if (request.has_value() && OptionOf(*request, protocol::Action::kWin) != nullptr) {
        SendAction(protocol::Action::kWin, {}, seq);
      } else {
        std::optional<Tile> chosen = ChooseDiscard(game_.hand_of(kSeat0), game_.melds_of(kSeat0), drawn_);
        const std::vector<Tile> hand = SortedTiles(game_.hand_of(kSeat0));
        SendDiscard(chosen.value_or(hand.back()), seq);
        drawn_.reset();
      }
      return true;
    }
    if (!request.has_value() || IntOf(*request, "window") != protocol::WireOf(protocol::Window::kClaim)) {
      return false;
    }
    if (OptionOf(*request, protocol::Action::kWin) != nullptr) {
      SendAction(protocol::Action::kWin, {}, seq);
    } else if (const json::Value* pong = OptionOf(*request, protocol::Action::kPong); pong != nullptr) {
      SendAction(protocol::Action::kPong, TilesOf(*pong, "tiles"), seq);
    } else {
      SendAction(protocol::Action::kPass, {}, seq);
    }
    return true;
  }

  uint64_t TakeDeadline(bool (*match)(uint64_t)) {
    for (size_t index = scheduled_.size(); index > 0; --index) {
      if (match(scheduled_[index - 1].second)) {
        const uint64_t token = scheduled_[index - 1].second;
        scheduled_.erase(scheduled_.begin() + static_cast<std::ptrdiff_t>(index - 1));
        return token;
      }
    }
    return 0;
  }

  bool FireDeadline(bool (*match)(uint64_t)) {
    const uint64_t token = TakeDeadline(match);
    if (token == 0) {
      return false;
    }
    Deliver(game_.OnTimeout(token));
    return true;
  }

  bool AdvanceOpenWindow() {
    if (game_.game_finished()) {
      return false;
    }
    const uint64_t before = game_.seq();
    Deliver(game_.OnTimeout(before));
    return game_.game_finished() || game_.seq() != before;
  }

  bool Step() {
    if (game_.game_finished()) {
      return false;
    }
    if (game_.round_finished()) {
      if (FireDeadline(&IsRoundAdvanceToken)) {
        return true;
      }
      return FireDeadline(&IsMatchEndToken);
    }
    if (AnswerWindow()) {
      return true;
    }
    if (FireDeadline(&IsAiPacingToken)) {
      return true;
    }
    return AdvanceOpenWindow();
  }

  RiichiMahjongGame game_;
  std::array<std::vector<std::string>, kSeatCount> frames_;
  std::vector<std::pair<int64_t, uint64_t>> scheduled_;
  std::vector<GameResult> results_;
  std::optional<Tile> drawn_;
  size_t seat0_consumed_ = 0;
};

// The same transcript the room-level golden folds: seat 0's stream first, then seats 1..3, with
// the one real-clock field (`action_request.deadline_ms`) zeroed.
std::string TranscriptOf(Driver& driver) {
  std::string dump;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    for (const std::string& frame : driver.frames_of(seat)) {
      dump += std::to_string(seat);
      dump += '\t';
      dump += frame;
      dump += '\n';
    }
  }
  return std::regex_replace(dump, std::regex("\"deadline_ms\":[0-9]+"), "\"deadline_ms\":0");
}

TEST(RiichiMahjongGameTest, GoldenTranscriptIsFrozen) {
  struct Golden {
    uint64_t seed;
    const char* name;
    uint64_t expected;
  };
  // Frozen by room.cc's GoldenTranscriptIsFrozen before the split. Same seed, same policy, same
  // four seats, tonpu — a different number here means the port changed the wire.
  const Golden cases[] = {
      {20251005, "riichi/seed-20251005", 0xf0a019a1abcb98acull},
      {7, "riichi/seed-7", 0xa97f495a097f5634ull},
      {424242, "riichi/seed-424242", 0xf905a3d9c4067481ull},
  };
  for (const Golden& golden : cases) {
    Driver driver(golden.seed);
    driver.PlayFullGame();
    ASSERT_TRUE(driver.game().game_finished()) << golden.name << " 未能打完一整场";
    const std::string transcript = TranscriptOf(driver);
    const uint64_t hash = Fnv1a64(transcript);
    const size_t frames = static_cast<size_t>(std::count(transcript.begin(), transcript.end(), '\n'));
    std::printf("[golden] %-22s frames=%5zu hash=0x%016llx\n", golden.name, frames,
                static_cast<unsigned long long>(hash));
    EXPECT_EQ(hash, golden.expected) << golden.name << "（frames=" << frames << "）";
  }
}

// A whole match reports exactly one settled round per game_end, and only the last one says the
// match is over.
TEST(RiichiMahjongGameTest, OneResultPerRoundAndOnlyTheLastEndsTheMatch) {
  Driver driver(20251005);
  driver.PlayFullGame();
  ASSERT_TRUE(driver.game().game_finished());
  const std::vector<GameResult>& results = driver.results();
  EXPECT_EQ(static_cast<int>(results.size()), driver.game().hands_played());
  ASSERT_FALSE(results.empty());
  EXPECT_TRUE(results.back().match_over);
  for (size_t index = 0; index + 1 < results.size(); ++index) {
    EXPECT_FALSE(results[index].match_over) << "第 " << index << " 局不该结束整场";
  }
  // 東場 4 局 + 连庄：不下于 4 局，且不会有第 5 个不同的 round_index。
  EXPECT_GE(driver.game().hands_played(), 4);
  EXPECT_EQ(driver.game().round_count(), 4);
}

// 日麻的核心不变量：持点恒 25000×4 = 100000。立直棒只是从持点挪到供托，没有任何一帧能造牌。
TEST(RiichiMahjongGameTest, PointsAreConservedAtMatchEnd) {
  for (uint64_t seed : {20251005ull, 7ull, 424242ull}) {
    Driver driver(seed);
    driver.PlayFullGame();
    ASSERT_TRUE(driver.game().game_finished()) << "seed " << seed;
    int total = 0;
    for (int seat = 0; seat < kSeatCount; ++seat) {
      total += driver.game().points_of(seat);
    }
    EXPECT_EQ(total, riichi::kStartPoints * kSeatCount) << "seed " << seed << ": 四家持点之和必须恒为 100000";
    // 场终把手上的供托全归了一位，所以终局不该还剩棒。
    EXPECT_EQ(driver.game().kyoutaku(), 0) << "seed " << seed;
  }
}

// 王牌不发给客户端，但服务器自己必须切对：14 张，指示牌从 1 张起、每杠多翻一张、最多 5 张。
TEST(RiichiMahjongGameTest, DeadWallIsFourteenTilesAndIndicatorsGrowWithKans) {
  Driver driver(20251005);
  // Setup 已经发好第一局：王牌、指示牌都该就位。
  EXPECT_EQ(driver.game().dead_wall_of().size(), size_t {14});
  EXPECT_EQ(driver.game().dora_indicators_of().size(), size_t {1});
  driver.PlayFullGame();
  ASSERT_TRUE(driver.game().game_finished());
  EXPECT_LE(driver.game().dora_indicators_of().size(), size_t {5});
}

// The wall never leaves the server: no frame may carry the seed, the wall or the dead wall.
TEST(RiichiMahjongGameTest, NoFrameCarriesTheWallOrTheSeed) {
  Driver driver(20251005);
  driver.PlayFullGame();
  for (int seat = 0; seat < kSeatCount; ++seat) {
    for (const std::string& frame : driver.frames_of(seat)) {
      EXPECT_EQ(frame.find("\"wall\""), std::string::npos) << frame;
      EXPECT_EQ(frame.find("\"dead_wall\""), std::string::npos) << frame;
      EXPECT_EQ(frame.find("\"seed\""), std::string::npos) << frame;
    }
  }
}

// 日麻和牌的 `win` 载荷必须带符、点数、满贯分档——这三个是 classic 的 `win` 里没有的键，
// 少一个客户端结算页就只显示番数。
TEST(RiichiMahjongGameTest, WinPayloadCarriesFuPointsAndLimit) {
  int wins = 0;
  for (uint64_t seed : {20251005ull, 7ull, 424242ull}) {
    Driver driver(seed);
    driver.PlayFullGame();
    ASSERT_TRUE(driver.game().game_finished()) << "seed " << seed;
    for (int seat = 0; seat < kSeatCount; ++seat) {
      for (const std::string& text : driver.frames_of(seat)) {
        const std::optional<json::Value> frame = json::Value::Parse(text, nullptr);
        if (!frame.has_value() || TypeOf(*frame) != protocol::FrameType::kGameEnd) {
          continue;
        }
        const json::Value* win = frame->Find("win");
        if (win == nullptr || !win->IsObject()) {
          continue;  // 荒庄
        }
        ++wins;
        EXPECT_GT(IntOf(*win, "fu"), 0) << text;
        EXPECT_GT(IntOf(*win, "points"), 0) << text;
        // limit 是必带键：0 = kNone（满贯以下），其余是满贯/跳满/倍满/三倍满/役满。
        EXPECT_NE(win->Find("limit"), nullptr) << text;
        EXPECT_GT(IntOf(*win, "total"), 0) << text;
      }
    }
  }
  ASSERT_GT(wins, 0) << "三个种子里一次和牌都没有，这条断言就失去意义";
}

// 立直是公开信息，但只公开「有没有立直」这一位：四家看到的是同一份，手牌依然是每人只看
// 自己那一份。这条用例同时证明**机器人会立直**（`PlayAutomatedDiscard` 里那段日麻独有的
// 宣言逻辑）——golden 那三个种子里一次都没出现立直，所以它覆盖不到这里。
TEST(RiichiMahjongGameTest, DeclaredRiichiIsPublicButHandsStayPrivate) {
  // 先找一个真的出现立直的种子：机器人立直需要「门清形听 + 点数够 + 打掉某张仍听」，
  // 不是每个种子都会碰到。
  std::optional<uint64_t> seed_with_riichi;
  for (uint64_t seed = 1; seed <= 40 && !seed_with_riichi.has_value(); ++seed) {
    Driver driver(seed);
    driver.PlayFullGame();
    for (int seat = 0; seat < kSeatCount && !seed_with_riichi.has_value(); ++seat) {
      for (const std::string& text : driver.frames_of(seat)) {
        if (text.find("\"riichi\":true") != std::string::npos) {
          seed_with_riichi = seed;
          break;
        }
      }
    }
  }
  ASSERT_TRUE(seed_with_riichi.has_value()) << "40 个种子里一次立直都没有，机器人立直那段逻辑就是死代码";

  Driver driver(*seed_with_riichi);
  driver.PlayFullGame();
  ASSERT_TRUE(driver.game().game_finished());
  for (int seat = 0; seat < kSeatCount; ++seat) {
    for (const std::string& text : driver.frames_of(seat)) {
      const std::optional<json::Value> frame = json::Value::Parse(text, nullptr);
      if (!frame.has_value() || TypeOf(*frame) != protocol::FrameType::kStateSync) {
        continue;
      }
      // 每一份 state_sync 都必须带自己的 hand，且 seats[] 里四家都在（含 riichi 位）。
      const json::Value* hand = frame->Find("hand");
      ASSERT_NE(hand, nullptr) << text;
      EXPECT_TRUE(hand->IsArray());
      const json::Value* seats = frame->Find("seats");
      ASSERT_NE(seats, nullptr) << text;
      ASSERT_TRUE(seats->IsArray());
      EXPECT_EQ(seats->AsArray().size(), size_t {kSeatCount}) << text;
      for (const json::Value& entry : seats->AsArray()) {
        EXPECT_NE(entry.Find("riichi"), nullptr) << text;
        // H6：别人的手牌永远不出现在 seats[] 里，只有张数。
        EXPECT_EQ(entry.Find("hand"), nullptr) << text;
        EXPECT_NE(entry.Find("hand_count"), nullptr) << text;
      }
    }
  }
}

}  // namespace
}  // namespace mahjong

int main(int argc, char** argv) {
  spdlog::set_level(spdlog::level::warn);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
