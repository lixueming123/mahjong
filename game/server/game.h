// Game: the plugin interface a room drives, and the only thing that knows the rules.
//
// Why this exists. RoomActor used to be one 2700-line class holding the wall, four hands, the
// turn machine, two rule systems and 63 `mode_` branches; every new game meant another branch in
// the same state machine. Now the actor is a generic table (seats, endpoints, dispatch, clocks,
// broadcast) and *everything else* — state, legal moves, mutations, frames, scoring, bots — lives
// behind this interface, one implementation per game. A room for a new game is
// `RoomActorFor<ThatGame>` (room_actor_for.h); it needs no change here.
//
// The contract in one sentence: **the game returns frames, the room delivers them.** A game never
// touches an ActorRef, a connection, a timer or another seat's mailbox — it produces
// `Outbound{recipients, frame}` and the shell is the only thing that sends. That is what keeps a
// game unit-testable (mahjong_room_test.cc drives a Game through the shell with capturing seats)
// and keeps "one actor writes its own state" true.
//
// A `GameStep` is what one transition produced: the frames to send, the deadlines to arm, and
// the rounds that settled. `Apply` is the *only* mutator; every other hook is either a query,
// or is documented as read-only so the shell can call it without a gate.

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
// seat (that is the room's job, and `RoomSeat::endpoint` is how it does it), so the endpoint is
// stripped off at this boundary. It is also the reason a game is testable without the actor
// runtime at all.
struct GameSeat {
  int seat = 0;
  bool is_bot = false;
  // True when this seat has somewhere to send frames. Fixed for the life of a match: a seat's
  // endpoint is installed before the deal and only swapped on reconnect, never cleared. The
  // game needs it because building a per-seat frame is the expensive half of a broadcast and
  // an unwired seat (a bot) never sees one; `Outbound` with empty recipients is filtered by the
  // room, but the per-seat frames of a state_sync have to be skipped before they are built.
  bool wired = false;
  std::string player_id;
  std::string name;
};

// What a game needs from its room at setup. `options_json` is whatever the lobby accepted for
// this game (empty for the games that take none); the shell never interprets it.
struct GameContext {
  std::string room_id;
  uint64_t seed = 0;
  int action_timeout_ms = 0;     // how long a human's window stays open (§5.7 deadline_ms)
  int inter_hand_delay_ms = 0;   // the interlude between rounds (§5.10/§5.15)
  // The pace at which an automated seat is played (§5.4). < 0 means "the game's own default":
  // the room does not know whether this game even has bots.
  int ai_delay_ms = -1;
  // True when the room can honour `GameStep::deadlines`. The room's unit test deliberately
  // runs one with no scheduler to prove nothing waits for a message that can never arrive, and
  // that inline fallback is frozen by tests, so a game has to know which world it is in.
  bool deadlines_supported = false;
  std::string options_json;
  std::vector<GameSeat> seats;
};

// One frame and its recipients. An empty `recipients` means "every seat that has an endpoint";
// otherwise the listed seat numbers only. A game that wants a private view builds a different
// frame per seat and emits one Outbound per seat — that is exactly what state_sync does.
struct Outbound {
  std::vector<int> recipients;
  std::string frame;
};

// The outcome of one settled round, as the game reports it.
//
// Deliberately NOT `GameSettlement`: that one carries each seat's identity, which the room has
// held since the deal and the game never sees (`GameSeat` strips it). The game says *what
// happened* (who won, how much each seat moved, whether the match is over); the room adds *who
// they were* and sends the result to its owner. Same split as frames: the game decides, the room
// delivers.
struct GameResult {
  protocol::RoundEndReason reason = protocol::RoundEndReason::kInvalid;
  int winner_seat = -1;  // -1 for a drawn round
  std::array<int, kSeatCount> delta {};
  // True when this round ended the match (the room reports `match_over` and the owner arms the
  // §1.4.5 reclaim); a settled round that merely continues the match is false.
  bool match_over = false;
  // ---- 场终快照（match_over 时有效）----
  int hands_played = 0;
  std::array<int, kSeatCount> dealer_hands {};
  std::array<int, kSeatCount> totals {};
  std::array<int, kSeatCount> final_rank {};
};

