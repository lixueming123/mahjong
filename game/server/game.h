// Game: the plugin interface a room drives, and the only thing that knows the rules.
//
// The actor is a generic table (seats, endpoints, dispatch, clocks, broadcast); *everything
// else* — state, legal moves, mutations, frames, scoring, bots — lives behind this interface, one
// implementation per game. A room for a new game is `RoomActorFor<ThatGame>`, no change here.
//
// The contract: **the game returns frames, the room delivers them.** A game never touches an
// ActorRef, a connection, a timer or another seat's mailbox — it produces `Outbound{recipients,
// frame}` and the shell sends. That is what keeps a game unit-testable and "one actor writes its
// own state" true.
//
// `Apply` is the *only* mutator; every other hook is a query or documented as read-only, so the
// shell may call it without a gate.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "json.h"
#include "protocol_enums.h"
#include "room_contract.h"

namespace mahjong {

// A seat as the game sees it. Deliberately NOT `RoomSeat`: a game must not be able to message a
// seat, so the endpoint is stripped off at this boundary.
struct GameSeat {
  int seat = 0;
  bool is_bot = false;
  // True when this seat has somewhere to send frames; fixed for the life of a match (installed
  // before the deal, only swapped on reconnect). A per-seat frame for an unwired seat (a bot) is
  // never built.
  bool wired = false;
  std::string player_id;
  std::string name;
};

// What a game needs from its room at setup. `options_json` is whatever the lobby accepted for this
// game (empty for the games that take none); the shell never interprets it.
struct GameContext {
  std::string room_id;
  uint64_t seed = 0;
  int action_timeout_ms = 0;     // how long a human's window stays open (deadline_ms)
  int inter_hand_delay_ms = 0;   // the interlude between rounds
  // Pace at which an automated seat is played. < 0 means "the game's own default".
  int ai_delay_ms = -1;
  // True when the room can honour `GameStep::deadlines`. The room's unit test runs one with no
  // scheduler to prove nothing waits for a message that can never arrive.
  bool deadlines_supported = false;
  std::string options_json;
  std::vector<GameSeat> seats;
};

// One frame and its recipients. An empty `recipients` means "every seat that has an endpoint";
// otherwise the listed seat numbers only.
struct Outbound {
  std::vector<int> recipients;
  std::string frame;
};

// One rendered JSON object per seat, in seat order; empty text means "this seat has no view" (and,
// on a recording's per-step track, "this seat's view did not change").
using SeatViews = std::array<std::string, kSeatCount>;

// The outcome of one settled round, as the game reports it. Deliberately NOT `GameSettlement`:
// that one carries each seat's identity, which the room adds — same split as frames.
struct GameResult {
  protocol::RoundEndReason reason = protocol::RoundEndReason::kInvalid;
  int winner_seat = -1;  // -1 for a drawn round
  std::array<int, kSeatCount> delta {};
  // True when this round ended the match; a settled round that merely continues is false.
  bool match_over = false;
  // ---- 场终快照（match_over 时有效）----
  int hands_played = 0;
  std::array<int, kSeatCount> dealer_hands {};
  std::array<int, kSeatCount> totals {};
  std::array<int, kSeatCount> final_rank {};
};

// One deadline to arm. A step can carry several at once (a claim window arms the human deadline
// and the AI pacing deadline for deferred bots), hence a list.
struct Deadline {
  int64_t delay_ms = 0;
  uint64_t token = 0;
};

// What one transition produced. Deadlines are one-shot: a step arms the deadlines it lists and
// nothing else, so a game cannot accumulate timers by forgetting to clear one.
struct GameStep {
  std::vector<Outbound> outbounds;
  std::vector<Deadline> deadlines;
  // Rounds that settled during this transition, in order. Usually one; zero when the step only
  // moved the table, and two if a bot settles a round and the next one inside one message.
  std::vector<GameResult> results;

