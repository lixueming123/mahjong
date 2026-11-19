// The replay recorder (replay.h) driven directly — no room, no socket, no filesystem.
//
// What is proven here:
//
//   1. **A recording is a fact, not a program.** The same match recorded twice from the same
//      seed produces byte-identical files, because the recorder only appends what the game
//      emitted. `same seed -> same frames` is already proven elsewhere; this pins the step that
//      turns frames into a file.
//   2. **The view track is each seat's own private view**, not a re-render of something public:
//      replaying from a seat with no endpoint is only possible because the game says what that
//      seat was looking at.
//   3. **A broadcast is recorded once with all four recipients**, and a private frame with only
//      its own: that expansion is what lets any seat's viewpoint be replayed from one file.
//   4. **The view track only appears on steps that could change it.** A deadline-only step must
//      not serialize four views — that is the whole reason AddStep takes a nullable track.
//   5. **The view text is opaque to the recorder and the shell.** It is compared, stored and
//      spliced verbatim; a game that calls its per-seat state something else needs no change here.
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

  // Mirrors RoomActor::RecordStep: the seat views are only asked for on a step that re-renders the
  // table, which is recognized by the state_sync prefix (both games write `type` first).
  void RecordAndDeliver(GameStep& step) {
    constexpr std::string_view kStateSyncPrefix = R"({"type":20,)";
    SeatViews views;
    const SeatViews* track = nullptr;
    for (const Outbound& outbound : step.outbounds) {
      if (outbound.frame.rfind(kStateSyncPrefix, 0) == 0) {
        views = game_.ReplaySeatViews();
        track = &views;
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

TEST(ReplayRecorderTest, ViewsAreRecordedOnlyWhenTheyChange) {
  ReplayMeta meta;
  meta.room_id = "r-0000000000001";
  ReplayRecorder recorder(meta);

  const SeatViews first {R"({"hand":["1m","2m"],"melds":[]})", "", "", ""};
  recorder.AddStep(0, {Outbound {{}, R"({"type":20})"}}, &first);

  SeatViews moved = first;
  moved[3] = R"({"hand":["9s"],"melds":[]})";
  recorder.AddStep(10, {Outbound {{}, R"({"type":20})"}}, &moved);

  // A deadline-only step passes nullptr: nothing about the table changed.
  recorder.AddStep(20, {Outbound {{}, R"({"type":18})"}}, nullptr);

  const json::Value file = *json::Value::Parse(recorder.Serialize(), nullptr);
  const json::Value* steps = file.Find("steps");
  ASSERT_NE(steps, nullptr);
  ASSERT_EQ(steps->AsArray().size(), 3u);
  // A view is spliced as an object, never as a string: it is rendered JSON already, and quoting it
  // would escape every inner quote.
  const json::Value* first_views = steps->AsArray()[0].Find("v");
  ASSERT_NE(first_views, nullptr);
  ASSERT_EQ(first_views->AsArray().size(), 4u);
  EXPECT_TRUE(first_views->AsArray()[0].IsObject());
  EXPECT_EQ(first_views->AsArray()[0].Find("hand")->AsArray().size(), 2u);
  EXPECT_EQ(first_views->AsArray()[1].AsString(), "");  // unchanged seats stay empty
  const json::Value* second_views = steps->AsArray()[1].Find("v");
  EXPECT_EQ(second_views->AsArray()[0].AsString(), "");  // unchanged, so omitted
  EXPECT_EQ(second_views->AsArray()[3].Find("hand")->AsArray()[0].AsString(), "9s");
  const json::Value* third_views = steps->AsArray()[2].Find("v");
  EXPECT_EQ(third_views, nullptr) << "no view moved, so the whole field is omitted";
}

TEST(ReplayRecorderTest, SerializedFileContainsNoEscapes) {
  ReplayMeta meta;
  meta.room_id = "r-0000000000001";
  meta.game_id = "riichi";
  meta.options_json = R"({"length":2})";   // the real riichi shape: quotes inside
  meta.seed = 7;
  ReplayRecorder recorder(meta);

  const SeatViews views {R"({"hand":["1m","2m"],"melds":[]})", "", "", ""};
  recorder.AddStep(0, {Outbound {{}, R"({"type":20,"seq":1,"seats":[]})"}}, &views);

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

// A view that carries melds must be spliced, not escaped, and it must keep the shape the player's
// reducer expects. Melds live in `state_sync.seats[].melds`, a per-seat private frame that is never
// built for a seat without an endpoint, so a replay from that seat's viewpoint used to show an
// empty table (rivers and hand right, melds missing) — the view track is what fixes that.
//
// A concealed kan keeps `tiles: []` (§8.1): the track is read from any of the four viewpoints, so
// an entry may not carry what a seat is never told.
TEST(ReplayRecorderTest, ViewCarriesMeldsAndIsSplicedNotEscaped) {
  ReplayMeta meta;
  meta.room_id = "r-0000000000001";
  ReplayRecorder recorder(meta);

  const SeatViews views {"",
                         "",
                         R"({"hand":["1m"],"melds":[{"type":2,"tiles":["6m","7m","8m"]},{"type":4,"tiles":[]}]})",
                         ""};
  recorder.AddStep(0, {Outbound {{}, R"({"type":20})"}}, &views);

  const std::string text = recorder.Serialize();
  EXPECT_EQ(text.find("\\\""), std::string::npos) << "a view must be spliced, not escaped";

  const json::Value file = *json::Value::Parse(text, nullptr);
  const json::Value* steps = file.Find("steps");
  ASSERT_NE(steps, nullptr);
  ASSERT_EQ(steps->AsArray().size(), 1u);
  const json::Value* first = steps->AsArray()[0].Find("v");
  ASSERT_NE(first, nullptr);
  ASSERT_EQ(first->AsArray().size(), 4u);
  EXPECT_EQ(first->AsArray()[0].AsString(), "") << "a seat with no view stays empty";
  ASSERT_EQ(first->AsArray()[2].Find("melds")->AsArray().size(), 2u);
  EXPECT_EQ(first->AsArray()[2].Find("melds")->AsArray()[1].Find("tiles")->AsArray().size(), 0u)
      << "a concealed kan must carry no tiles";
}

TEST(ReplayRecorderTest, EmptyStepIsNotRecorded) {
  ReplayMeta meta;
  meta.room_id = "r-0000000000001";
  ReplayRecorder recorder(meta);
  recorder.AddStep(0, {}, nullptr);
  EXPECT_EQ(recorder.step_count(), 0u);
}

// §4.13 authorization used to read the seat table back out of this file. It cannot: the store
// writes gzip now, and the answer lives in `matches` (db.h ReplayAccess), keyed by account id so
// that it is ordered behind the match record it depends on. The seat table is still recorded —
// the player needs the names to render the table — so that is what this checks, together with the
// file names the read path resolves.
TEST(ReplayRecorderTest, SeatTableIsRecordedAndFilesAreNamedByRoomId) {
  ReplayMeta meta;
  meta.room_id = "r-2o5endvwb8zcx";
  meta.seats[0].name = "小明";
  meta.seats[0].account_id = 42;
  meta.seats[1].is_bot = true;
  ReplayRecorder recorder(meta);

  const std::optional<json::Value> parsed = json::Value::Parse(recorder.Serialize(), nullptr);
  ASSERT_TRUE(parsed.has_value());
  const json::Value* seats = parsed->Find("seats");
  ASSERT_NE(seats, nullptr);
  ASSERT_TRUE(seats->IsArray());
  ASSERT_EQ(seats->AsArray().size(), 4u);
  EXPECT_EQ(seats->AsArray()[0].Find("name")->AsString(), "小明");
  EXPECT_EQ(seats->AsArray()[0].Find("account_id")->AsInt(), 42);
  EXPECT_TRUE(seats->AsArray()[1].Find("is_bot")->AsBool());

  // v2.6: the file is a gzip stream, so the name says so. The `.json` spelling is only ever read
  // (a recording written by the previous build), never written.
  EXPECT_EQ(ReplayRecorder::FileNameFor("r-2o5endvwb8zcx"), "r-2o5endvwb8zcx.json.gz");
  EXPECT_EQ(ReplayRecorder::LegacyFileNameFor("r-2o5endvwb8zcx"), "r-2o5endvwb8zcx.json");
}

// A match is written one hand at a time (room_actor.cc WriteReplay), so the names have to sort in
// play order and stay inside the directory: the room id is shape-checked before it becomes a path
// (replay_store.cc IsRoomIdShape) and the index is always digits.
TEST(ReplayRecorderTest, HandPartsAreNamedInPlayOrder) {
  EXPECT_EQ(ReplayRecorder::PartFileNameFor("r-2o5endvwb8zcx", 0), "r-2o5endvwb8zcx.h00.json.gz");
  EXPECT_EQ(ReplayRecorder::PartFileNameFor("r-2o5endvwb8zcx", 7), "r-2o5endvwb8zcx.h07.json.gz");
  EXPECT_EQ(ReplayRecorder::PartFileNameFor("r-2o5endvwb8zcx", 12), "r-2o5endvwb8zcx.h12.json.gz");
  // Lexicographic order == play order for the 4..8 hands a match has (PROTOCOL §5.15).
  EXPECT_LT(ReplayRecorder::PartFileNameFor("r-2o5endvwb8zcx", 2),
            ReplayRecorder::PartFileNameFor("r-2o5endvwb8zcx", 10));
}

// The merge is what keeps the client unchanged: whatever the room wrote it in parts, the answer to
// a §4.13 request is one document with one timeline, byte-for-byte the shape a whole-match file
// used to have.
//
// The interesting assertions are the two that a per-hand recorder makes non-obvious: the tail
// fields (`finished`/`match_over`/`totals`) come from the LAST part, because only it knows the
// match ended; and the steps are concatenated in part order with nothing lost in the middle.
TEST(ReplayPartsTest, MergeProducesTheWholeMatchShape) {
  ReplayMeta meta;
  meta.room_id = "r-2o5endvwb8zcx";
  meta.game_id = "classic";
  meta.options_json = "{}";
  meta.seed = 9;
  meta.started_at_ms = 1000;
  meta.seats[0].name = "小明";

  std::vector<std::string> parts;
  constexpr int kHands = 3;
  for (int hand = 0; hand < kHands; ++hand) {
    ReplayRecorder recorder(meta);
    recorder.AddStep(hand * 1000, {Outbound {{}, R"({"type":17,"seat":1})"}}, nullptr);
    recorder.AddStep(hand * 1000 + 10, {Outbound {{}, R"({"type":20,"seq":1})"}}, nullptr);
    GameResult result;
    result.match_over = hand == kHands - 1;
    result.totals[0] = hand == kHands - 1 ? 12345 : 0;
    result.final_rank[0] = hand == kHands - 1 ? 1 : 0;
    recorder.Finish(result);
    parts.push_back(recorder.Serialize());
  }

  const std::string merged = MergeReplayParts(parts);
  ASSERT_FALSE(merged.empty());
  const json::Value file = *json::Value::Parse(merged, nullptr);
  EXPECT_EQ(file.Find("format")->AsString(), "mahjong-replay");
  EXPECT_EQ(file.Find("version")->AsInt(), ReplayRecorder::kFormatVersion);
  EXPECT_EQ(file.Find("room_id")->AsString(), "r-2o5endvwb8zcx");
  // The head survives once, not once per part.
  EXPECT_EQ(merged.find("\"seats\":"), merged.rfind("\"seats\":"));
  // Every hand's steps are there, in order: two per hand.
  const json::Value* steps = file.Find("steps");
  ASSERT_NE(steps, nullptr);
  ASSERT_EQ(steps->AsArray().size(), static_cast<size_t>(kHands * 2));
  EXPECT_EQ(steps->AsArray()[0].Find("t")->AsInt(), 0);
  EXPECT_EQ(steps->AsArray()[5].Find("t")->AsInt(), 2010);
  // The tail is the last part's: only it saw match_over and the final totals.
  EXPECT_TRUE(file.Find("finished")->AsBool());
  EXPECT_TRUE(file.Find("match_over")->AsBool());
  EXPECT_EQ(file.Find("totals")->AsArray()[0].AsInt(), 12345);
  EXPECT_EQ(file.Find("final_rank")->AsArray()[0].AsInt(), 1);
}

// The strongest statement the merge can make: split any sequence of steps into parts, merge them
// back, and the bytes are the file one recorder would have written for the whole match. If this
// holds, the split is invisible to every reader — the client, the player, the e2e fixture — and
// nothing about the on-disk layout can leak into the wire shape.
//
// Built from synthetic steps rather than a driven match on purpose: the split has to be exercised
// at arbitrary boundaries (including one that cuts a hand in two), which a real match's hand
// boundaries would never test.
TEST(ReplayPartsTest, MergingSplitsEqualsOneWholeFile) {
  ReplayMeta meta;
  meta.room_id = "r-2o5endvwb8zcx";
  meta.game_id = "riichi";
  meta.options_json = R"({"length":2})";
  meta.seed = 42;
  meta.started_at_ms = 5000;
  meta.seats[0].name = "小明";
  meta.seats[2].name = "Bob";

  struct HandStep {
    int64_t t_ms = 0;
    std::string frame;
    bool moves_view = false;
  };
  std::vector<HandStep> script;
  for (int index = 0; index < 9; ++index) {
    script.push_back(HandStep {.t_ms = index * 250,
                               .frame = R"({"type":17,"seat":)" + std::to_string(index % kSeatCount) + "}",
                               .moves_view = index % 3 == 0});
  }
  const std::vector<size_t> cuts = {0, 4, 4, 7};  // an empty part in the middle, on purpose

  auto play = [&meta, &script](size_t begin, size_t end, const GameResult& tail) {
    ReplayRecorder recorder(meta);
    for (size_t index = begin; index < end; ++index) {
      SeatViews views;
      const SeatViews* track = nullptr;
      if (script[index].moves_view) {
        views[1] = R"({"hand":[")" + std::to_string(index) + R"("],"melds":[]})";
        track = &views;
      }
      recorder.AddStep(script[index].t_ms, {Outbound {{}, script[index].frame}}, track);
    }
    recorder.Finish(tail);
    return recorder.Serialize();
  };

  GameResult final_result;
  final_result.match_over = true;
  final_result.totals[0] = -300;
  final_result.final_rank[0] = 4;
  const std::string whole = play(0, script.size(), final_result);

  std::vector<std::string> parts;
  std::vector<int> ends(cuts.begin() + 1, cuts.end());
  ends.push_back(static_cast<int>(script.size()));
  for (size_t index = 0; index < cuts.size(); ++index) {
    // Every part but the last is finished with an unfinished tail: only the last hand knows the
    // match ended, which is exactly the case the merge has to resolve in favour of the last part.
    const GameResult tail = index + 1 == cuts.size() ? final_result : GameResult {};
    parts.push_back(play(cuts[index], static_cast<size_t>(ends[index]), tail));
  }

  const std::string merged = MergeReplayParts(parts);
  ASSERT_FALSE(merged.empty());
  EXPECT_EQ(merged, whole) << "a merged match must be byte-identical to one recorded in one go";
}

// (game.h GameStep::results), in which case the second part carries the tail without a timeline of
// its own. Merging must not drop that tail, nor emit a stray comma.
TEST(ReplayPartsTest, AnEmptyPartStillSuppliesTheTail) {
  ReplayMeta meta;
  meta.room_id = "r-2o5endvwb8zcx";

  ReplayRecorder first(meta);
  first.AddStep(0, {Outbound {{}, R"({"type":17,"seat":0})"}}, nullptr);
  first.Finish(GameResult {});

  ReplayRecorder second(meta);
  second.Finish(GameResult {.match_over = true});

  const std::string merged = MergeReplayParts({first.Serialize(), second.Serialize()});
  const json::Value file = *json::Value::Parse(merged, nullptr);
  ASSERT_NE(file.Find("steps"), nullptr);
  EXPECT_EQ(file.Find("steps")->AsArray().size(), 1u);
  EXPECT_TRUE(file.Find("match_over")->AsBool());
}

// A missing part is answered as "no such recording" rather than half a match: the store logs and
// returns not-found, and it can only do that if the merge says so instead of guessing.
TEST(ReplayPartsTest, AnUnreadablePartIsRefused) {
  ReplayMeta meta;
  meta.room_id = "r-2o5endvwb8zcx";
  ReplayRecorder recorder(meta);
  recorder.AddStep(0, {Outbound {{}, R"({"type":17,"seat":0})"}}, nullptr);

  EXPECT_TRUE(MergeReplayParts({}).empty());
  EXPECT_TRUE(MergeReplayParts({"not json"}).empty());
  EXPECT_TRUE(MergeReplayParts({recorder.Serialize(), ""}).empty());
  // A document without a top-level timeline is not a part either.
  EXPECT_TRUE(MergeReplayParts({R"({"format":"mahjong-replay"})"}).empty());
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
    // A step omits `v` when no view moved, so compare the whole step instead of reaching for the
    // field. `t` is stamped from the driver's own counter (not wall clock), so it matches too.
    ASSERT_EQ(ScrubDeadline(left.Dump()), ScrubDeadline(right.Dump()))
        << "step " << index << " views differ";
  }
  // Everything except the pacing track is identical too (metadata, seats, totals).
  EXPECT_EQ(first_file.Find("seats")->Dump(), second_file.Find("seats")->Dump());
  EXPECT_EQ(first_file.Find("totals")->Dump(), second_file.Find("totals")->Dump());
}

}  // namespace
}  // namespace mahjong
