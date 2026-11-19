// The on-disk layout of a recording: hand parts in, one document out.
//
// Why this is a separate test from replay_test.cc: that one covers the recorder and the merge as
// pure functions, this one covers the filesystem contract around them — which files a room's
// match leaves behind, in what order they are read, and what an upgraded server does with the two
// layouts the previous builds wrote. Every one of those can be broken while every unit still
// passes, and the symptom is a ROOM_NOT_FOUND on a replay somebody just played.
//
// No actor, no database, no socket: LoadReplayFromDisk is the whole read path minus authorization
// (§4.13), and authorization is the part a database test covers.

#include "replay_store.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "base64.h"
#include "gzip.h"
#include "json.h"
#include "replay.h"

namespace mahjong {
namespace {

namespace fs = std::filesystem;

// A throwaway directory per test. The name is derived from the test name so a failure leaves a
// readable path behind instead of a random temp dir.
class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    static int counter = 0;
    path_ = fs::temp_directory_path() /
            ("mahjong-replay-store-" + tag + "-" + std::to_string(++counter) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    fs::create_directories(path_, error);
    EXPECT_FALSE(error) << error.message();
  }
  ~TempDir() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return path_; }
  std::string string() const { return path_.string(); }

  void WriteGzip(const std::string& name, const std::string& text) const {
    const std::string bytes = GzipCompress(text);
    EXPECT_FALSE(bytes.empty());
    std::ofstream out(path_ / name, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  void WriteText(const std::string& name, const std::string& text) const {
    std::ofstream out(path_ / name, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
  }

 private:
  fs::path path_;
};

constexpr char kRoomId[] = "r-2o5endvwb8zcx";

// One hand's part, exactly the shape ReplayRecorder::Serialize produces: the full header, that
// hand's steps, and the tail as of that hand.
std::string MakePart(const std::string& room_id, int hand, bool match_over, int total) {
  ReplayMeta meta;
  meta.room_id = room_id;
  meta.game_id = "classic";
  meta.options_json = "{}";
  meta.seed = 7;
  meta.started_at_ms = 1000;
  meta.seats[0].name = "小明";
  meta.seats[0].player_id = "u-000042";
  meta.seats[0].account_id = 42;
  for (int seat = 1; seat < kSeatCount; ++seat) {
    meta.seats[static_cast<size_t>(seat)].is_bot = true;
  }

  ReplayRecorder recorder(meta);
  const SeatViews views {R"({"hand":["1m","2m"],"melds":[]})", "", "", ""};
  // Two steps per hand: one that re-renders the table and moves a view, one deadline-only.
  recorder.AddStep(hand * 10000, {Outbound {{}, R"({"type":20,"seq":1})"}}, &views);
  recorder.AddStep(hand * 10000 + 100, {Outbound {{}, R"({"type":17,"seat":1})"}}, nullptr);
  GameResult result;
  result.match_over = match_over;
  result.totals[0] = total;
  recorder.Finish(result);
  return recorder.Serialize();
}

// The wire payload back to the document it is base64 of. base64.h is encode-only on purpose (the
// server never decodes), so the test carries the decoder it needs: the alphabet is the RFC 4648
// one, and a wrong payload shows up as a mismatch rather than as a parse error three layers down.
std::string Base64DecodeForTest(const std::string& text) {
  constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string bytes;
  int bits = 0;
  int accumulator = 0;
  for (const char c : text) {
    if (c == '=') {
      break;
    }
    const char* found = std::find(std::begin(kAlphabet), std::end(kAlphabet) - 1, c);
    EXPECT_NE(found, std::end(kAlphabet) - 1) << "not base64: " << c;
    accumulator = (accumulator << 6) | static_cast<int>(found - kAlphabet);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(static_cast<char>((accumulator >> bits) & 0xFF));
    }
  }
  return bytes;
}

std::string DecodePayload(const ReplayLoad& load) {
  EXPECT_EQ(load.encoding, "gzip");
  EXPECT_FALSE(load.payload.empty());
  return GzipDecompress(Base64DecodeForTest(load.payload));
}

TEST(ReplayStoreTest, MissingRoomIsNotFound) {
  TempDir dir("missing");
  const ReplayLoad load = LoadReplayFromDisk(dir.string(), kRoomId);
  EXPECT_FALSE(load.found);
  EXPECT_TRUE(load.payload.empty());
}

// The case the whole change exists for: a match is a directory of parts and the answer is still
// one document, which is what lets the client stay untouched.
TEST(ReplayStoreTest, HandPartsAreMergedIntoOneDocument) {
  TempDir dir("parts");
  dir.WriteGzip(ReplayRecorder::PartFileNameFor(kRoomId, 0), MakePart(kRoomId, 0, false, 0));
  dir.WriteGzip(ReplayRecorder::PartFileNameFor(kRoomId, 1), MakePart(kRoomId, 1, true, 4321));

  const ReplayLoad load = LoadReplayFromDisk(dir.string(), kRoomId);
  ASSERT_TRUE(load.found);
  const std::optional<json::Value> parsed = json::Value::Parse(DecodePayload(load), nullptr);
  ASSERT_TRUE(parsed.has_value());
  const json::Value& file = *parsed;
  EXPECT_EQ(file.Find("room_id")->AsString(), kRoomId);
  // Both hands' steps, in part order: the merge must not lose the earlier hand.
  const json::Value* steps = file.Find("steps");
  ASSERT_NE(steps, nullptr);
  ASSERT_EQ(steps->AsArray().size(), 4u);
  EXPECT_EQ(steps->AsArray()[0].Find("t")->AsInt(), 0);
  EXPECT_EQ(steps->AsArray()[3].Find("t")->AsInt(), 10100);
  // And the tail is the last part's, so a finished match reads as finished.
  EXPECT_TRUE(file.Find("match_over")->AsBool());
  EXPECT_EQ(file.Find("totals")->AsArray()[0].AsInt(), 4321);
}

// Order is by the index in the name, not by listing order or by the filesystem's mood: part 10
// must land after part 2 even though "10" < "2" as text.
TEST(ReplayStoreTest, PartsAreReadInNumericOrder) {
  TempDir dir("order");
  // Written out of order on purpose, and with a gap so the merge cannot be "sort by name".
  dir.WriteGzip(ReplayRecorder::PartFileNameFor(kRoomId, 10), MakePart(kRoomId, 10, true, 0));
  dir.WriteGzip(ReplayRecorder::PartFileNameFor(kRoomId, 2), MakePart(kRoomId, 2, false, 0));

  const ReplayLoad load = LoadReplayFromDisk(dir.string(), kRoomId);
  ASSERT_TRUE(load.found);
  const std::optional<json::Value> parsed = json::Value::Parse(DecodePayload(load), nullptr);
  ASSERT_TRUE(parsed.has_value());
  const json::Value* steps = parsed->Find("steps");
  ASSERT_NE(steps, nullptr);
  ASSERT_EQ(steps->AsArray().size(), 4u);
  EXPECT_EQ(steps->AsArray()[0].Find("t")->AsInt(), 20000) << "part 2 must come before part 10";
  EXPECT_EQ(steps->AsArray()[3].Find("t")->AsInt(), 100100);
}

// Other rooms' parts sit in the same directory; a prefix match that was too loose (or a suffix
// that was not checked) would merge a stranger's timeline into this room's.
TEST(ReplayStoreTest, OtherRoomsPartsAreIgnored) {
  TempDir dir("stranger");
  dir.WriteGzip(ReplayRecorder::PartFileNameFor(kRoomId, 0), MakePart(kRoomId, 0, false, 0));
  constexpr char kOther[] = "r-2o5endvwb8zcy";
  dir.WriteGzip(ReplayRecorder::PartFileNameFor(kOther, 1), MakePart(kOther, 1, true, 999));

  const ReplayLoad load = LoadReplayFromDisk(dir.string(), kRoomId);
  ASSERT_TRUE(load.found);
  const std::optional<json::Value> parsed = json::Value::Parse(DecodePayload(load), nullptr);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->Find("steps")->AsArray().size(), 2u);
  EXPECT_FALSE(parsed->Find("match_over")->AsBool());
}

// A truncated part is refused rather than spliced: half a hand in the middle of a timeline reads
// as a corrupt recording, which is a worse answer than the ROOM_NOT_FOUND this returns.
TEST(ReplayStoreTest, ATruncatedPartIsRefused) {
  TempDir dir("truncated");
  dir.WriteGzip(ReplayRecorder::PartFileNameFor(kRoomId, 0), MakePart(kRoomId, 0, false, 0));
  const std::string part = GzipCompress(MakePart(kRoomId, 1, true, 1));
  ASSERT_GT(part.size(), 10u);
  std::ofstream out(dir.path() / ReplayRecorder::PartFileNameFor(kRoomId, 1), std::ios::binary);
  out.write(part.data(), static_cast<std::streamsize>(part.size() / 2));
  out.close();

  EXPECT_FALSE(LoadReplayFromDisk(dir.string(), kRoomId).found);
}

// A part that inflates but is not a recording (no top-level `steps`) is refused for the same
// reason: merging would have to guess where the timeline goes.
TEST(ReplayStoreTest, APartWithoutATimelineIsRefused) {
  TempDir dir("not-a-recording");
  dir.WriteGzip(ReplayRecorder::PartFileNameFor(kRoomId, 0), R"({"format":"mahjong-replay"})");
  EXPECT_FALSE(LoadReplayFromDisk(dir.string(), kRoomId).found);
}

// The migration cases, newest first: a v2.6 whole-match file and a v2.5 plain one. Neither may
// become unreadable because this build writes parts instead; the second one also has to keep
// answering with no `encoding`, or a client that predates v2.6 would try to inflate JSON.
TEST(ReplayStoreTest, AWholeMatchFileStillReads) {
  TempDir dir("whole");
  dir.WriteGzip(ReplayRecorder::FileNameFor(kRoomId), MakePart(kRoomId, 0, true, 77));
  const ReplayLoad load = LoadReplayFromDisk(dir.string(), kRoomId);
  ASSERT_TRUE(load.found);
  EXPECT_EQ(load.encoding, "gzip");
  const std::optional<json::Value> parsed = json::Value::Parse(DecodePayload(load), nullptr);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->Find("room_id")->AsString(), kRoomId);
}

