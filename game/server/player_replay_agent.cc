// The replay-owned half of a player's protocol: one frame, PROTOCOL v2.3 §4.13.
//
// This is the one handler in the player that suspends. It awaits the store, which awaits the
// account database to authorize the request, so the answer arrives after two round trips. Because
// the dispatch holds PlayerActor's AsyncGate across the await, the mailbox cannot start the next
// message meanwhile, and the actor — including `this` — is guaranteed to still be alive: a retire
// is just another message queued behind this one.

#include "player_replay_agent.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "frame_parser.h"
#include "player_actor.h"
#include "protocol.h"
#include "replay_store.h"

namespace mahjong {
namespace {

// Frames one loaded recording as however many §5.18 `replay` frames it takes and queues them on
// |connection|. It captures no actor: the connection is what survives, and SendFrame is
// thread-safe and a no-op once the socket is closed, so a client that hung up gets nothing.
//
// §5.18 splits here rather than in the store because the 64 KB line limit is a property of the
// wire (§1.2), and this is the side that owns the outbox.
void SendReplayFrames(const std::shared_ptr<Connection>& connection, std::string room_id, const ReplayLoad& loaded) {
  if (!loaded.found) {
    // §4.13: one code for "no such recording" and "not yours" — a stranger must not be able to
    // probe which room ids were played.
    connection->SendFrame(protocol::ReplayErrorFrame(room_id, protocol::ErrorCode::kRoomNotFound));
    return;
  }
  for (std::string& frame : protocol::SplitReplayFrames(room_id, loaded.encoding, loaded.payload)) {
    connection->SendFrame(std::move(frame));
  }
}

}  // namespace

stdexec::task<void> PlayerReplayAgent::HandleReplayRequest(json::Value frame) {
  frame_parser::Error parse_error;
  std::optional<frame_parser::ReplayRequest> intent = frame_parser::ParseReplayRequest(frame, &parse_error);
  if (!intent.has_value()) {
    player_.ReplyParseError(parse_error);
    co_return;
  }
  std::string room_id = std::move(intent->room_id);
  ReplayStore& store = GetReplayStore();
  if (store.IsEmpty() || player_.self().IsEmpty()) {
    // No store installed is the "recording disabled" build: answer the same ROOM_NOT_FOUND a
    // missing file gets, so the client needs no second code path (§4.13).
    player_.SendFrame(protocol::ReplayErrorFrame(room_id, protocol::ErrorCode::kRoomNotFound));
    co_return;
  }
  // Authorization is by account (§4.13): only the account that sat in that game may read it. The
  // lookup is a plain await — the caller holds the gate, so no other message touches this player
  // while it is parked — and only the connection crosses the suspension, as a shared_ptr copy.
  std::shared_ptr<Connection> connection = player_.connection();
  const uint64_t account_id = player_.session().account_id();
  const ReplayLoad loaded = co_await store.Load(room_id, account_id);
  SendReplayFrames(connection, std::move(room_id), loaded);
  co_return;
}

}  // namespace mahjong
