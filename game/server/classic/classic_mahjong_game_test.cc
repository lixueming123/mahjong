// ClassicMahjongGame driven directly — no room, no actor, no socket.
//
// Two things are proven here and nowhere else:
//
//   1. **The game is actor-free.** This file never constructs a RoomActor, an ActorRef or a
//      mailbox: it calls Setup/Apply/OnTimeout and reads the frames out of the returned
//      GameStep. If a rule ever needed the room to work, this test could not exist.
//   2. **The port is byte-exact.** The three hashes below are the classic half of
//      mahjong_room_test's GoldenTranscriptIsFrozen, frozen *before* room.cc was split. They
//      were produced by the old monolithic room; reproducing them here means the split changed
//      no byte of any frame for any seat across a whole 4-round match.
//
// The driver mirrors the old fixture's step loop exactly (answer the human's window, otherwise
// let it expire; round advance and match end are fired from the recorded deadlines) because the
// transcript's cross-seat order is a property of that loop, not of the frames.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "classic/classic_mahjong_game.h"
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

  ClassicMahjongGame& game() { return game_; }

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
  // window take a win, else a pong, else pass.
  bool AnswerWindow() {
    const std::optional<json::Value> request = PendingRequest();
    const protocol::Window window = game_.CurrentWindow();
    const uint64_t seq = game_.seq();
    if (window == protocol::Window::kDiscard && game_.current_seat() == kSeat0) {
      if (request.has_value() && OptionOf(*request, protocol::Action::kWin) != nullptr) {
        SendAction(protocol::Action::kWin, {}, seq);
      } else {
        std::optional<Tile> chosen =
            ChooseDiscard(game_.hand_of(kSeat0), game_.melds_of(kSeat0), drawn_);
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

  ClassicMahjongGame game_;
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

TEST(ClassicMahjongGameTest, GoldenTranscriptIsFrozen) {
  struct Golden {
    uint64_t seed;
    const char* name;
    uint64_t expected;
  };
  // Frozen by room.cc's GoldenTranscriptIsFrozen before the split (commit 00ab595). Same seed,
  // same policy, same four seats — a different number here means the port changed the wire.
  const Golden cases[] = {
      {20251005, "classic/seed-20251005", 0x006adf6ea1e90542ull},
      {7, "classic/seed-7", 0xd1920094dbc4dbd0ull},
      {424242, "classic/seed-424242", 0xdc74452076937cbcull},
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
// match is over — that flag is what the room's reclaim hangs off (§1.4.5).
TEST(ClassicMahjongGameTest, OneResultPerRoundAndOnlyTheLastEndsTheMatch) {
  Driver driver(20251005);
  driver.PlayFullGame();
  ASSERT_TRUE(driver.game().game_finished());
  const std::vector<GameResult>& results = driver.results();
  // 4 rounds plus the renchan this seed produces; the exact count is a property of the deal.
  EXPECT_EQ(static_cast<int>(results.size()), driver.game().hands_played());
  ASSERT_FALSE(results.empty());
  EXPECT_TRUE(results.back().match_over);
  for (size_t index = 0; index + 1 < results.size(); ++index) {
    EXPECT_FALSE(results[index].match_over) << "第 " << index << " 局不该结束整场";
  }
}

// The wall never leaves the server: no frame may carry the seed or the undrawn tiles.
TEST(ClassicMahjongGameTest, NoFrameCarriesTheWallOrTheSeed) {
  Driver driver(20251005);
  driver.PlayFullGame();
  for (int seat = 0; seat < kSeatCount; ++seat) {
    for (const std::string& frame : driver.frames_of(seat)) {
      EXPECT_EQ(frame.find("\"wall\""), std::string::npos) << frame;
      EXPECT_EQ(frame.find("\"seed\""), std::string::npos) << frame;
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
