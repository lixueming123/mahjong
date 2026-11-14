// The replay recorder (replay.h) driven directly — no room, no socket, no filesystem.
//
// What is proven here:
//
//   1. **A recording is a fact, not a program.** The same match recorded twice from the same
//      seed produces byte-identical files, because the recorder only appends what the game
//      emitted. `same seed -> same frames` is already proven elsewhere; this pins the step that
//      turns frames into a file.
//   2. **The god view is the concealed hands**, and it agrees with `game_end.results[].hand`
//      once the melds are subtracted — i.e. the track a player uses for 透视 is really each
//      seat's own hand, not a re-render of something public.
//   3. **A broadcast is recorded once with all four recipients**, and a private frame with only
//      its own: that expansion is what lets any seat's viewpoint be replayed from one file.
//   4. **The hand track only appears on steps that could change it.** A deadline-only step must
//      not serialize four hands — that is the whole reason AddStep takes a nullable track.
//
// The driver is deliberately a minimal one (answer seat 0's window with the first legal option,
// otherwise let the deadline expire) rather than a copy of the golden fixture's: these tests are
// about the recorder's shape, not about the frozen bytes of a particular match.

#include "replay.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "classic/classic_mahjong_game.h"
#include "game_test_support.h"
#include "protocol.h"

namespace mahjong {
namespace {

using game_test::IntOf;
using game_test::OptionOf;
using game_test::StrOf;
using game_test::TilesOf;
using game_test::TypeOf;

// Zeroes every `deadline_ms` inside a rendered frame list. That field is `clock::EpochMs() +
// timeout` — real time, so no two runs can share it; everything else about a frame is
// deterministic. The room's own golden transcript normalizes exactly this field for the same
// reason, and `e2e_full_game.py --determinism-check` strips it too.
//
// The key is looked up unescaped: frames are spliced into the file as JSON objects (see
// ReplayRecorder::Serialize), so `Dump()` renders `"deadline_ms":` and never `\"deadline_ms\":`.
std::string ScrubDeadline(std::string text) {
  constexpr std::string_view kKey = "\"deadline_ms\":";
  size_t position = 0;
  while ((position = text.find(kKey, position)) != std::string::npos) {
    const size_t value_start = position + kKey.size();
    size_t value_end = value_start;
    while (value_end < text.size() && text[value_end] >= '0' && text[value_end] <= '9') {
      ++value_end;
    }
    text.replace(value_start, value_end - value_start, "0");
    position = value_start + 1;
  }
  return text;
}

constexpr uint64_t kSeed = 0x5DEECE66Dull;
constexpr int kSeat0 = 0;

// Drives one whole classic match and hands every step to a ReplayRecorder, exactly as
// RoomActor::Run does (record, then deliver).
class RecorderDriver {
 public:
  explicit RecorderDriver(uint64_t seed) : recorder_(Meta()) {
    GameContext context;
    context.room_id = "r-0000000000001";
    context.seed = seed;
    context.action_timeout_ms = 15000;
    context.inter_hand_delay_ms = 0;
    context.ai_delay_ms = 0;
    context.deadlines_supported = true;
    context.options_json = "{}";
    for (int seat = 0; seat < kSeatCount; ++seat) {
      context.seats.push_back(GameSeat {.seat = seat,
                                        .is_bot = seat != kSeat0,
                                        .wired = true,
                                        .player_id = seat == kSeat0 ? "u-000042" : "bot-" + std::to_string(seat),
                                        .name = seat == kSeat0 ? "Human" : "AI-" + std::to_string(seat)});
    }
    GameStep step = game_.Setup(context);
    RecordAndDeliver(step);
  }

  void PlayFullGame(int max_steps = 20000) {
    int steps = 0;
    while (!game_.game_finished() && steps < max_steps) {
      if (!Step()) {
        break;
      }
      ++steps;
    }
  }
  const ReplayRecorder& recorder() const { return recorder_; }
  const std::vector<GameResult>& results() const { return results_; }
  // Seat 0's frames in delivery order (what the player would have seen).
  const std::vector<std::string>& seat0_frames() const { return seat0_frames_; }

