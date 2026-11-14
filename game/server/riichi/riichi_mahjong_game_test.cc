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
        // 喰い替え禁止（v2.5）：真人客户端也会避开被禁的牌面 —— 先按禁区剔除候选再选，
        // 与 AI 同口径（否则这条策略会撞上 ILLEGAL_ACTION，而 Apply 不重算合法性别）。
        std::vector<Tile> legal;
        const std::vector<Face> forbidden = game_.forbidden_discards_of(kSeat0);
        for (Tile tile : game_.hand_of(kSeat0)) {
          if (std::find(forbidden.begin(), forbidden.end(), FaceOf(tile)) == forbidden.end()) {
            legal.push_back(tile);
          }
        }
        if (legal.empty()) {
          legal = game_.hand_of(kSeat0);
        }
        const std::optional<Tile> drawn =
            drawn_.has_value() && std::find(legal.begin(), legal.end(), *drawn_) != legal.end() ? drawn_ : std::nullopt;
        std::optional<Tile> chosen = ChooseDiscard(legal, game_.melds_of(kSeat0), drawn);
        const std::vector<Tile> hand = SortedTiles(legal);
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
  // v2.5 喰い替え禁止：打牌窗口的 `action_request` 在**确实有禁区**时多带一个可选字段
  // `forbidden_discards`，帧数与每帧的其它字段都不变（frames 计数逐字相同），因此这里重新
  // 冻结三个值。Driver 的打牌策略也同步按禁区剔除候选 —— 否则它会发出必被拒的牌。
  const Golden cases[] = {
      {20251005, "riichi/seed-20251005", 0xcd856fafe4620256ull},
      {7, "riichi/seed-7", 0xa35ddd9f0a14d820ull},
      {424242, "riichi/seed-424242", 0x239d92b8e658a2f0ull},
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

// ---- 同巡振听（v1.22）--------------------------------------------------------
// 规则库那一半由 `riichi_rules_test` 的 `RiichiTemporaryFuriten` 覆盖；这里验房间层状态机：
// 放过和牌张 ⇒ 只挡那个牌面 ⇒ 自己摸牌或鸣牌后解除。

namespace {

// 一帧里某个键的布尔值（缺键 / 非布尔 ⇒ false）。
bool BoolOf(const json::Value& frame, const std::string& key) {
  const json::Value* value = frame.Find(key);
  return value != nullptr && value->IsBool() && value->AsBool();
}

// 一手「听 6s/9s 两面」的门前 13 张（打 1s 之后听）。
std::vector<Tile> TemporaryFuritenHand() {
  std::vector<Tile> hand;
  for (const char* text : {"2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "4p", "7s", "8s", "1s", "9p", "9p"}) {
    hand.push_back(*ParseTile(text));
  }
  return hand;
}

// 全 bot 的桌面驱动器。`ai_delay_ms = 900` 是必须的：设为 0 时 `Pump` 会在 `Setup` 里
// 一口气跑完整局，测试就观察不到中间的窗口与状态变化。这里显式地逐个放行 AI 决策令牌，
// 让每一步都成为可检查的中间态。
class BotTable {
 public:
  // `wired_seat` >= 0 时该座位带 wire（能收到 tenpai / state_sync 这类仅本家的帧）。
  // 其余座位保持 bot + 无 wire，这样对局仍能自动推进。
  // `human_seat` 给出时该座位是**真人**（is_bot=false）：只有真人窗口才真的下发
  // `action_request` 并保留 deadline，测「重连重放」这类帧契约需要它。此时 `Step()` 不能
  // 推进这一步 —— 调用方用 `OnTimeout` 让它超时（默认动作）继续。
  explicit BotTable(uint64_t seed, int wired_seat = -1, int human_seat = -1) {
    GameContext context;
    context.room_id = "r-tempfuriten";
    context.seed = seed;
    context.action_timeout_ms = kActionTimeoutMs;
    context.inter_hand_delay_ms = 0;
    context.ai_delay_ms = 900;
    context.deadlines_supported = true;
    context.options_json = kTonpuOptions;
    for (int seat = 0; seat < kSeatCount; ++seat) {
      const bool wired = seat == wired_seat;
      context.seats.push_back(GameSeat {.seat = seat,
                                        .is_bot = seat != human_seat,
                                        .wired = wired,
                                        .player_id = "bot-" + std::to_string(seat),
                                        .name = "AI-" + std::to_string(seat)});
    }
    Deliver(game_.Setup(context));
  }

  RiichiMahjongGame& game() { return game_; }

  // 该座位收到的帧（与 Driver 同口径，按投递顺序）。bot 座位没有 wire，因此恒为空 ——
  // 这里保留它只为让断言写法统一。
  const std::vector<std::string>& frames_of(int seat) const { return frames_[static_cast<size_t>(seat)]; }

  // 放行一次 AI 决策（或推进一次窗口）。返回 false 表示已经推不动了。
  bool Step() {
    if (game_.game_finished()) {
      return false;
    }
    if (game_.round_finished()) {
      return FireDeadline(&IsRoundAdvanceToken) || FireDeadline(&IsMatchEndToken);
    }
    // 优先放行 AI 决策令牌：有它说明有座位在「延迟思考」。
    if (FireDeadline(&IsAiPacingToken)) {
      return true;
    }
    const uint64_t before = game_.seq();
    Deliver(game_.OnTimeout(before));
    return game_.seq() != before || game_.round_finished() || game_.game_finished();
  }

 private:
  void Deliver(const GameStep& step) {
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
  }

  bool FireDeadline(bool (*match)(uint64_t)) {
    for (size_t index = scheduled_.size(); index > 0; --index) {
      if (match(scheduled_[index - 1].second)) {
        const uint64_t token = scheduled_[index - 1].second;
        scheduled_.erase(scheduled_.begin() + static_cast<std::ptrdiff_t>(index - 1));
        Deliver(game_.OnTimeout(token));
        return true;
      }
    }
    return false;
  }

  RiichiMahjongGame game_;
  std::array<std::vector<std::string>, kSeatCount> frames_;
  std::vector<std::pair<int64_t, uint64_t>> scheduled_;
};

}  // namespace

// 喰い替え禁止（v2.5）：碰之后不能打回同一张，吃之后不能打モロ + スジ。
// 三条断言分别守「拒绝打」「打完即解除」「整手被禁时放开（不许卡死局面）」。
TEST(RiichiMahjongGameTest, DiscardRightAfterAClaimIsRefused) {
  BotTable table(7, /*wired_seat=*/kSeat0);
  RiichiMahjongGame& game = table.game();
  // 「刚碰完 5p」的局面：禁区 = {5p}，手里确实还有一张 5p（碰后剩下的那一张）。
  std::vector<Tile> hand;
  for (const char* text : {"2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "4p", "7s", "8s", "9p", "9p", "5p"}) {
    hand.push_back(*ParseTile(text));
  }
  game.SetHandForTest(kSeat0, hand);
  game.SetKuikaeForTest(kSeat0, {FaceOf(*ParseTile("5p"))});
  ASSERT_EQ(game.forbidden_discards_of(kSeat0).size(), size_t {1});

  json::Value discard = json::Value::Obj();
  discard.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kDiscard)));
  discard.Set("tile", json::Value::Str("5p"));
  discard.Set("seq", json::Value::Int(static_cast<int64_t>(game.seq())));
  const GameVerdict verdict = game.Validate(kSeat0, discard);
  EXPECT_EQ(verdict.kind, GameVerdict::Kind::kRefuse) << "碰完当巡打回同一张必须被拒";
  EXPECT_EQ(verdict.code, protocol::ErrorCode::kIllegalAction);

  // 打别的牌照常通过，且打完之后禁区解除。
  discard.Set("tile", json::Value::Str("9p"));
  ASSERT_EQ(game.Validate(kSeat0, discard).kind, GameVerdict::Kind::kAccept);
  game.Apply(kSeat0, discard);
  EXPECT_TRUE(game.forbidden_discards_of(kSeat0).empty()) << "打出一张之后，喰い替え限制就用完了";
}