// One deadline to arm. A step can carry several at once — a claim window arms the human
// deadline and, in the same message, the AI pacing deadline for the bots that were deferred —
// which is why this is a list and not a pair of fields.
struct Deadline {
  int64_t delay_ms = 0;
  uint64_t token = 0;
};

// What one transition produced. The deadlines are one-shot: a step arms the deadlines it
// lists and nothing else, so a game cannot accumulate timers by forgetting to clear one.
struct GameStep {
  std::vector<Outbound> outbounds;
  std::vector<Deadline> deadlines;
  // Rounds that settled during this transition, in order. Usually one; zero when the step only
  // moved the table, and (with AI pacing disabled) two if a bot deals itself a winning hand and
  // the next round settles inside the same message.
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

// One frame per seat, skipping the seats whose frame is empty (a broadcast that has per-seat
// content: state_sync, and nothing else today).
inline void EmitPerSeat(GameStep& step, std::array<std::string, kSeatCount> frames) {
  for (size_t seat = 0; seat < frames.size(); ++seat) {
    Emit(step, static_cast<int>(seat), std::move(frames[seat]));
  }
}

// Arms one deadline. A delay of 0 is legal and means "as soon as the room can" — that is how
// the unit tests drive round advances without sleeping, and the old room armed those too.
inline void Arm(GameStep& step, int64_t delay_ms, uint64_t token) {
  step.deadlines.push_back(Deadline {delay_ms, token});
}

// What the room should do with an inbound action. Three outcomes, not two, because PROTOCOL §6.3
// distinguishes them: an action for a window that already moved on is dropped *silently* (the
// client raced a timeout and must not see an error for it), while a well-formed action that the
// rules refuse is answered and charged to the §7.2 counter.
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

  // Start the first round. Called exactly once, after the shell has decided every seat is
  // ready. May return a result (a game that can be over before it starts).
  virtual GameStep Setup(const GameContext& context) = 0;

  // True once nothing more will happen: the shell refuses every further action and stops arming
  // deadlines. A settled round that is still waiting for the next one is NOT over.
  virtual bool IsOver() const = 0;

  // Seats that may act right now, in the order the room should try them (used to decide who a
  // generic "the seat went away" path should automate, and by tests).
  virtual std::vector<int> ActiveSeats() const = 0;

  // Read-only: the room calls this first and applies nothing unless it says kAccept. A game
  // therefore never mutates on a rejected input, and never builds an error frame itself.
  virtual GameVerdict Validate(int seat, const json::Value& action) const = 0;

  // The one mutator. Only called after Validate returned kAccept, and only while a window is
  // open — the shell never calls it for a stale or unsolicited action.
  virtual GameStep Apply(int seat, const json::Value& action) = 0;

  // A deadline fired. `token` is the one this game armed; a game whose state moved on must
  // recognise the stale token and return an empty step (PROTOCOL.md §0.5 seq invalidation —
  // the shell does not filter for it, because only the game knows what the token meant).
  virtual GameStep OnTimeout(uint64_t token) = 0;

  // This seat's own current view, for a `sync` request. Goes to that seat only. Cheap: the
  // client just wants the authoritative board back.
  virtual GameStep Sync(int seat) = 0;

  // The same, for a seat whose socket was just replaced (§8.5). Separate from Sync because the
  // client in this case has lost everything, so the round metadata (game_start) has to be
  // replayed too — and a reconnect must not grant a fresh deadline for a window that is already
  // counting down. Only the game knows which frames that case needs.
  virtual GameStep Reconnect(int seat) = 0;

  // A seat's wire went away or came back. The room has already marked its own seat table; this
  // only tells the game which seat currently has nobody listening, because a request sent there
  // would be lost and is replayed on reconnect instead (§8.5). A disconnect never plays for the
  // seat: the open window keeps its countdown, and that is a rule, not an omission.
  virtual void OnSeatPresence(int seat, bool present) = 0;
};

}  // namespace mahjong
