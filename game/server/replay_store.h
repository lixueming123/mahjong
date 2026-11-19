// ReplayActor: the only part of the server that touches replay files.
//
// Same reason as DbActor, and the same shape: a filesystem write is a blocking syscall, and
// ACCOUNT_DESIGN.md §6.1's rule is that a game handler must never block the actor world's
// threads. So the room does not write; it serializes the file in memory (that part is pure CPU on
// data it already has) and posts the text here. This actor runs on its own single-thread pool, so
// a slow disk parks exactly one thread that nothing else is scheduled on, and the mailbox keeps
// writes in the order the rooms reported them.
//
// gzip lives here for the same reason (gzip.h): deflate is CPU nobody's timeline should wait on,
// and a hand part is written once and never rewritten (ReplayRecorder::PartFileNameFor).
//
// Authorization (§4.13) is a database question, not a file question: see Load.
//
// Callers reach it through `GetReplayService()` for writes and `GetReplayStore()` for reads,
// exactly like `GetDbService()`: main() installs the actor before the gateway exists, and an empty
// service is the "recording disabled" configuration that unit tests and `--replay-dir=""` runs use.
//
// On disk a match is a directory of hand parts (`<room_id>.h<NN>.json.gz`), not one file: the room
// writes each hand once and drops it, which is what bounds a room's memory. LoadReplayFromDisk is
// where they are glued back into the single document §5.18 defines; nothing above this file (the
// player, the client, the wire) ever sees a part.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "ex_actor/api.h"
#include "replay.h"

namespace mahjong {

// One answer to a §4.13 request. |encoding| + |payload| are what §5.18's frames carry:
// ("gzip", base64 of a gzip stream) for a recording this server wrote, or ("", the JSON text) for
// one written before the store compressed.
struct ReplayLoad {
  bool found = false;  // a missing file and a refusal are the same answer (§4.13)
  std::string encoding;
  std::string payload;
};

// Reads one room's recording out of |dir| and frames it for the wire. Split out of ReplayActor so
// the layout rules can be tested without an actor, a database or a socket: the parts are the one
// thing a build can get wrong and still "work" on a server that has never restarted.
//
// Three layouts are read, in this order:
//   1. the hand parts this build writes (`<room_id>.h<NN>.json.gz`), merged back into one
//      document (MergeReplayParts) and compressed again for the wire;
//   2. a whole-match `<room_id>.json.gz` written by the previous build;
//   3. a plain `<room_id>.json` from before the store compressed (served as-is, no `encoding`).
// Anything unreadable — a truncated part, a part that is not a recording — is "not found" rather
// than a best-effort answer, because half a timeline is worse than an honest ROOM_NOT_FOUND.
ReplayLoad LoadReplayFromDisk(const std::string& dir, const std::string& room_id);

class ReplayActor : public ReplaySink {
 public:
  // |dir| is the directory every file lives in. Empty means "no storage": Store does nothing and
  // Load always answers "not found", which is what a build without a data directory wants.
  explicit ReplayActor(std::string dir);

  const std::string& dir() const { return dir_; }

  // (ReplaySink) Fire and forget, like DbActor::RecordMatch: the match is already over and the
  // players have their result, so a failed write is logged and never propagates to the game.
  //
  // One file per hand (`<room_id>.h<NN>.json.gz`, ReplayRecorder::PartFileNameFor), each written
  // once and never rewritten: the memory a room holds is one hand's frames, not the whole match,
  // and a crash mid-match still leaves every finished hand readable. Load merges the parts back.
  stdexec::task<void> Store(std::string room_id, int part_index, std::string contents) override;

  // Answers §4.13 for |requester_account_id|: whether that account sat in |room_id|'s game, and
  // the recording if it did.
  //
  // The seat table used to be read back out of the file, which stopped working the moment the
  // store compressed it. `matches` already holds the same fact (one row per account-backed seat
  // per round) and DbService keys both the write and this query by account id, so the check lands
  // in the same shard mailbox as the match record it depends on: the §4.1 FIFO order then covers
  // "the round just ended, ask for its replay immediately" with no extra table to keep in sync.
  //
  // This handler is the one place in the server that awaits another actor: it suspends on the db
  // shard and resumes on this actor's own scheduler (stdexec's `task` is affine). While it is
  // parked, a Store of the same room may run — the two share `dir_` (read-only) and nothing else,
  // so they interleave safely without a gate.
  stdexec::task<ReplayLoad> Load(std::string room_id, uint64_t requester_account_id);

  // Method list for `Send<&ReplayActor::X>`; a method missing here cannot be addressed.
  constexpr static auto kActorMethods = std::make_tuple(&ReplayActor::Store, &ReplayActor::Load);

 private:
  std::string dir_;
};

// Process-wide access for the READ side only (the write side is ReplayService in replay.h, which
// the room shell can link without pulling this file in). main() spawns one actor and registers it
// in both; the player side calls Load through this narrower view because it links the store anyway.
class ReplayStore {
 public:
  void SetActor(ex_actor::ActorRef<ReplayActor> actor) { actor_ = std::move(actor); }
  bool IsEmpty() const { return actor_.IsEmpty(); }
  const ex_actor::ActorRef<ReplayActor>& actor() const { return actor_; }

  [[nodiscard]] auto Store(std::string room_id, int part_index, std::string contents) const {
    return actor_.Send<&ReplayActor::Store>(std::move(room_id), part_index, std::move(contents));
  }
  [[nodiscard]] auto Load(std::string room_id, uint64_t requester_account_id) const {
    return actor_.Send<&ReplayActor::Load>(std::move(room_id), requester_account_id);
  }

 private:
  ex_actor::ActorRef<ReplayActor> actor_;
};

inline ReplayStore& GetReplayStore() {
  static ReplayStore service;
  return service;
}

}  // namespace mahjong
