// ReplayActor: the only part of the server that touches replay files.
//
// Same shape as DbActor: a filesystem write is a blocking syscall, so the room does not write — it
// serializes in memory and posts the text here. This actor runs on its own single-thread pool, so a
// slow disk parks one thread nothing else is scheduled on, and the mailbox keeps writes in order.
//
// gzip lives here for the same reason: deflate is CPU nobody's timeline should wait on.
//
// Callers reach it through `GetReplayService()` for writes and `GetReplayStore()` for reads. An
// empty service is "recording disabled", which unit tests and `--replay-dir=""` use.
//
// On disk a match is a directory of hand parts (`<room_id>.h<NN>.json.gz`), not one file:
// LoadReplayFromDisk glues them back into the single document a client asks for.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "ex_actor/api.h"
#include "replay.h"

namespace mahjong {

// One answer to a replay request. |encoding| + |payload| are what the frames carry: ("gzip", base64
// of a gzip stream) for a recording this server wrote, or ("", the JSON text) for one written before
// the store compressed.
struct ReplayLoad {
  bool found = false;  // a missing file and a refusal are the same answer
  std::string encoding;
  std::string payload;
};

// Reads one room's recording out of |dir| and frames it for the wire. Split out of ReplayActor so the
// layout rules can be tested without an actor, a database or a socket.
//
// Three layouts are read, in this order:
//   1. the hand parts this build writes (`<room_id>.h<NN>.json.gz`), merged (MergeReplayParts) and
//      compressed again for the wire;
//   2. a whole-match `<room_id>.json.gz` written by the previous build;
//   3. a plain `<room_id>.json` from before the store compressed (served as-is, no `encoding`).
// Anything unreadable is "not found" rather than a best-effort answer: half a timeline is worse than
// an honest ROOM_NOT_FOUND.
ReplayLoad LoadReplayFromDisk(const std::string& dir, const std::string& room_id);

class ReplayActor : public ReplaySink {
 public:
  // |dir| is the directory every file lives in. Empty means "no storage": Store does nothing and Load
  // always answers "not found".
  explicit ReplayActor(std::string dir);

  const std::string& dir() const { return dir_; }

  // (ReplaySink) Fire and forget. One file per hand, each written once, so a room holds one hand's
  // frames and a crash mid-match still leaves every finished hand readable.
  stdexec::task<void> Store(std::string room_id, int part_index, std::string contents) override;

  // Whether |requester_account_id| sat in |room_id|'s game, and the recording if it did. The check is
  // a database question (see DbActor::ReplayAccess), keyed by account so it is ordered after the
  // match record it depends on.
  //
  // The one handler here that awaits another actor: it suspends on the db shard and resumes on this
  // actor's own scheduler. While parked, a Store of the same room may run — they share only `dir_`,
  // so they interleave safely without a gate.
  stdexec::task<ReplayLoad> Load(std::string room_id, uint64_t requester_account_id);

  // Method list for `Send<&ReplayActor::X>`; a method missing here cannot be addressed.
  constexpr static auto kActorMethods = std::make_tuple(&ReplayActor::Store, &ReplayActor::Load);

 private:
  std::string dir_;
};

// Process-wide access for the READ side only (writes are ReplayService in replay.h, which the room
// shell can link without pulling this file in). main() registers the actor in both.
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