  bool empty() const { return outbounds.empty() && deadlines.empty() && results.empty(); }
};

// Small builders. A game writes its frames into the step it is building; nothing else.
inline void Emit(GameStep& step, int seat, std::string frame) {
  if (!frame.empty()) {
    step.outbounds.push_back(Outbound {{seat}, std::move(frame)});
  }
}

// Same frame to every seat that has a wire (the room expands the empty recipient list).
inline void EmitAll(GameStep& step, std::string frame) {
  if (!frame.empty()) {
    step.outbounds.push_back(Outbound {{}, std::move(frame)});
  }
}

// One frame per seat, skipping the seats whose frame is empty (a broadcast with per-seat content).
inline void EmitPerSeat(GameStep& step, std::array<std::string, kSeatCount> frames) {
  for (size_t seat = 0; seat < frames.size(); ++seat) {
    Emit(step, static_cast<int>(seat), std::move(frames[seat]));
  }
}

// Arms one deadline. A delay of 0 means "as soon as the room can" (how the unit tests advance
// rounds without sleeping).
inline void Arm(GameStep& step, int64_t delay_ms, uint64_t token) {
  step.deadlines.push_back(Deadline {delay_ms, token});
}

// What the room should do with an inbound action. Three outcomes, not two: an action for a window
// that already moved on is dropped *silently*, while a well-formed action the rules refuse is
// answered and charged to the quota counter.
struct GameVerdict {
  enum class Kind {
    kAccept,  // Apply may be called with this action
    kRefuse,  // answer |code| / |message| to the seat, change nothing
    kDrop,    // say nothing, change nothing
  };
  Kind kind = Kind::kAccept;
  protocol::ErrorCode code = protocol::ErrorCode::kBadRequest;  // kRefuse only
  std::string message;                                          // kRefuse only

  static GameVerdict Accept() { return {}; }
  static GameVerdict Refuse(protocol::ErrorCode code, std::string message) {
    return {.kind = Kind::kRefuse, .code = code, .message = std::move(message)};
  }
  static GameVerdict Drop() { return {.kind = Kind::kDrop}; }
};

class Game {
 public:
  Game() = default;
  Game(const Game&) = delete;
  Game& operator=(const Game&) = delete;
  virtual ~Game() = default;

  // Stable id: the lobby's `create_room{game}` selector, the DB `mode` column, and the log tag.
  // Must not change once released — match history is keyed by it.
  virtual std::string id() const = 0;

  // Start the first round. Called exactly once, after the shell decided every seat is ready.
  virtual GameStep Setup(const GameContext& context) = 0;

  // True once nothing more will happen: the shell refuses every further action and stops arming
  // deadlines. A settled round waiting for the next one is NOT over.
  virtual bool IsOver() const = 0;

  // Seats that may act right now, in the order the room should try them.
  virtual std::vector<int> ActiveSeats() const = 0;

  // Read-only: the room calls this first and applies nothing unless it says kAccept, so a game
  // never mutates on a rejected input and never builds an error frame itself.
  virtual GameVerdict Validate(int seat, const json::Value& action) const = 0;

  // The one mutator. Only called after Validate returned kAccept, and only while a window is open.
  virtual GameStep Apply(int seat, const json::Value& action) = 0;

  // A deadline fired. `token` is the one this game armed; a game whose state moved on must
  // recognise the stale token and return an empty step — the shell does not filter for it, because
  // only the game knows what the token meant.
  virtual GameStep OnTimeout(uint64_t token) = 0;

  // This seat's own current view, for a `sync` request.
  virtual GameStep Sync(int seat) = 0;

  // The same for a seat whose socket was just replaced. Separate because the client lost everything,
  // so round metadata has to be replayed too — and a reconnect must not grant a fresh deadline for a
  // window already counting down.
  virtual GameStep Reconnect(int seat) = 0;

  // A seat's wire went away or came back. The room already marked its own seat table; this only
  // tells the game which seat has nobody listening, because a request sent there would be lost.
  virtual void OnSeatPresence(int seat, bool present) = 0;

  // ---- replay recording (replay.h) ------------------------------------------------------
  // Each seat's own view of the table, one rendered JSON object per seat, in seat order.
  //
  // Recorded so a replay can offer a view no client ever saw: a seat with no endpoint has its
  // private frames filtered out before they are built, so no recorded wire traffic can
  // reconstruct what that seat was looking at.
  //
  // Opaque by design: what a seat's view *contains* is the game's business, so the shell and the
  // recorder only relay the text. Two rules the text must keep:
  //   * it is read by that game's own client, so it must use the same spelling as its frames;
  //   * it may not reveal more than that seat itself may see.
  //
  // Returns an empty array when the game has no such view (the recording then has no view track).
  virtual SeatViews ReplaySeatViews() const { return {}; }
};

}  // namespace mahjong