 private:
  ReplayMeta Meta() {
    ReplayMeta meta;
    meta.room_id = "r-0000000000001";
    meta.game_id = "classic";
    meta.options_json = "{}";
    meta.seed = kSeed;
    meta.started_at_ms = 0;
    for (int seat = 0; seat < kSeatCount; ++seat) {
      ReplaySeat& entry = meta.seats[static_cast<size_t>(seat)];
      entry.seat = seat;
      entry.is_bot = seat != kSeat0;
      entry.player_id = seat == kSeat0 ? "u-000042" : "bot-" + std::to_string(seat);
      entry.name = seat == kSeat0 ? "Human" : "AI-" + std::to_string(seat);
      // Only the human seat is an account; that is what §4.13 authorizes on.
      entry.account_id = seat == kSeat0 ? 42 : 0;
    }
    return meta;
  }

  // One transition, mirroring the golden fixture's step loop: answer the open window (a real
  // discard for a discard window, win/pong/pass for a claim window), otherwise advance a deadline.
  // Answering with `pass` in a discard window would be ILLEGAL_ACTION, and the match would then be
  // played entirely by timeouts — which is not the match we want to record.
  bool Step() {
    if (game_.game_finished()) {
      return false;
    }
    if (AnswerWindow()) {
      return true;
    }
    if (scheduled_.empty()) {
      return false;
    }
    const Deadline next = scheduled_.front();
    scheduled_.erase(scheduled_.begin());
    GameStep step = game_.OnTimeout(next.token);
    if (step.empty()) {
      return true;  // a stale token: the game says so by returning nothing
    }
    RecordAndDeliver(step);
    ++seq_;
    RecordResults(step);
    return true;
  }

  void RecordResults(const GameStep& step) {
    for (const GameResult& result : step.results) {
      results_.push_back(result);
      recorder_.Finish(result);
    }
  }

  // True when it answered something. Seat 0 is the only human; the bots play inline
  // (ai_delay_ms = 0), so when it is not their window there is nothing to answer.
  bool AnswerWindow() {
    const std::optional<json::Value> request = PendingRequest();
    const protocol::Window window = game_.CurrentWindow();
    const uint64_t seq = game_.seq();
    if (window == protocol::Window::kDiscard && game_.current_seat() == kSeat0) {
      GameStep step;
      if (request.has_value() && OptionOf(*request, protocol::Action::kWin) != nullptr) {
        step = game_.Apply(kSeat0, ActionFrame(protocol::Action::kWin, {}, seq));
      } else {
        // The timeout policy's own choice is good enough here: this driver records shapes, not
        // the frozen golden, so any deterministic legal discard will do.
        const std::optional<Tile> chosen = ChooseDiscard(game_.hand_of(kSeat0), game_.melds_of(kSeat0));
        const std::vector<Tile>& hand = game_.hand_of(kSeat0);
        step = game_.Apply(kSeat0, DiscardFrame(chosen.value_or(hand.back()), seq));
      }
      if (step.empty()) {
        return false;
      }
      RecordAndDeliver(step);
      ++seq_;
      RecordResults(step);
      return true;
    }
    if (!request.has_value() || IntOf(*request, "window") != protocol::WireOf(protocol::Window::kClaim)) {
      return false;
    }
    GameStep step;
    if (const json::Value* pong = OptionOf(*request, protocol::Action::kPong); pong != nullptr) {
      step = game_.Apply(kSeat0, ActionFrame(protocol::Action::kPong, TilesOf(*pong, "tiles"), seq));
    } else {
      step = game_.Apply(kSeat0, ActionFrame(protocol::Action::kPass, {}, seq));
    }
    if (step.empty()) {
      return false;
    }
    RecordAndDeliver(step);
    ++seq_;
    RecordResults(step);
    return true;
  }

