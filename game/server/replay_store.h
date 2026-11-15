// ReplayActor: the only part of the server that touches replay files.
//
// Same reason as DbActor, and the same shape: a filesystem write is a blocking syscall, and
// ACCOUNT_DESIGN.md §6.1's rule is that a game handler must never block the actor world's
// threads. So the room does not write; it serializes the file in memory (that part is pure CPU on
// data it already has) and posts the text here. This actor runs on its own single-thread pool, so
// a slow disk parks exactly one thread that nothing else is scheduled on, and the mailbox keeps
// writes in the order the rooms reported them.
//
// gzip lives here for the same reason (gzip.h): 1.5 MB of JSON is ~12 ms of deflate, which is five
// frames of one table's timeline and would be five frames late if it ran on the room's thread.
//
// Authorization (§4.13) is a database question, not a file question: see Load.
//
// Callers reach it through `GetReplayService()` for writes and `GetReplayStore()` for reads,
// exactly like `GetDbService()`: main() installs the actor before the gateway exists, and an empty
// service is the "recording disabled" configuration that unit tests and `--replay-dir=""` runs use.

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

class ReplayActor : public ReplaySink {
 public:
  // |dir| is the directory every file lives in. Empty means "no storage": Store does nothing and
  // Load always answers "not found", which is what a build without a data directory wants.
  explicit ReplayActor(std::string dir);

  const std::string& dir() const { return dir_; }

  // (ReplaySink) Fire and forget, like DbActor::RecordMatch: the match is already over and the
  // players have their result, so a failed write is logged and never propagates to the game. The
  // whole file is replaced each time (the room calls this once per settled round), so a crash
  // mid-match still leaves every finished round readable.
  stdexec::task<void> Store(std::string room_id, std::string contents) override;

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
  // Reads and gunzips. Returns "not found" for a refusal, a missing file or an unreadable one:
  // §4.13 collapses all of them into ROOM_NOT_FOUND.
  ReplayLoad LoadFile(const std::string& room_id) const;

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

  [[nodiscard]] auto Store(std::string room_id, std::string contents) const {
    return actor_.Send<&ReplayActor::Store>(std::move(room_id), std::move(contents));
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
