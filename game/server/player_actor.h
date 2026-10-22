// PlayerActor: the player a TCP connection speaks for, and the one entry point for client
// protocol.
//
// One connection carries at most one player, so the player is deliberately not a second actor:
// splitting identity out of this mailbox would turn every "who sent this frame" into a
// cross-actor query on the hot path, or force a mirror of the answer to be kept here. What the
// old name got wrong was the role, not the boundary — this class *is* the player, so it is
// named for one and owns the protocol outright. `Connection` stays the socket boundary (outbox,
// close request, quota counters) and `Session` (session.h) holds who this player currently is.
//
// Every client frame is decoded, gated and routed here (OnFrame + kFrameRoutes) — never in the
// gateway, the lobby or a room. `frame_parser` turns a frame into an intent (names, shapes,
// tile codes), this class decides whether the player may send it at all (login gate §4.1) and
// which actor owns the intent, and the room re-checks only what is game state: whose turn it
// is, whether the tile is in hand, whether the window is still open (§4.5/§6.3).
//
// It is also the seat endpoint a RoomActor delivers game frames to, which is why it derives from
// SeatEndpoint: the room holds `ActorRef<SeatEndpoint>` and never sees this class.
//
// Every message arrives through `Dispatch`. No arm suspends, so the mailbox already runs one
// message to completion before the next: there is no gate. A later `co_await` in here would
// let the following message start, and would need a lock around the session fields it touches.
//
// Accounts stay asynchronous (ACCOUNT_DESIGN.md §6.1) for a different reason: a password check
// needs MySQL and MySQL *blocks*, so the query lives on DbActor's own single-thread pool
// (db.h) and this actor only posts it a message. The answer arrives later as kOnLoginResult /
// kOnRegisterResult / kOnStatsResult, which are the only places that touch the session's
// identity state. Nothing here awaits the database, so this file keeps its no-suspension rule.
//
// "Who this player is" lives in one `Session` (session.h) rather than in loose fields: the
// five reachable states are an enum there, and every transition happens inside that type. This
// class only decides *when* to ask for one, so the state machine is testable without a socket
// (session_test.cc) and the frame shape rules without an actor (frame_parser.h).

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
#include "seat_endpoint.h"
#include "session.h"

namespace mahjong {

class PlayerActor : public SeatEndpoint {
 public:
  PlayerActor(std::shared_ptr<Connection> conn, std::shared_ptr<PlayerIdAllocator> player_ids, DbService db,
              std::shared_ptr<SessionRegistry> sessions, ex_actor::ActorRef<LobbyActor> lobby);

  // The one public entry (seat_endpoint.h): gateway frames and socket events, account results,
  // and the seat's frame/error/close deliveries all arrive here, each as its own message type.
  exec::task<void> Dispatch(PlayerEnvelope envelope) override;

  // Introspection (logging and tests); like the room's getters it is deliberately outside the
  // gate, and nothing outside this actor reads it in production.
  const std::string& player_id() const { return session_.player_id(); }

  // The surface a frame handler may use (player_agent.h): answer this client, and start an
  // account operation. Everything else a handler needs arrives in PlayerServices, so it cannot
  // reach the account registry, the database or the quota rules directly — that is how a player
  // turns back into a god object.
  //
  // Answers a frame the parser rejected, charging the §7.2 counter its kind names.
  void ReplyParseError(const frame_parser::Error& error);
  void SendError(std::string_view code, std::string_view message);
  // Queues one frame to this client.
  void SendFrame(std::string frame);
  // Answers BAD_REQUEST and charges the §7.2 bad-frame counter.
  void BadRequest(std::string_view message);
  // True while this player may still start a login or register (§3.1): no identity yet, none in
  // flight. Checked before parsing, so a second login is refused as such rather than as a
  // malformed frame — the two are charged to different §7.2 counters.
  bool CanStartAuth() const { return session_.CanStartAuth(); }

  // The account entry points. Each owns the "no database" refusal (INTERNAL), so a handler never
  // has to know about quotas, and the password never travels further than this class (§6.1).
  void BeginAccountLogin(std::string username, std::string password);
  void LoginAsGuest(std::string name);
  void BeginRegister(std::string username, std::string password, std::string nickname);
  void RequestStats();

 private:
  void SetSelfRef(ex_actor::ActorRef<PlayerActor> self);
  void OnFrame(std::string line);
  void OnClosed(std::string reason);
  void OnIdleTimeout();
  void OnLoginResult(AuthOutcome outcome);
  void OnRegisterResult(AuthOutcome outcome);
  void OnStatsResult(StatsOutcome outcome);
  // The lobby tells this connection which room it plays in (PlayerEnvelope::SetRoom), so
  // gameplay frames no longer have to round-trip through the lobby.
  void SetRoom(ex_actor::ActorRef<RoomActor> room);
  // The SeatEndpoint view (room/lobby deliveries).
  void SeatFrame(std::string frame);
  void SeatError(std::string frame, QuotaKind quota);
  void Close(std::string reason);

  // The references frame handlers operate on, bound fresh for each dispatch.
  PlayerServices Services() {
    return PlayerServices {.player = *this, .session = session_, .lobby = lobby_, .room = room_, .self = self_ref_};
  }

  // Sends an error and accounts the illegal-operation quota (§7.2).
  void IllegalOperation(std::string_view code, std::string_view message);
  PlayerInfo Player() const { return session_.Info(); }

  // Runs the final half of a successful register/login: claims the account for this
  // connection (or answers ALREADY_ONLINE) and sends login_ok. Returns whether it took it.
  bool AcceptAccount(const AuthOutcome& outcome);
  // Answers INTERNAL for "MySQL is not usable" (ACCOUNT_DESIGN.md §2.3). Deliberately not
  // charged to any quota: the client did nothing wrong.
  void AccountServiceUnavailable();

  std::shared_ptr<Connection> conn_;
  std::shared_ptr<PlayerIdAllocator> player_ids_;
  // Empty in a build/test that has no database: the account frames then answer INTERNAL,
  // which is the same degraded path as a MySQL that is down (§2.3).
  DbService db_;
  std::shared_ptr<SessionRegistry> sessions_;
  ex_actor::ActorRef<LobbyActor> lobby_;
  ex_actor::ActorRef<PlayerActor> self_ref_;
  // Identity and phase live in one value (session.h) instead of loose bools: five states, not
  // sixteen combinations, and every transition is visible in that one file.
  Session session_;
  // Routing cache, not identity: the lobby keeps the same fact as a room_id string and this
  // side needs the ref to skip the lobby on every gameplay frame. Empty until the lobby sets it.
  ex_actor::ActorRef<RoomActor> room_;
};

}  // namespace mahjong
