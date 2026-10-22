// ConnectionActor: per-connection session handling (one TCP connection == at most one player).
//
// It parses frames, enforces the connection-scoped rules of PROTOCOL.md §4 (login, name
// validation, frame shape, error codes, quotas) and routes room intents to LobbyActor. It is
// also the seat endpoint a RoomActor delivers game frames to, which is why it derives from
// SeatEndpoint: the room holds `ActorRef<SeatEndpoint>` and never sees this class.
//
// Every message arrives through `Dispatch`. No arm suspends, so the mailbox already runs one
// message to completion before the next: there is no gate. A later `co_await` in here would
// let the following message start, and would need a lock around the session fields it touches.
//
// Accounts stay asynchronous (ACCOUNT_DESIGN.md §6.1) for a different reason: a password check
// needs MySQL and MySQL *blocks*, so the actor submits the query to AccountStore and returns;
// the answer arrives later as kOnLoginResult / kOnRegisterResult / kOnStatsResult, which are the
// only places that touch the session's identity state.

#pragma once

#include <memory>
#include <string>

#include "ex_actor/api.h"

#include "connection.h"
#include "db.h"
#include "json.h"
#include "lobby.h"
#include "player.h"
#include "seat_endpoint.h"

namespace mahjong {

class ConnectionActor : public SeatEndpoint {
 public:
  ConnectionActor(std::shared_ptr<Connection> conn, std::shared_ptr<PlayerIdAllocator> player_ids,
                  std::shared_ptr<AccountStore> accounts, std::shared_ptr<SessionRegistry> sessions,
                  ex_actor::ActorRef<LobbyActor> lobby);

  // The one public entry (seat_endpoint.h): gateway frames and socket events, account results,
  // and the seat's frame/error/close deliveries all arrive here, carrying their envelope kind.
  exec::task<void> Dispatch(ConnectionEnvelope envelope) override;

  // Introspection (logging and tests); like the room's getters it is deliberately outside the
  // gate, and nothing outside this actor reads it in production.
  const std::string& player_id() const { return player_id_; }

 private:
  void SetSelfRef(ex_actor::ActorRef<ConnectionActor> self);
  void OnFrame(std::string line);
  void OnClosed(std::string reason);
  void OnIdleTimeout();
  void OnLoginResult(AuthOutcome outcome);
  void OnRegisterResult(AuthOutcome outcome);
  void OnStatsResult(StatsOutcome outcome);
  // The lobby tells this connection which room it plays in (ConnectionEnvelope::SetRoom), so
  // gameplay frames no longer have to round-trip through the lobby.
  void SetRoom(ex_actor::ActorRef<RoomActor> room);
  // Sends one gameplay frame (discard/action) to this connection's room.
  void RouteGameFrame(json::Value frame);
  // The SeatEndpoint view (room/lobby deliveries).
  void SeatFrame(std::string frame);
  void SeatError(std::string frame, QuotaKind quota);
  void Close(std::string reason);

  void HandleLogin(json::Value frame);
  void HandleRegister(json::Value frame);
  void HandleStatsRequest();
  void HandleJoinRoom(json::Value frame);
  void HandleDiscard(json::Value frame);
  void HandleAction(json::Value frame);

  void SendError(std::string_view code, std::string_view message);
  // Sends BAD_REQUEST and accounts the bad-frame quota (§7.2).
  void BadRequest(std::string_view message);
  // Sends an error and accounts the illegal-operation quota (§7.2).
  void IllegalOperation(std::string_view code, std::string_view message);
  PlayerInfo Player() const { return PlayerInfo {player_id_, name_, account_id_}; }

  // True while the connection may still start an account operation: reachable for a guest,
  // refused once this session already has an identity (or one in flight).
  bool CanStartAuth() const { return !logged_in_ && !auth_pending_; }
  // Runs the final half of a successful register/login: claims the account for this
  // connection (or answers ALREADY_ONLINE) and sends login_ok. Returns whether it took it.
  bool AcceptAccount(const AuthOutcome& outcome);
  // Answers INTERNAL for "MySQL is not usable" (ACCOUNT_DESIGN.md §2.3). Deliberately not
  // charged to any quota: the client did nothing wrong.
  void AccountServiceUnavailable();
  // Releases the account held by this connection, if any (§1.3).
  void ReleaseAccount();

  std::shared_ptr<Connection> conn_;
  std::shared_ptr<PlayerIdAllocator> player_ids_;
  std::shared_ptr<AccountStore> accounts_;
  std::shared_ptr<SessionRegistry> sessions_;
  ex_actor::ActorRef<LobbyActor> lobby_;
  ex_actor::ActorRef<ConnectionActor> self_ref_;
  // Set by the lobby once create_room registered the room; empty until then.
  ex_actor::ActorRef<RoomActor> room_;
  bool logged_in_ = false;
  // An account login/register is in flight; the connection has no identity yet.
  bool auth_pending_ = false;
  bool closed_notified_ = false;
  bool session_claimed_ = false;
  uint64_t account_id_ = 0;  // 0 for a guest (ACCOUNT_DESIGN.md §1.2)
  std::string player_id_;
  std::string name_;
};

}  // namespace mahjong
