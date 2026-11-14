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

#include "riichi/riichi_mahjong_game.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "game_test_support.h"
#include "protocol.h"

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
  // 注意：这条转录同时冻结了**机器人策略**（Driver 里那个「人类」座位的打牌/鸣牌选择也是
  // 冻结的），所以调整 AI 决策会让它变红，那不一定是协议改动。这三个值是在把机器人弃牌
  // 改成「并列处按役/振听决胜」、并给立直加手牌锁之后重新冻结的（classic 的同款 golden 未变，
  // 共享的 `mahjong_rules::ChooseDiscard` 没有被改动）。
  const Golden cases[] = {
      {20251005, "riichi/seed-20251005", 0xb5e73d8de8662cafull},
      {7, "riichi/seed-7", 0xbdc6d6a4584e8977ull},
      {424242, "riichi/seed-424242", 0xd74c3ebf444db32full},
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

// 机器人在立直之后必须锁手牌（摸切）。`ValidateDiscard` 只对真人执行这条规则，而 AI 不走
// Validate —— 少了这个分支，机器人会继续按牌效率换牌，那是日麻不允许的。
TEST(RiichiMahjongGameTest, BotRiichiLocksItsHandToTheDrawnTile) {
  // 逐种子重放：单个种子里「机器人立直后又摸到能改手的牌」不一定出现，所以要多跑几局。
  // 只要有一个座位在立直后改了手牌就失败 —— 回滚这段逻辑时这条用例必须变红。
  int violations = 0;
  int rounds = 0;
  int riichi_rounds = 0;
  for (uint64_t seed = 1; seed <= 12; ++seed) {
    RiichiMahjongGame game;
    GameContext context;
    context.room_id = "r-riichi-lock";
    context.seed = seed;
    context.action_timeout_ms = kActionTimeoutMs;
    context.inter_hand_delay_ms = 0;
    // 必须用非零 delay：ai_delay=0 时 `Pump` 会在**一条**消息里跑完整局，观察者只看得到
    // 段末状态，抓不到「立直后改了手牌」这种中间步骤。900 = 生产节奏，一次决策一步。
    context.ai_delay_ms = 900;
    context.deadlines_supported = true;
    context.options_json = kTonpuOptions;
    for (int seat = 0; seat < kSeatCount; ++seat) {
      context.seats.push_back(GameSeat {.seat = seat,
                                        .is_bot = true,
                                        .wired = false,
                                        .player_id = "bot-" + std::to_string(seat),
                                        .name = "AI-" + std::to_string(seat)});
    }
    std::vector<std::pair<int64_t, uint64_t>> scheduled;
    std::array<std::vector<Face>, kSeatCount> locked;
    std::array<bool, kSeatCount> have_lock {};
    bool saw_riichi = false;
    const auto drain = [&](const GameStep& step) {
      for (const Deadline& deadline : step.deadlines) {
        scheduled.emplace_back(deadline.delay_ms, deadline.token);
      }
      rounds += static_cast<int>(step.results.size());
      if (game.game_finished() || game.round_finished()) {
        have_lock.fill(false);
        for (auto& faces : locked) faces.clear();
      }
      for (int seat = 0; seat < kSeatCount; ++seat) {
        const std::vector<Tile>& hand = game.hand_of(seat);
        if (hand.size() + 3 * game.melds_of(seat).size() != 13) continue;
        if (!game.riichi_declared(seat)) {
          have_lock[static_cast<size_t>(seat)] = false;
          continue;
        }
        saw_riichi = true;
        std::vector<Face> faces;
        for (Tile tile : hand) faces.push_back(FaceOf(tile));
        std::sort(faces.begin(), faces.end());
        if (!have_lock[static_cast<size_t>(seat)]) {
          have_lock[static_cast<size_t>(seat)] = true;
          locked[static_cast<size_t>(seat)] = std::move(faces);
        } else if (faces != locked[static_cast<size_t>(seat)]) {
          ++violations;
          locked[static_cast<size_t>(seat)] = std::move(faces);
        }
      }
    };
    drain(game.Setup(context));
    int guard = 0;
    while (!game.game_finished() && guard++ < 200000) {
      if (game.round_finished()) {
        bool fired = false;
        for (size_t index = scheduled.size(); index > 0 && !fired; --index) {
          const uint64_t token = scheduled[index - 1].second;
          if (IsRoundAdvanceToken(token) || IsMatchEndToken(token)) {
            scheduled.erase(scheduled.begin() + static_cast<std::ptrdiff_t>(index - 1));
            drain(game.OnTimeout(token));
            fired = true;
          }
        }
        if (!fired) break;
        continue;
      }
      const uint64_t before = game.seq();
      drain(game.OnTimeout(before));
      if (game.seq() == before && !game.round_finished() && !game.game_finished()) {
        bool fired = false;
        for (size_t index = scheduled.size(); index > 0 && !fired; --index) {
          const uint64_t token = scheduled[index - 1].second;
          if (IsAiPacingToken(token)) {
            scheduled.erase(scheduled.begin() + static_cast<std::ptrdiff_t>(index - 1));
            drain(game.OnTimeout(token));
            fired = true;
          }
        }
        if (!fired) break;
      }
    }
    if (saw_riichi) ++riichi_rounds;
  }
  ASSERT_GT(rounds, 0);
  ASSERT_GT(riichi_rounds, 0) << "12 个种子里一次立直都没有，这条用例失去意义";
  EXPECT_EQ(violations, 0) << "机器人立直后改了手牌：立直手必须只摸切";
}

// 国士无双必须能真正和掉。窗口选项（`riichi::DiscardOptions`）早就把国士的 win 放进去了，
// 但落地校验曾经用共享的 `IsWinningHand`（番数制口径，不认国士）—— 于是「看得到胡、点了被拒」。
// 这条用例走完整的 Apply 路径，直接摆一手国士自摸。
TEST(RiichiMahjongGameTest, KokushiTsumoActuallyWins) {
  RiichiMahjongGame game;
  GameContext context;
  context.room_id = "r-kokushi";
  context.seed = 7;
  context.action_timeout_ms = kActionTimeoutMs;
  context.inter_hand_delay_ms = 0;
  context.ai_delay_ms = 0;
  context.deadlines_supported = true;
  context.options_json = kTonpuOptions;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    context.seats.push_back(GameSeat {.seat = seat,
                                      .is_bot = seat != kSeat0,
                                      .wired = true,
                                      .player_id = seat == kSeat0 ? "p-human" : "bot-" + std::to_string(seat),
                                      .name = seat == kSeat0 ? "Human" : "AI-" + std::to_string(seat)});
  }
  game.Setup(context);
  ASSERT_EQ(game.current_seat(), kSeat0) << "东一局庄家是 seat 0，起手就是它的打牌窗口";

  // 庄家起手 14 张摆成国士：十三种幺九字各一张 + 其中一种多一张。
  std::vector<Tile> hand;
  for (const char* text : {"1m", "9m", "1p", "9p", "1s", "9s", "1z", "2z", "3z", "4z", "5z", "6z", "7z", "1m"}) {
    const std::optional<Tile> tile = ParseTile(text);
    ASSERT_TRUE(tile.has_value()) << text;
    hand.push_back(*tile);
  }
  game.SetHandForTest(kSeat0, std::move(hand));

  // 窗口必须真的给出 win（否则客户端根本看不到「胡」按钮）。
  json::Value win = json::Value::Obj();
  win.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kAction)));
  win.Set("action", json::Value::Int(protocol::WireOf(protocol::Action::kWin)));
  win.Set("tiles", json::Value::Arr(0));
  win.Set("seq", json::Value::Int(static_cast<int64_t>(game.seq())));
  ASSERT_EQ(game.Validate(kSeat0, win).kind, GameVerdict::Kind::kAccept)
      << "国士的自摸必须在窗口里可选、且校验通过";

  const GameStep step = game.Apply(kSeat0, win);
  ASSERT_FALSE(step.results.empty()) << "国士自摸应当结算这一局";
  EXPECT_EQ(step.results.front().reason, protocol::RoundEndReason::kWinTsumo);
  EXPECT_EQ(step.results.front().winner_seat, kSeat0);
}