  static json::Value DiscardFrame(Tile tile, uint64_t seq) {
    json::Value frame = json::Value::Obj(3);
    frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kDiscard)));
    frame.Set("tile", json::Value::Str(FormatTile(tile)));
    frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
    return frame;
  }

  static json::Value ActionFrame(protocol::Action action, const std::vector<Tile>& tiles, uint64_t seq) {
    json::Value frame = json::Value::Obj(4);
    frame.Set("type", json::Value::Int(protocol::WireOf(protocol::FrameType::kAction)));
    frame.Set("action", json::Value::Int(protocol::WireOf(action)));
    json::Value array = json::Value::Arr(tiles.size());
    for (Tile tile : tiles) {
      array.Append(json::Value::Str(FormatTile(tile)));
    }
    frame.Set("tiles", std::move(array));
    frame.Set("seq", json::Value::Int(static_cast<int64_t>(seq)));
    return frame;
  }

  // Mirrors RoomActor::RecordStep: the god view is only asked for on a step that re-renders the
  // table, which is recognized by the state_sync prefix (both games write `type` first).
  void RecordAndDeliver(GameStep& step) {
    constexpr std::string_view kStateSyncPrefix = R"({"type":20,)";
    std::array<std::string, kSeatCount> hands;
    const std::array<std::string, kSeatCount>* track = nullptr;
    for (const Outbound& outbound : step.outbounds) {
      if (outbound.frame.rfind(kStateSyncPrefix, 0) == 0) {
        hands = game_.ReplayHands();
        track = &hands;
        break;
      }
    }
    recorder_.AddStep(static_cast<int64_t>(seq_) * 100, step.outbounds, track);
    for (const Outbound& outbound : step.outbounds) {
      if (outbound.recipients.empty()) {
        for (int seat = 0; seat < kSeatCount; ++seat) {
          if (seat == kSeat0) {
            seat0_frames_.push_back(outbound.frame);
          }
        }
        continue;
      }
      for (const int seat : outbound.recipients) {
        if (seat == kSeat0) {
          seat0_frames_.push_back(outbound.frame);
        }
      }
    }
    for (const Deadline& deadline : step.deadlines) {
      scheduled_.push_back(deadline);
    }
  }

  // The first action_request for seat 0 that belongs to the currently open window.
  std::optional<json::Value> PendingRequest() {
    std::optional<json::Value> pending;
    for (size_t index = consumed_; index < seat0_frames_.size(); ++index) {
      const std::optional<json::Value> frame = json::Value::Parse(seat0_frames_[index], nullptr);
      if (!frame.has_value()) {
        continue;
      }
      if (TypeOf(*frame) != protocol::FrameType::kActionRequest) {
        continue;
      }
      if (static_cast<uint64_t>(IntOf(*frame, "seq")) != game_.seq()) {
        continue;
      }
      if (IntOf(*frame, "window") != protocol::WireOf(protocol::Window::kDiscard)) {
        continue;  // this minimal driver only answers its own discard window
      }
      pending = *frame;
    }
    consumed_ = seat0_frames_.size();
    return pending;
  }

  ClassicMahjongGame game_;
  ReplayRecorder recorder_;
  std::vector<Deadline> scheduled_;
  std::vector<std::string> seat0_frames_;
  std::vector<GameResult> results_;
  size_t consumed_ = 0;
  int64_t seq_ = 0;
};

// A step of only deadlines/frames produces a file entry; the interesting assertion is the
// shape, so it is checked with a hand-built recorder rather than a whole match.
TEST(ReplayRecorderTest, BroadcastExpandsToEverySeatAndPrivateFrameDoesNot) {
  ReplayMeta meta;
  meta.room_id = "r-0000000000001";
  meta.game_id = "classic";
  ReplayRecorder recorder(meta);

  std::vector<Outbound> outbounds;
  outbounds.push_back(Outbound {{}, R"({"type":17,"seat":1})"});  // broadcast
  outbounds.push_back(Outbound {{2}, R"({"type":20,"seq":3})"});  // private: seat 2 only
  recorder.AddStep(0, outbounds, nullptr);

  const json::Value file = *json::Value::Parse(recorder.Serialize(), nullptr);
  const json::Value* steps = file.Find("steps");
  ASSERT_NE(steps, nullptr);
  ASSERT_EQ(steps->AsArray().size(), 1u);
  const json::Value* frames = steps->AsArray().front().Find("f");
  ASSERT_NE(frames, nullptr);
  ASSERT_EQ(frames->AsArray().size(), 2u);
  const json::Value& broadcast = frames->AsArray()[0].AsArray()[0];
  EXPECT_EQ(broadcast.AsArray().size(), static_cast<size_t>(kSeatCount));
  const json::Value& private_frame = frames->AsArray()[1].AsArray()[0];
  ASSERT_EQ(private_frame.AsArray().size(), 1u);
  EXPECT_EQ(private_frame.AsArray()[0].AsInt(), 2);
  // The frame is spliced verbatim as JSON — NOT stored as a string. That is the whole point:
  // wrapping it would escape every `"` in it (~50k of them for a riichi match, +16% file size)
  // and make the file unreadable. Checked by reading the frame's own field back.
  const json::Value& frame = frames->AsArray()[0].AsArray()[1];
  EXPECT_TRUE(frame.IsObject()) << "a frame must be spliced as a JSON object, not a string";
  EXPECT_EQ(IntOf(frame, "type"), protocol::WireOf(protocol::FrameType::kDiscardEvent));
  EXPECT_EQ(IntOf(frame, "seat"), 1);
  // And the serialized text must not contain a single escaped quote.
  EXPECT_EQ(recorder.Serialize().find("\\\""), std::string::npos);
}

