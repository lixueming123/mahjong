// Replay recording and retrieval.
//
// Why record frames rather than actions: a replay must keep working after a rule change. A file
// of (seed, action list) would have to be re-simulated by whatever the rules are today, so
// yesterday's game would render yesterday's rules against today's engine — or, worse, not render
// at all. The frames are the game as it was played, so a recording is a fact, not a program.
//
// The recorder is deliberately dumb: it does not know a game, a rule or a phase. The room hands
// it each step's outbounds and (optionally) each seat's concealed hand; it appends them. That is
// the same boundary as game.h — the game decides, the room delivers, the recorder remembers.
//
// Hand snapshots are the one thing the wire does not carry for every seat: a bot has no endpoint,
// so its `state_sync` is never built (RoomActor::Context sets `wired`). The god view therefore
// needs `Game::ReplayHands()`, which the room asks for only while a recording is on.
//
// I/O never happens on a game thread. `ReplayRecorder` only accumulates in memory; `ReplayActor`
// (replay_store.h) owns the disk. The room serializes the file and posts it there, exactly like
// the lobby posts match records to DbActor: a JSON write on the room's thread would park the
// whole table behind a filesystem call.

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
  // The file format version. Bumped only when a reader would have to change; a reader that sees
  // a newer version refuses the file instead of guessing (replay.js does the same).
  static constexpr int kFormatVersion = 1;

  explicit ReplayRecorder(ReplayMeta meta);

  // One transition. |outbounds| is exactly what the room is about to deliver, so a broadcast
  // (empty recipients) is recorded once with all four seats as recipients and each private frame
  // with its own. |hands| is Game::ReplayHands(), or nullptr on a step that does not re-render
  // the table (no `state_sync`), which is most of them: a deadline-only step changes no hand.
  //
  // |t_ms| is the offset since the match started. It is used for pacing only — a replay must
  // never depend on it to be correct, because the original delays are wall clock.
  void AddStep(int64_t t_ms, const std::vector<Outbound>& outbounds, const std::array<std::string, kSeatCount>* hands);

  // Marks the match as finished and stores the final table. Called from the match_over result;
  // an unfinished match still serializes (a server that dies mid-match leaves a usable file).
  void Finish(const GameResult& result);

  const ReplayMeta& meta() const { return meta_; }
  size_t step_count() const { return steps_.size(); }
  bool finished() const { return finished_; }
  // True when any seat in this recording belongs to |account_id|. What §4.13 authorizes on.
  bool IncludesAccount(uint64_t account_id) const;

  // The file contents. Deterministic in the recorded steps: the same match recorded twice
  // produces byte-identical output (replay_test.cc asserts it).
  std::string Serialize() const;

  // `replays/<room_id>.json`. Room ids are `r-` + 13 lowercase base36 (lobby.cc: FormatRoomId),
  // so the name needs no escaping and cannot escape |dir|.
  static std::string FileNameFor(std::string_view room_id);

 private:
  // One recorded frame group: the seats it went to and the exact bytes.
  struct Step {
    int64_t t_ms = 0;
    // Changed hands only, as "1m,2m,..." (empty = unchanged). Fixed size so a step can index by
    // seat; the serializer drops the empty entries.
    std::array<std::string, kSeatCount> hands;
    struct Frame {
      std::vector<int> recipients;
      std::string frame;
    };
    std::vector<Frame> frames;
  };

  ReplayMeta meta_;
  std::vector<Step> steps_;
  std::array<std::string, kSeatCount> last_hands_ {};
  bool finished_ = false;
  // Final table, valid when finished_.
  bool match_over_ = false;
  std::array<int, kSeatCount> totals_ {};
  std::array<int, kSeatCount> final_rank_ {};
};

// The room's narrow view of the store, exactly like SeatEndpoint is the room's view of a seat:
// ReplayActor implements it, the room only depends on this interface, and a target that links the
// room shell (the bench, the frozen legacy room test) does not have to link the store.
//
// ReplayActor must derive *directly* from ReplaySink (single, non-virtual inheritance) so the base
// subobject stays at offset 0 — ActorRef casts the concrete actor pointer to the method pointer's
// class without a runtime check.
class ReplaySink {
 public:
  ReplaySink() = default;
  ReplaySink(const ReplaySink&) = delete;
  ReplaySink& operator=(const ReplaySink&) = delete;
  virtual ~ReplaySink() = default;

  // Fire and forget: the round is already over and the players have their result, so a failed
  // write is logged by the store and never propagates back to the game.
  virtual stdexec::task<void> Store(std::string room_id, std::string contents) = 0;
};

// Process-wide access, mirroring GetDbService(): main() installs the actor before the gateway
// exists, and every room reads that same object. Empty means recording is off — the unit-test path
// and `--replay-dir=""`.
//
// Reads (§4.13) deliberately do not live here: only the player side asks for a file, and it links
// the store anyway, so its narrower concrete call keeps this header free of that dependency.
class ReplayService {
 public:
  void SetSink(ex_actor::ActorRef<ReplaySink> sink) { sink_ = std::move(sink); }
  bool IsEmpty() const { return sink_.IsEmpty(); }

  [[nodiscard]] auto Store(std::string room_id, std::string contents) const {
    return sink_.Send<&ReplaySink::Store>(std::move(room_id), std::move(contents));
  }

 private:
  ex_actor::ActorRef<ReplaySink> sink_;
};

inline ReplayService& GetReplayService() {
  static ReplayService service;
  return service;
}

}  // namespace mahjong