json::Value AutoplayFrame(bool on) {
  json::Value frame = json::Value::Obj();
  frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kAutoplay)));
  frame.Set("on", json::Value::Bool(on));
  return frame;
}

// Manual autoplay is a separate flag from is_bot. Turning it off hands the open window back;
// a bot seat ignores the frame, so is_bot cannot be cancelled.
TEST(RiichiMahjongGameTest, ManualAutoplayCanBeCancelledAndDoesNotChangeBots) {
  RiichiMahjongGame game;
  GameContext context;
  context.room_id = "r-autoplay";
  context.seed = 7;
  context.action_timeout_ms = kActionTimeoutMs;
  context.inter_hand_delay_ms = 0;
  context.ai_delay_ms = 900;
  context.deadlines_supported = true;
  context.options_json = kTonpuOptions;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    context.seats.push_back(GameSeat {.seat = seat,
                                      .is_bot = seat != kSeat0,
                                      .wired = true,
                                      .player_id = seat == kSeat0 ? "p-human" : "bot-" + std::to_string(seat),
                                      .name = seat == kSeat0 ? "Human" : "AI-" + std::to_string(seat)});
  }
  const GameStep setup = game.Setup(context);
  ASSERT_FALSE(setup.outbounds.empty());
  ASSERT_EQ(game.CurrentWindow(), protocol::Window::kDiscard);
  ASSERT_EQ(game.current_seat(), kSeat0);
  const uint64_t seq = game.seq();

  EXPECT_EQ(game.Validate(/*seat=*/1, AutoplayFrame(false)).kind, GameVerdict::Kind::kDrop);
  const GameStep bot_ignored = game.Apply(/*seat=*/1, AutoplayFrame(false));
  EXPECT_TRUE(bot_ignored.empty());
  EXPECT_EQ(game.seq(), seq);

  ASSERT_EQ(game.Validate(kSeat0, AutoplayFrame(true)).kind, GameVerdict::Kind::kAccept);
  const GameStep started = game.Apply(kSeat0, AutoplayFrame(true));
  EXPECT_EQ(game.seq(), seq);
  EXPECT_EQ(game.CurrentWindow(), protocol::Window::kDiscard);
  bool paced = false;
  for (const Deadline& deadline : started.deadlines) {
    if (IsAiPacingToken(deadline.token) && WindowSeqOfToken(deadline.token) == seq) {
      paced = true;
    }
  }
  EXPECT_TRUE(paced);
  bool saw_sync = false;
  for (const Outbound& outbound : started.outbounds) {
    const std::optional<json::Value> frame = json::Value::Parse(outbound.frame, nullptr);
    if (!frame.has_value()) {
      continue;
    }
    EXPECT_NE(TypeOf(*frame), protocol::FrameType::kDiscardEvent);
    if (TypeOf(*frame) != protocol::FrameType::kStateSync) {
      continue;
    }
    saw_sync = true;
    const json::Value* seats = frame->Find("seats");
    ASSERT_NE(seats, nullptr);
    for (const json::Value& entry : seats->AsArray()) {
      const int seat = static_cast<int>(IntOf(entry, "seat"));
      const json::Value* is_bot = entry.Find("is_bot");
      ASSERT_NE(is_bot, nullptr);
      if (seat == kSeat0) {
        EXPECT_FALSE(is_bot->AsBool());
        const json::Value* autoplay = entry.Find("autoplay");
        ASSERT_NE(autoplay, nullptr);
        EXPECT_TRUE(autoplay->AsBool());
      } else {
        EXPECT_TRUE(is_bot->AsBool());
        EXPECT_EQ(entry.Find("autoplay"), nullptr) << "seat " << seat;
      }
    }
  }
  EXPECT_TRUE(saw_sync);

  json::Value discard = json::Value::Obj();
  discard.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kDiscard)));
  discard.Set("tile", json::Value::Str(FormatTile(game.hand_of(kSeat0).back())));
  discard.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
  // The room never Apply-s a dropped frame. Doing so here would play the tile anyway.
  EXPECT_EQ(game.Validate(kSeat0, discard).kind, GameVerdict::Kind::kDrop);
  EXPECT_EQ(game.seq(), seq);

  const GameStep stopped = game.Apply(kSeat0, AutoplayFrame(false));
  EXPECT_EQ(game.seq(), seq);
  EXPECT_EQ(game.CurrentWindow(), protocol::Window::kDiscard);
  bool replayed = false;
  bool cleared = false;
  for (const Outbound& outbound : stopped.outbounds) {
    const std::optional<json::Value> frame = json::Value::Parse(outbound.frame, nullptr);
    if (!frame.has_value()) {
      continue;
    }
    if (TypeOf(*frame) == protocol::FrameType::kActionRequest && static_cast<uint64_t>(IntOf(*frame, "seq")) == seq) {
      replayed = true;
    }
    if (TypeOf(*frame) != protocol::FrameType::kStateSync) {
      continue;
    }
    const json::Value* seats = frame->Find("seats");
    ASSERT_NE(seats, nullptr);
    for (const json::Value& entry : seats->AsArray()) {
      if (IntOf(entry, "seat") != kSeat0) {
        EXPECT_EQ(entry.Find("autoplay"), nullptr);
        continue;
      }
      cleared = true;
      EXPECT_FALSE(entry.Find("is_bot")->AsBool());
      EXPECT_EQ(entry.Find("autoplay"), nullptr);
    }
  }
  EXPECT_TRUE(replayed);
  EXPECT_TRUE(cleared);

  const GameStep late = game.OnTimeout(MakeAiPacingToken(seq));
  EXPECT_TRUE(late.empty());
  EXPECT_EQ(game.seq(), seq);
  EXPECT_EQ(game.current_seat(), kSeat0);

  ASSERT_EQ(game.Validate(kSeat0, discard).kind, GameVerdict::Kind::kAccept);
  game.Apply(kSeat0, discard);
  EXPECT_NE(game.seq(), seq);
  EXPECT_FALSE(game.discards_of(kSeat0).empty());
}

}  // namespace
}  // namespace mahjong

int main(int argc, char** argv) {
  spdlog::set_level(spdlog::level::warn);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