TEST(RiichiMahjongGameTest, KuikaeIsReleasedWhenEveryTileIsForbidden) {
  // 手里每一张都被禁（极端但可达：三副露 + 手牌全落在禁区）⇒ 必须放开，否则窗口永远
  // 等不到合法打牌，只能靠超时打出一张必被拒的牌。
  BotTable table(7);
  RiichiMahjongGame& game = table.game();
  std::vector<Face> banned;
  for (const char* text : {"2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "4p", "7s", "8s", "1s", "9p", "9p"}) {
    banned.push_back(FaceOf(*ParseTile(text)));
  }
  game.SetHandForTest(kSeat0, TemporaryFuritenHand());
  game.SetKuikaeForTest(kSeat0, banned);
  EXPECT_TRUE(game.forbidden_discards_of(kSeat0).empty())
      << "整手都被禁时必须放开（规则不能把局面卡死）";
}

TEST(RiichiMahjongGameTest, KuikaeOnlyForbidsFacesActuallyInHand) {
  // 禁的是「牌面」：手上没有那一面，就算被禁也不该出现在下发的表里。
  BotTable table(7);
  RiichiMahjongGame& game = table.game();
  game.SetHandForTest(kSeat0, TemporaryFuritenHand());
  game.SetKuikaeForTest(kSeat0, {FaceOf(*ParseTile("5p"))});
  EXPECT_TRUE(game.forbidden_discards_of(kSeat0).empty()) << "手里没有 5p，禁区应当是空的";
}

TEST(RiichiMahjongGameTest, ActionRequestCarriesForbiddenDiscards) {
  // 帧契约：只在**有禁区**时多带 `forbidden_discards`；其余键一律不变。
  // 用「seat 0 是真人」的桌面：只有真人窗口才真的下发 action_request 并保留 deadline，
  // 重连重放（`Reconnect`）才有东西可重放。
  BotTable table(7, /*wired_seat=*/kSeat0, /*human_seat=*/kSeat0);
  RiichiMahjongGame& game = table.game();

  // ① 开局那个窗口没有任何禁区 ⇒ 帧里不写这个键（老客户端逐字节一致）。
  bool saw_plain = false;
  for (const std::string& text : table.frames_of(kSeat0)) {
    const std::optional<json::Value> frame = json::Value::Parse(text, nullptr);
    if (frame.has_value() && TypeOf(*frame) == protocol::FrameType::kActionRequest) {
      saw_plain = true;
      EXPECT_EQ(frame->Find("forbidden_discards"), nullptr) << "没有禁区时不该多写这个键";
    }
  }
  EXPECT_TRUE(saw_plain) << "开局必须给真人下发 action_request";

  // ② 加上禁区后重放同一窗口 ⇒ 必须带该键，且是牌面码数组。
  ASSERT_FALSE(game.hand_of(kSeat0).empty());
  const Tile banned = SortedTiles(game.hand_of(kSeat0)).front();
  game.SetKuikaeForTest(kSeat0, {FaceOf(banned)});
  const GameStep step = game.Reconnect(kSeat0);
  bool saw_forbidden_request = false;
  for (const Outbound& outbound : step.outbounds) {
    const std::optional<json::Value> frame = json::Value::Parse(outbound.frame, nullptr);
    if (!frame.has_value() || TypeOf(*frame) != protocol::FrameType::kActionRequest) {
      continue;
    }
    saw_forbidden_request = true;
    const json::Value* forbidden = frame->Find("forbidden_discards");
    ASSERT_NE(forbidden, nullptr) << "禁区非空时必须带 forbidden_discards: " << outbound.frame;
    ASSERT_TRUE(forbidden->IsArray());
    ASSERT_EQ(forbidden->AsArray().size(), size_t {1});
    EXPECT_EQ(forbidden->AsArray().front().AsString(), FormatTile(banned, /*red=*/false));
  }
  EXPECT_TRUE(saw_forbidden_request) << "Reconnect 必须重放当前窗口的 action_request";
}

TEST(RiichiMahjongGameTest, RealClaimSetsTheKuikaeRestriction) {
  // 接线检查：真实吃/碰之后，`ApplyMeld` 必须把「由这组牌算出的禁区」落到该座位上
  // （纯函数本身由 riichi_rules_test 的 RiichiKuikae.* 覆盖，这里只验它确实被调用了）。
  // 全 bot 桌面 + ai_delay_ms > 0 才能观察到「刚鸣牌、还没打牌」这个中间态。
  int checked = 0;
  for (uint64_t seed = 1; seed <= 8 && checked == 0; ++seed) {
    BotTable table(seed);
    RiichiMahjongGame& game = table.game();
    std::array<size_t, kSeatCount> melds_seen {};
    for (int seat = 0; seat < kSeatCount; ++seat) {
      melds_seen[static_cast<size_t>(seat)] = game.melds_of(seat).size();
    }
    int guard = 0;
    while (guard++ < 600 && !game.round_finished() && !game.game_finished()) {
      // 鸣牌窗口把被鸣的那张从牌河里 pop 掉了，所以要在推进之前记下每家的河末张。
      std::array<std::optional<Tile>, kSeatCount> last_discard {};
      for (int seat = 0; seat < kSeatCount; ++seat) {
        const std::vector<Tile>& river = game.discards_of(seat);
        if (!river.empty()) {
          last_discard[static_cast<size_t>(seat)] = river.back();
        }
      }
      if (!table.Step()) {
        break;
      }
      bool saw_meld = false;
      int melder = -1;
      for (int seat = 0; seat < kSeatCount; ++seat) {
        const size_t now = game.melds_of(seat).size();
        if (now == melds_seen[static_cast<size_t>(seat)]) {
          continue;
        }
        melds_seen[static_cast<size_t>(seat)] = now;
        saw_meld = true;
        const Meld& meld = game.melds_of(seat).back();
        if (meld.type != MeldType::kChow && meld.type != MeldType::kPong) {
          continue;  // 杠不产生打牌前的限制（先补摸）
        }
        melder = seat;
      }
      if (melder < 0) {
        continue;
      }
      // 被鸣的那张 = 牌河缩短了的那家的河末张（上一轮记下的）。
      std::optional<Tile> claimed;
      for (int seat = 0; seat < kSeatCount; ++seat) {
        if (seat == melder || !last_discard[static_cast<size_t>(seat)].has_value()) {
          continue;
        }
        claimed = last_discard[static_cast<size_t>(seat)];
      }
      if (!claimed.has_value()) {
        continue;  // 该轮的被鸣牌不是本步打出的（理论上不会发生），跳过
      }
      // 从副露组里还原「手牌出的那几张」：去掉一张与被鸣牌同面的。
      std::vector<Tile> from_hand = game.melds_of(melder).back().tiles;
      const auto target = std::find_if(from_hand.begin(), from_hand.end(),
                                       [&claimed](Tile tile) { return FaceOf(tile) == FaceOf(*claimed); });
      ASSERT_NE(target, from_hand.end());
      from_hand.erase(target);

      // 生效口径：与手牌取交集；若整手都被禁则放开（与 KuikaeFacesFor 同口径）。
      const std::vector<Face> raw = riichi::KuikaeForbiddenFaces(from_hand, *claimed);
      const std::vector<Tile>& hand = game.hand_of(melder);
      std::vector<Face> expected;
      bool any_allowed = std::any_of(hand.begin(), hand.end(), [&raw](Tile tile) {
        return std::find(raw.begin(), raw.end(), FaceOf(tile)) == raw.end();
      });
      if (any_allowed) {
        for (Face face : raw) {
          if (std::any_of(hand.begin(), hand.end(), [face](Tile tile) { return FaceOf(tile) == face; })) {
            expected.push_back(face);
          }
        }
      }
      EXPECT_EQ(game.forbidden_discards_of(melder), expected)
          << "seed " << seed << " seat " << melder << "：鸣牌后禁区应由这组牌算出";
      ++checked;
      break;
    }
  }
  EXPECT_GT(checked, 0) << "8 个种子里一次吃/碰都没出现，这条用例失去意义";
}

TEST(RiichiMahjongGameTest, TemporaryFuritenBlocksOnlyThePassedFace) {
  BotTable table(7);
  RiichiMahjongGame& game = table.game();

  // 摆成「打 1s 之后听 6s/9s」的行动态，然后打 1s 进入等待态。
  // 这里借 kSeat0 自己的打牌窗口：全 bot 桌面上第一个窗口属于庄家，直接替它打。
  game.SetHandForTest(kSeat0, TemporaryFuritenHand());
  ASSERT_EQ(game.hand_of(kSeat0).size() + 3 * game.melds_of(kSeat0).size(), size_t {14});
  json::Value discard = json::Value::Obj();
  discard.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kDiscard)));
  discard.Set("tile", json::Value::Str("1s"));
  discard.Set("seq", json::Value::Int(static_cast<int64_t>(game.seq())));
  ASSERT_EQ(game.Validate(kSeat0, discard).kind, GameVerdict::Kind::kAccept);
  game.Apply(kSeat0, discard);
  ASSERT_EQ(game.hand_of(kSeat0).size() + 3 * game.melds_of(kSeat0).size(), size_t {13})
      << "打 1s 之后应当回到等待态";

  ASSERT_FALSE(game.HasTemporaryFuriten(kSeat0)) << "还没放过任何和牌张，不该有同巡振听";

  // 放过 6s ⇒ 只挡 6s；9s 不受影响。
  game.SetTemporaryFuritenForTest(kSeat0, *ParseTile("6s"), true);
  EXPECT_TRUE(game.TemporaryFuritenOf(kSeat0, *ParseTile("6s")));
  EXPECT_FALSE(game.TemporaryFuritenOf(kSeat0, *ParseTile("9s")))
      << "同巡振听按牌面记：放过 6s 不该连带挡住 9s";
  EXPECT_TRUE(game.HasTemporaryFuriten(kSeat0));

  // 关键断言：这一手形式听牌（流局照吃罚符），但 6s 已经和不了。
  // `IsFuritenFor` 是荣和过滤用的细口径，必须只对放过的牌面为真。
  EXPECT_TRUE(game.IsFuriten(kSeat0)) << "整手处于同巡振听";
  EXPECT_TRUE(game.IsFuritenFor(kSeat0, *ParseTile("6s"))) << "放过的牌面必须挡住荣和";
  EXPECT_FALSE(game.IsFuritenFor(kSeat0, *ParseTile("9s"))) << "没放过的牌面必须仍可荣和";
}

