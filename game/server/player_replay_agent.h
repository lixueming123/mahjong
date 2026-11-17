// The frames that read a finished match back: replay_request, PROTOCOL v2.3 §4.13.
//
// This owns the whole answer, not just the parse: the store lookup (which awaits the account
// database to authorize), the §5.18 splitting into as many `replay` frames as the payload needs,
// and the ROOM_NOT_FOUND for a recording this account may not read. The lookup is a plain
// `co_await` on the dispatch coroutine — PlayerActor holds its AsyncGate across the whole handler,
// so the mailbox cannot start the next message while this one is parked.

#pragma once

#include <string>

#include <exec/task.hpp>

#include "json.h"

namespace mahjong {

class PlayerActor;

class PlayerReplayAgent {
 public:
  explicit PlayerReplayAgent(PlayerActor& player) : player_(player) {}

  stdexec::task<void> HandleReplayRequest(json::Value frame);

 private:
  PlayerActor& player_;
};

}  // namespace mahjong
