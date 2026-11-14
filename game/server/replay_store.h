// ReplayActor: the only part of the server that touches replay files.
//
// Same reason as DbActor, and the same shape: a filesystem write is a blocking syscall, and
// ACCOUNT_DESIGN.md §6.1's rule is that a game handler must never block the actor world's
// threads. So the room does not write; it serializes the file in memory (that part is pure CPU on
// data it already has) and posts the text here. This actor runs on its own single-thread pool, so
// a slow disk parks exactly one thread that nothing else is scheduled on, and the mailbox keeps
// writes in the order the rooms reported them.
//
// The read side is here for the same reason plus one more: it validates the request against the
// file. `room_id` alone is not authorization — §4.13 asks whether the requester sat in that game,
// and the seat table lives in the file.
//
// Callers reach it through `GetReplayService()`, exactly like `GetDbService()`: main() installs
// the actor before the gateway exists, and an empty service is the "recording disabled"
// configuration that unit tests and `--replay-dir=""` runs use.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "ex_actor/api.h"
#include "replay.h"

namespace mahjong {

class PlayerActor;

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

  // Reads `room_id`'s file and answers |reply| with the text, or with the reason it refused.
  // Never suspends on the caller: the answer is a message back into PlayerActor's mailbox
  // (player_actor.h keeps its no-suspension rule).
  stdexec::task<void> Load(std::string room_id, uint64_t requester_account_id, ex_actor::ActorRef<PlayerActor> reply);

  // Method list for `Send<&ReplayActor::X>`; a method missing here cannot be addressed.
  constexpr static auto kActorMethods = std::make_tuple(&ReplayActor::Store, &ReplayActor::Load);

 private:
  // Reads and validates. Returns the file text on success; on failure fills |error| with the
  // protocol error code to answer (§4.13 collapses every refusal into ROOM_NOT_FOUND).
  std::string LoadFile(const std::string& room_id, uint64_t requester_account_id, bool* ok) const;

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
  [[nodiscard]] auto Load(std::string room_id, uint64_t requester_account_id,
                          ex_actor::ActorRef<PlayerActor> reply) const {
    return actor_.Send<&ReplayActor::Load>(std::move(room_id), requester_account_id, std::move(reply));
  }

 private:
  ex_actor::ActorRef<ReplayActor> actor_;
};

inline ReplayStore& GetReplayStore() {
  static ReplayStore service;
  return service;
}

}  // namespace mahjong
