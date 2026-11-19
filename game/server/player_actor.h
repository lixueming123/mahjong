// PlayerActor: the player a TCP connection speaks for, and the one entry point for client protocol.
//
// One connection carries at most one player, so the player is deliberately not a second actor:
// splitting identity out of this mailbox would turn every "who sent this frame" into a cross-actor
// query on the hot path. `Connection` stays the socket boundary and `Session` (session.h) holds who
// this player currently is.
//
// The protocol itself is not written here. It is four agents — session, lobby, room, replay — each a
// plain member holding a reference back to this actor. The table mapping a wire type to one of their
// methods is player_agent.h; adding a frame does not touch this file. What stays here is what only
// the actor can own: the mailbox, the dispatch gate, the quota counters and the retire handshake.
//
// `Dispatch` deliberately does not lock (see player_actor.cc): handlers must not touch actor state
// after a suspension, and the replay lookup — the only one that suspends — copies what it needs out
// first. A mailbox already serializes handlers that run to completion.
//
// Accounts stay asynchronous: MySQL *blocks*, so the query lives on DbActor's pool (db.h) and the
// answer arrives later as kOnLoginResult / kOnRegisterResult / kOnStatsResult.

#pragma once

#include <memory>
#include <string>

#include "connection.h"
#include "db.h"
#include "ex_actor/api.h"
#include "frame_parser.h"
#include "json.h"
#include "lobby.h"
#include "player.h"
#include "player_agent.h"
#include "player_lobby_agent.h"
#include "player_replay_agent.h"
#include "player_room_agent.h"
#include "player_session_agent.h"
#include "protocol.h"
#include "room_endpoint.h"
#include "seat_endpoint.h"
#include "session.h"

namespace mahjong {

class PlayerActor : public SeatEndpoint {
 public:
  PlayerActor(std::shared_ptr<Connection> conn, std::shared_ptr<PlayerIdAllocator> player_ids,
              std::shared_ptr<SessionRegistry> sessions, ex_actor::ActorRef<LobbyActor> lobby);

  // The one public entry (seat_endpoint.h): gateway frames and socket events, account results, and the
  // seat's frame/error/close deliveries all arrive here.
  stdexec::task<void> Dispatch(PlayerEnvelope envelope) override;
  // Introspection (logging and tests), deliberately outside the gate.
  const std::string& player_id() const { return session_.player_id(); }

 private:
  // The bridge that turns table rows into agent calls (player_agent.h). A friend because the table
  // holds the addresses of the forwarding thunks below.
  friend class PlayerFrames;
  friend class PlayerSessionAgent;
  friend class PlayerLobbyAgent;
  friend class PlayerRoomAgent;
  friend class PlayerReplayAgent;

  // ---- the narrow surface the agents share -----------------------------------------------
  // Reads. Writes never happen through these: a frame answers the client via SendFrame/SendError, and
  // an identity transition only happens in the session agent's own methods.
  const Session& session() const { return session_; }
  const ex_actor::ActorRef<LobbyActor>& lobby() const { return lobby_; }
  const ex_actor::ActorRef<RoomEndpoint>& room() const { return room_; }
  const ex_actor::ActorRef<PlayerActor>& self() const { return self_ref_; }
  const std::shared_ptr<Connection>& connection() const { return conn_; }
  const std::shared_ptr<PlayerIdAllocator>& player_ids() const { return player_ids_; }
  const std::shared_ptr<SessionRegistry>& sessions() const { return sessions_; }
  PlayerSessionAgent& session_agent() { return session_agent_; }
  PlayerLobbyAgent& lobby_agent() { return lobby_agent_; }
  PlayerRoomAgent& room_agent() { return room_agent_; }
  PlayerReplayAgent& replay_agent() { return replay_agent_; }

  // Answers the client. ReplyParseError charges the counter the parser's ErrorKind names,
  // BadRequest charges the bad-frame counter, and SendError charges nothing.
  void ReplyParseError(const frame_parser::Error& error);
  void SendError(protocol::ErrorCode code, std::string_view message);
  void SendFrame(std::string frame);
  void BadRequest(std::string_view message);
  // Sends an error and accounts the illegal-operation quota.
  void IllegalOperation(protocol::ErrorCode code, std::string_view message);

  // Session identity transitions. Only the session agent drives these, and only after a database
  // answer.
  Session& writable_session() { return session_; }
  // How many database replies this actor is still owed. DbActor holds this actor's ActorRef while one
  // is outstanding, so a retire defers the destroy until this reaches zero.
  void OweDatabaseReply() { ++db_replies_in_flight_; }

  // ---- actor lifecycle, kept here because only the actor owns it --------------------------
  void SetSelfRef(ex_actor::ActorRef<PlayerActor> self);
  // The dispatch gate, applied before a frame's handler parses it: the binding row names the phase
  // each frame is accepted in.
  bool AllowFrame(FrameGate gate);
  void OnClosed(std::string reason);
  // PlayerEnvelope::Retire arrives here: the last message this actor ever receives.
  void RetireRequested();
  // The frame path: parse, look the type up in the protocol table, apply its gate, run its handler.
  stdexec::task<void> OnFrame(std::string line);
  void OnIdleTimeout();
  // The lobby tells this connection which room it plays in, so gameplay frames skip the lobby.
  void SetRoom(ex_actor::ActorRef<RoomEndpoint> room);
  // The SeatEndpoint view (room/lobby deliveries).
  void SeatFrame(std::string frame);
  void SeatError(std::string frame, QuotaKind quota);
  void Close(std::string reason);
  PlayerInfo Player() const { return session_.Info(); }
  // Destroys this actor once a retire has been requested and no reply is owed.
  void RetireIfIdle();

  // The four protocol agents. A reference to this actor, nothing else: their state is this player's
  // state, which is why they are objects here rather than actors of their own.
  PlayerSessionAgent session_agent_ {*this};
  PlayerLobbyAgent lobby_agent_ {*this};
  PlayerRoomAgent room_agent_ {*this};
  PlayerReplayAgent replay_agent_ {*this};

  std::shared_ptr<Connection> conn_;
  std::shared_ptr<PlayerIdAllocator> player_ids_;
  std::shared_ptr<SessionRegistry> sessions_;
  ex_actor::ActorRef<LobbyActor> lobby_;
  ex_actor::ActorRef<PlayerActor> self_ref_;
  // Identity and phase live in one value (session.h) instead of loose bools: five states, not sixteen
  // combinations.
  Session session_;
  // Routing cache, not identity: the lobby keeps the same fact as a room_id string, this side needs the
  // ref to skip the lobby on every gameplay frame.
  ex_actor::ActorRef<RoomEndpoint> room_;
  // See OweDatabaseReply().
  int db_replies_in_flight_ = 0;
  // Set by RetireRequested(): destroy as soon as nothing is owed.
  bool retire_requested_ = false;
  // Set once the destroy has been requested, so a second reply cannot ask for it again.
  bool retired_ = false;
};

}  // namespace mahjong