TEST(RiichiMahjongGameTest, TemporaryFuritenClearsOnOwnDraw) {
  BotTable table(7);
  table.game().SetTemporaryFuritenForTest(kSeat0, *ParseTile("6s"), true);
  ASSERT_TRUE(table.game().HasTemporaryFuriten(kSeat0));

  // 推到 kSeat0 的下一次摸牌（或鸣牌）—— 两者都是「这一巡结束」。
  int guard = 0;
  while (guard++ < 400 && table.game().HasTemporaryFuriten(kSeat0) && !table.game().round_finished() &&
         !table.game().game_finished()) {
    if (!table.Step()) {
      break;
    }
  }
  EXPECT_FALSE(table.game().HasTemporaryFuriten(kSeat0)) << "自己摸牌之后同巡振听必须解除";
}

TEST(RiichiMahjongGameTest, TemporaryFuritenSurvivesOtherSeatsTurns) {
  BotTable table(7);
  table.game().SetTemporaryFuritenForTest(kSeat0, *ParseTile("6s"), true);
  ASSERT_TRUE(table.game().HasTemporaryFuriten(kSeat0));
  // 只要「kSeat0 自己还没摸牌/鸣牌」，别人的回合就不该解掉它。
  // 逐个推进，一旦 kSeat0 的手牌张数或副露数变化就说明轮到它了，在此之前都必须保持振听。
  const size_t hand_before = table.game().hand_of(kSeat0).size();
  const size_t melds_before = table.game().melds_of(kSeat0).size();
  int guard = 0;
  while (guard++ < 400) {
    const bool still_untouched = table.game().hand_of(kSeat0).size() == hand_before &&
                                 table.game().melds_of(kSeat0).size() == melds_before;
    if (!still_untouched || table.game().round_finished() || table.game().game_finished()) {
      break;
    }
    if (!table.Step()) {
      break;
    }
    if (!still_untouched) {
      break;
    }
    // 还没轮到 kSeat0 ⇒ 同巡振听必须还在。
    if (table.game().hand_of(kSeat0).size() == hand_before &&
        table.game().melds_of(kSeat0).size() == melds_before) {
      EXPECT_TRUE(table.game().HasTemporaryFuriten(kSeat0))
          << "别人的回合不该解除同巡振听";
    }
  }
}