TEST(ReplayStoreTest, APlainLegacyFileIsServedAsIs) {
  TempDir dir("plain");
  const std::string text = MakePart(kRoomId, 0, true, 5);
  dir.WriteText(ReplayRecorder::LegacyFileNameFor(kRoomId), text);
  const ReplayLoad load = LoadReplayFromDisk(dir.string(), kRoomId);
  ASSERT_TRUE(load.found);
  EXPECT_TRUE(load.encoding.empty()) << "a plain recording must not claim to be gzip";
  EXPECT_EQ(load.payload, text) << "a legacy file is served verbatim, not re-encoded";
}

// A match recorded by this build wins over a stale single file in the same directory: the parts
// are the newer truth, and a leftover whole-match file is exactly what a half-finished migration
// looks like.
TEST(ReplayStoreTest, PartsWinOverAStaleWholeFile) {
  TempDir dir("both");
  dir.WriteGzip(ReplayRecorder::PartFileNameFor(kRoomId, 1), MakePart(kRoomId, 1, true, 4321));
  dir.WriteGzip(ReplayRecorder::FileNameFor(kRoomId), MakePart(kRoomId, 0, false, 1));

  const ReplayLoad load = LoadReplayFromDisk(dir.string(), kRoomId);
  ASSERT_TRUE(load.found);
  const std::optional<json::Value> parsed = json::Value::Parse(DecodePayload(load), nullptr);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->Find("steps")->AsArray().size(), 2u);
  EXPECT_TRUE(parsed->Find("match_over")->AsBool());
}

}  // namespace
}  // namespace mahjong
