// Replay recording and retrieval.
//
// Recorded as frames rather than actions: a file of (seed, action list) would have to be
// re-simulated by today's rules, so yesterday's game would render with today's engine. The frames
// are the game as it was played, so a recording is a fact, not a program.
//
// The recorder knows no game, rule or phase: the room hands it each step's outbounds and
// (optionally) each seat's own view.
//
// A seat's view is recorded because the wire does not carry it for every seat — a seat with no
// endpoint has its private frames filtered out before they are built.
//
// I/O never happens on a game thread: `ReplayRecorder` only accumulates in memory, and
// `ReplayActor` (replay_store.h) owns the disk. One recorder covers one hand, so a match is a
// series of parts, each written once and dropped; `MergeReplayParts` glues them back together.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ex_actor/api.h"
#include "game.h"
#include "json.h"
#include "room_contract.h"

namespace mahjong {

// The fixed properties of a recording: everything a player needs before the first step.
struct ReplaySeat {
  int seat = 0;
  std::string player_id;
  std::string name;
  bool is_bot = false;
  uint64_t account_id = 0;  // 0 for a guest or a bot: such a seat cannot claim its own replay
};

struct ReplayMeta {
  std::string room_id;
  std::string game_id;
  std::string options_json;
  uint64_t seed = 0;
  int64_t started_at_ms = 0;
  std::array<ReplaySeat, kSeatCount> seats {};
};

class ReplayRecorder {
 public:
  // The file format version; bumped only when a reader would have to change (replay.js refuses a
  // newer file instead of guessing).
  //
  // 2: `steps[].v` (one opaque per-seat view) replaced `steps[].h` + `steps[].m`. A v1 reader
  //    ignores `v`; a v2 reader still accepts v1 files.
  static constexpr int kFormatVersion = 2;

  explicit ReplayRecorder(ReplayMeta meta);

  // One transition. |outbounds| is exactly what the room is about to deliver, so a broadcast (empty
  // recipients) is recorded once with all four seats as recipients. |views| is
  // Game::ReplaySeatViews(), or nullptr on a step that does not re-render the table.
  //
  // |t_ms| is the offset since the match started; pacing only, since the original delays are wall
  // clock.
  void AddStep(int64_t t_ms, const std::vector<Outbound>& outbounds, const SeatViews* views);

  // Marks the match as finished and stores the final table. An unfinished match still serializes,
  // so a server that dies mid-match leaves a usable file.
  void Finish(const GameResult& result);

  const ReplayMeta& meta() const { return meta_; }
  size_t step_count() const { return steps_.size(); }
  bool finished() const { return finished_; }

  // The file contents, deterministic in the recorded steps: the same match recorded twice produces
  // byte-identical output (replay_test.cc asserts it).
  //
  // A recorder covers ONE hand, so this text is one *part* of the match — a complete document on
  // its own.
  std::string Serialize() const;

  // `replays/<room_id>.json.gz`. Room ids are `r-` + 13 lowercase base36, so the name needs no
  // escaping and cannot escape |dir|. Only ever read now (a whole-match recording); this build
  // writes parts.
  static std::string FileNameFor(std::string_view room_id);

  // `replays/<room_id>.h00.json.gz`: the name one hand's part is written under. The index is
  // zero-padded so a directory listing sorts the parts in play order, which is the order the reader
  // merges them in.
  static std::string PartFileNameFor(std::string_view room_id, int part_index);

  // `replays/<room_id>.json`: the uncompressed name, still read so a restart does not orphan
  // recordings written by the previous build. Never written now.
  static std::string LegacyFileNameFor(std::string_view room_id);

 private:
  // One recorded frame group: the seats it went to and the exact bytes.
  struct Step {
    int64_t t_ms = 0;
    // Changed views only, as rendered JSON (empty = unchanged). Fixed size so a step can index by
    // seat; the serializer drops the empty entries.
    SeatViews views;
    struct Frame {
      std::vector<int> recipients;
      std::string frame;
    };
    std::vector<Frame> frames;
  };

  ReplayMeta meta_;
  std::vector<Step> steps_;
  SeatViews last_views_ {};
  bool finished_ = false;
  // Final table, valid when finished_.
  bool match_over_ = false;
  std::array<int, kSeatCount> totals_ {};
  std::array<int, kSeatCount> final_rank_ {};
};

// Glues one match's hand parts back into the single document a client asks for, so that client
// sees no difference between a split recording and an older one.
//
// Two rules make it a text splice rather than a re-serialize:
//   * the head (format … seats) is the FIRST part's, the tail (finished, match_over, totals,
//     final_rank, steps) is the LAST part's: those fields describe the match when that part was
//     written, and only the last one knows whether it ended;
//   * the `steps` arrays are concatenated in part order, spliced rather than parsed — rebuilding a
//     600 KB document in `json::Value` (96 bytes per element) would buy no change of bytes.
//
// The splice is safe because the recorder is the only producer and `json::Value` escapes every
// string it writes.
//
// Returns an empty string when a part cannot be scanned (the caller answers "no such recording").
// A part with no steps still counts as a tail candidate: a step can settle two rounds at once, and
// the second one carries `match_over`.
std::string MergeReplayParts(const std::vector<std::string>& parts);

// The room's narrow view of the store, exactly like SeatEndpoint is the room's view of a seat: a
// target that links the room shell (the bench, the frozen legacy room test) does not have to link
// the store.
//
// ReplayActor must derive *directly* from ReplaySink (single, non-virtual inheritance) so the base
// subobject stays at offset 0 — ActorRef casts the concrete actor pointer without a runtime check.
class ReplaySink {
 public:
  ReplaySink() = default;
  ReplaySink(const ReplaySink&) = delete;
  ReplaySink& operator=(const ReplaySink&) = delete;
  virtual ~ReplaySink() = default;

  // Fire and forget: the round is already over and the players have their result, so a failed write
  // is logged by the store and never propagates back to the game.
  //
  // |contents| is ONE hand's part; the store never merges or deletes anything, so a match is a
  // directory of parts read back through MergeReplayParts.
  virtual stdexec::task<void> Store(std::string room_id, int part_index, std::string contents) = 0;
};

// Process-wide write access, mirroring GetDbService(): main() installs the actor before the gateway
// exists. Empty means recording is off — the unit-test path and `--replay-dir=""`.
//
// Reads deliberately do not live here: only the player side asks for a file, and it links the store
// anyway.
class ReplayService {
 public:
  void SetSink(ex_actor::ActorRef<ReplaySink> sink) { sink_ = std::move(sink); }
  bool IsEmpty() const { return sink_.IsEmpty(); }

  [[nodiscard]] auto Store(std::string room_id, int part_index, std::string contents) const {
    return sink_.Send<&ReplaySink::Store>(std::move(room_id), part_index, std::move(contents));
  }

 private:
  ex_actor::ActorRef<ReplaySink> sink_;
};

inline ReplayService& GetReplayService() {
  static ReplayService service;
  return service;
}

}  // namespace mahjong