TEST(RiichiMahjongGameTest, TemporaryFuritenSurvivesOtherSeatMeld) {
  // 关键场景：A 放过一张和牌张 ⇒ A 进入同巡振听。随后**别人**（不是 A）吃/碰/杠，
  // A 的振听必须保持 —— 鸣牌的「翻页」只对该鸣牌者自己的那一巡生效。
  // 这条守住的是 `ApplyMeld` 里只能清 `seat` 自己、绝不能清全场。
  int checked_rounds = 0;
  for (uint64_t seed = 1; seed <= 8 && checked_rounds == 0; ++seed) {
    BotTable table(seed);
    RiichiMahjongGame& game = table.game();
    // 把同巡振听同时挂在所有座位上：这样无论谁先鸣牌，都能看出「是否误清全场」。
    // kSeat0 之外的三家都会真实行动，只要有一家鸣了牌而别人仍保持振听，断言就成立。
    for (int seat = 0; seat < kSeatCount; ++seat) {
      game.SetTemporaryFuritenForTest(seat, *ParseTile("6s"), true);
    }
    std::array<size_t, kSeatCount> melds_seen {};
    std::array<size_t, kSeatCount> hands_seen {};
    // 累计跟踪：某个座位只要「摸过牌或鸣过牌」，它的振听就已经合法解除，之后不该再要求它保持。
    // 只看「这一步」是否变化是不够的 —— 座位可能在更早的几步里就轮到了。
    std::array<bool, kSeatCount> ever_acted {};
    for (int seat = 0; seat < kSeatCount; ++seat) {
      melds_seen[static_cast<size_t>(seat)] = game.melds_of(seat).size();
      hands_seen[static_cast<size_t>(seat)] = game.hand_of(seat).size();
    }
    int guard = 0;
    while (guard++ < 600 && !game.round_finished() && !game.game_finished()) {
      if (!table.Step()) {
        break;
      }
      int melder = -1;
      for (int seat = 0; seat < kSeatCount; ++seat) {
        const size_t melds_now = game.melds_of(seat).size();
        const size_t hand_now = game.hand_of(seat).size();
        const size_t index = static_cast<size_t>(seat);
        if (melds_now > melds_seen[index]) {
          melder = seat;
        }
        if (melds_now != melds_seen[index] || hand_now != hands_seen[index]) {
          ever_acted[index] = true;
        }
        melds_seen[index] = melds_now;
        hands_seen[index] = hand_now;
      }
      if (melder < 0) {
        continue;
      }
      // 有一个座位鸣了牌：它自己的振听应该被清掉。
      EXPECT_FALSE(game.HasTemporaryFuriten(melder))
          << "鸣牌者自己的同巡振听应当解除（本巡已翻页）";
      // 其余「从设置振听起就一直没动过」的座位，必须仍然带着振听。
      for (int other = 0; other < kSeatCount; ++other) {
        if (other == melder || ever_acted[static_cast<size_t>(other)]) {
          continue;
        }
        EXPECT_TRUE(game.HasTemporaryFuriten(other)) << "别人鸣牌不该解除座位 " << other << " 的同巡振听";
      }
      ++checked_rounds;
      break;  // 一次真实鸣牌足够；再多只是同一条逻辑的重复
    }
  }
  ASSERT_GT(checked_rounds, 0) << "8 个种子里一次鸣牌都没出现，这条用例失去意义";
}