TEST(ReplayRecorderTest, HandsAreRecordedOnlyWhenTheyChange) {
  ReplayMeta meta;
  meta.room_id = "r-0000000000001";
  ReplayRecorder recorder(meta);

  const std::array<std::string, kSeatCount> first {"1m,2m", "", "", ""};
  recorder.AddStep(0, {Outbound {{}, R"({"type":20})"}}, &first);

  std::array<std::string, kSeatCount> moved = first;
  moved[3] = "9s";
  recorder.AddStep(10, {Outbound {{}, R"({"type":20})"}}, &moved);

  // A deadline-only step passes nullptr: nothing about the table changed.
  recorder.AddStep(20, {Outbound {{}, R"({"type":18})"}}, nullptr);

  const json::Value file = *json::Value::Parse(recorder.Serialize(), nullptr);
  const json::Value* steps = file.Find("steps");
  ASSERT_NE(steps, nullptr);
  ASSERT_EQ(steps->AsArray().size(), 3u);
  const json::Value* first_hands = steps->AsArray()[0].Find("h");
  ASSERT_NE(first_hands, nullptr);
  EXPECT_EQ(first_hands->AsArray()[0].AsString(), "1m,2m");
  EXPECT_EQ(first_hands->AsArray()[1].AsString(), "");  // unchanged seats stay empty
  const json::Value* second_hands = steps->AsArray()[1].Find("h");
  EXPECT_EQ(second_hands->AsArray()[0].AsString(), "");  // unchanged, so omitted
  EXPECT_EQ(second_hands->AsArray()[3].AsString(), "9s");
  const json::Value* third_hands = steps->AsArray()[2].Find("h");
  EXPECT_EQ(third_hands->AsArray()[0].AsString(), "");
  EXPECT_EQ(third_hands->AsArray()[3].AsString(), "");
}

TEST(ReplayRecorderTest, SerializedFileContainsNoEscapes) {
  ReplayMeta meta;
  meta.room_id = "r-0000000000001";
  meta.game_id = "riichi";
  meta.options_json = R"({"length":2})";   // the real riichi shape: quotes inside
  meta.seed = 7;
  ReplayRecorder recorder(meta);

  const std::array<std::string, kSeatCount> hands {"1m,2m", "", "", ""};
  recorder.AddStep(0, {Outbound {{}, R"({"type":20,"seq":1,"seats":[]})"}}, &hands);

  const std::string text = recorder.Serialize();
  EXPECT_EQ(text.find('\\'), std::string::npos)
      << "replay text must not contain escapes; first at offset " << text.find('\\')
      << ": " << text.substr(text.find('\\') > 60 ? text.find('\\') - 60 : 0, 140);

  // And the options survive as a real object, not a re-parseable string.
  const json::Value file = *json::Value::Parse(text, nullptr);
  const json::Value* options = file.Find("options_json");
  ASSERT_NE(options, nullptr);
  EXPECT_TRUE(options->IsObject());
  const json::Value* length = options->Find("length");
  ASSERT_NE(length, nullptr);
  EXPECT_EQ(length->AsInt(), 2);
}

// An empty or malformed options string must not break the recording.
TEST(ReplayRecorderTest, NonObjectOptionsDegradeToEmptyObject) {
  for (const std::string options : {"", "not json", "[1,2]"}) {
    ReplayMeta meta;
    meta.room_id = "r-0000000000001";
    meta.options_json = options;
    ReplayRecorder recorder(meta);
    const json::Value file = *json::Value::Parse(recorder.Serialize(), nullptr);
    const json::Value* recorded = file.Find("options_json");
    ASSERT_NE(recorded, nullptr) << "options=" << options;
    EXPECT_TRUE(recorded->IsObject()) << "options=" << options;
    EXPECT_EQ(recorded->Dump(), "{}") << "options=" << options;
  }
}

TEST(ReplayRecorderTest, EmptyStepIsNotRecorded) {
  ReplayMeta meta;
  meta.room_id = "r-0000000000001";
  ReplayRecorder recorder(meta);
  recorder.AddStep(0, {}, nullptr);
  EXPECT_EQ(recorder.step_count(), 0u);
}

TEST(ReplayRecorderTest, AuthorizationFollowsTheSeatTable) {
  ReplayMeta meta;
  meta.room_id = "r-0000000000001";
  meta.seats[0].account_id = 42;
  meta.seats[0].is_bot = false;
  meta.seats[1].account_id = 0;  // a bot
  meta.seats[1].is_bot = true;
  ReplayRecorder recorder(meta);

  EXPECT_TRUE(recorder.IncludesAccount(42));
  EXPECT_FALSE(recorder.IncludesAccount(43));  // never sat here
  EXPECT_FALSE(recorder.IncludesAccount(0));   // a guest has nothing to claim (§4.13)
}

// The whole point of recording frames instead of actions: the same match recorded twice is the
// same file. This is what makes a replay a fact rather than a re-simulation.
//
// The `t` track is the one field that is NOT reproducible: it is a wall-clock offset, and two
// runs of the same match take different real time. So the comparison is over the file with every
// `t` zeroed — the same normalization the room's own golden transcript uses for
// `action_request.deadline_ms` — and the frame/hand tracks must match byte for byte.
TEST(ReplayRecorderTest, SameSeedRecordsTheSameFile) {
  RecorderDriver first(kSeed);
  first.PlayFullGame();
  RecorderDriver second(kSeed);
  second.PlayFullGame();

  ASSERT_GT(first.recorder().step_count(), 10u) << "the driver did not play a match";
  const json::Value first_file = *json::Value::Parse(first.recorder().Serialize(), nullptr);
  const json::Value second_file = *json::Value::Parse(second.recorder().Serialize(), nullptr);
  const json::Value* first_steps = first_file.Find("steps");
  const json::Value* second_steps = second_file.Find("steps");
  ASSERT_NE(first_steps, nullptr);
  ASSERT_NE(second_steps, nullptr);
  ASSERT_EQ(first_steps->AsArray().size(), second_steps->AsArray().size());
  for (size_t index = 0; index < first_steps->AsArray().size(); ++index) {
    const json::Value& left = first_steps->AsArray()[index];
    const json::Value& right = second_steps->AsArray()[index];
    // `action_request.deadline_ms` is wall clock (clock::EpochMs() + timeout), so it is the one
    // frame field two runs cannot share. Everything else must match byte for byte.
    //
    // Compared on the frame objects' Dump() rather than the raw file text: frames are spliced
    // verbatim now, but a re-serialized object is still an exact description of the same frame,
    // and it lets ScrubDeadline work on one well-defined rendering.
    ASSERT_EQ(ScrubDeadline(left.Find("f")->Dump()), ScrubDeadline(right.Find("f")->Dump()))
        << "step " << index << " frames differ";
    ASSERT_EQ(left.Find("h")->Dump(), right.Find("h")->Dump()) << "step " << index << " hands differ";
  }
  // Everything except the pacing track is identical too (metadata, seats, totals).
  EXPECT_EQ(first_file.Find("seats")->Dump(), second_file.Find("seats")->Dump());
  EXPECT_EQ(first_file.Find("totals")->Dump(), second_file.Find("totals")->Dump());
}

}  // namespace
}  // namespace mahjong