TEST(RiichiMahjongGameTest, TemporaryFuritenClearsOnOwnAction) {
  BotTable table(7);
  table.game().SetTemporaryFuritenForTest(kSeat0, *ParseTile("6s"), true);
  ASSERT_TRUE(table.game().HasTemporaryFuriten(kSeat0));

  // 推到 kSeat0 自己动作为止（摸牌或鸣牌）—— 两种都是「这一巡结束」，同一条断言覆盖。
  int guard = 0;
  while (guard++ < 400 && table.game().HasTemporaryFuriten(kSeat0) && !table.game().round_finished() &&
         !table.game().game_finished()) {
    if (!table.Step()) {
      break;
    }
  }
  EXPECT_FALSE(table.game().HasTemporaryFuriten(kSeat0)) << "kSeat0 摸牌或鸣牌之后同巡振听必须解除";
}

TEST(RiichiMahjongGameTest, TenpaiFrameCarriesFuriten) {
  // 振听是本家私有信息（他家算不出来），所以随**仅本家**的 tenpai 帧下发。
  // 这里验三件事：默认不报、置位后帧里 `furiten` 为 true、逐张 `waits[].furiten` 按牌面区分。
  BotTable table(7, /*wired_seat=*/kSeat0);
  RiichiMahjongGame& game = table.game();

  // 摆成听 6s/9s 的等待态：打掉 1s。
  game.SetHandForTest(kSeat0, TemporaryFuritenHand());
  json::Value discard = json::Value::Obj();
  discard.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kDiscard)));
  discard.Set("tile", json::Value::Str("1s"));
  discard.Set("seq", json::Value::Int(static_cast<int64_t>(game.seq())));
  ASSERT_EQ(game.Validate(kSeat0, discard).kind, GameVerdict::Kind::kAccept);
  game.Apply(kSeat0, discard);

  // 找到 kSeat0 最近一帧 tenpai。全 bot 桌面没有 wire，所以直接构造一次 Sync 走下发路径。
  const auto latest_tenpai = [&]() -> std::optional<json::Value> {
    const GameStep sync = game.Sync(kSeat0);
    for (const Outbound& outbound : sync.outbounds) {
      const std::optional<json::Value> frame = json::Value::Parse(outbound.frame, nullptr);
      if (frame.has_value() && TypeOf(*frame) == protocol::FrameType::kTenpai) {
        return frame;
      }
    }
    return std::nullopt;
  };

  // 1) 未振听：furiten=false，逐张也全 false。
  const std::optional<json::Value> clean = latest_tenpai();
  ASSERT_TRUE(clean.has_value()) << "Sync 必须补发一帧 tenpai（§5.16 T3）";
  ASSERT_TRUE(clean->Find("is_tenpai")->AsBool()) << "打 1s 之后应当形听";
  EXPECT_FALSE(BoolOf(*clean, "furiten")) << "未振听时 furiten 必须为 false";

  // 2) 放过 6s ⇒ 整手 furiten=true，且只有 6s 那一条是 true。
  game.SetTemporaryFuritenForTest(kSeat0, *ParseTile("6s"), true);
  const std::optional<json::Value> dirty = latest_tenpai();
  ASSERT_TRUE(dirty.has_value());
  EXPECT_TRUE(BoolOf(*dirty, "furiten")) << "振听时 furiten 必须为 true";
  const json::Value* waits = dirty->Find("waits");
  ASSERT_NE(waits, nullptr);
  ASSERT_TRUE(waits->IsArray());
  int furiten_waits = 0;
  for (const json::Value& item : waits->AsArray()) {
    const std::string tile = StrOf(item, "tile");
    const bool flagged = BoolOf(item, "furiten");
    if (flagged) {
      ++furiten_waits;
      EXPECT_EQ(tile, "6s") << "只有被放过的那个牌面该标振听，实际是 " << tile;
    }
  }
  EXPECT_EQ(furiten_waits, 1) << "6s/9s 两面听里应当恰好一张被标振听";

  // 3) 形听语义不变：振听不影响 is_tenpai（流局罚符照吃）。
  EXPECT_TRUE(dirty->Find("is_tenpai")->AsBool()) << "振听不得被当成「没听牌」";
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
