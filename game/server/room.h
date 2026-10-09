// RoomActor: the authoritative state machine of one room (t4).
//
// Responsibilities:
//   * single source of truth: wall, four hands, discards, melds, current seat and seq;
//   * turn driving: deal -> draw -> discard -> claim window -> meld or next player;
//   * action windows and the frozen "seq invalidation" timeout scheme (PROTOCOL.md §0.5/§6);
//   * three server-side AI seats (synchronous decisions, never awaiting another actor);
//   * settlement through the pure rules library (game/docs/RULESET.md, H8).
//
// Threading model: every message arrives through `Dispatch`, which holds the actor's gate for
// the whole handler (async_gate.h), so exactly one transition is in flight at a time and the
// state machine needs no locks of its own. That is what the framework does not give on its own:
// it starts the next message as soon as a handler suspends, and the room is expected to start
// suspending (see the Deliver note in room.cc), so the gate has to be taken here. The
// introspection getters below are deliberately outside that rule — nothing but the unit tests
// and logging reads room state from outside.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "async_gate.h"
#include "ex_actor/api.h"
#include "json.h"
#include "mahjong_rules.h"
#include "player.h"
#include "seat_endpoint.h"
#include "timer.h"

namespace mahjong {

inline constexpr int kSeatCount = 4;

// Schedules the frozen "window timeout" message: after |delay_ms| the scheduler must call
// RoomActor::OnWindowTimeout(|window_seq|). The actor world injects the real implementation
// (WindowTimer + the room's own ActorRef); tests inject a capturing one.
using TimeoutScheduler = std::function<void(int64_t delay_ms, uint64_t window_seq)>;

// One seat's outcome of a finished game. It carries the whole identity (not just the seat) so
// the owner can record the result without asking the room anything (ACCOUNT_DESIGN.md §4).
struct SettledSeat {
  PlayerInfo player;  // account_id == 0 for a guest or a bot
  bool is_bot = false;
  bool winner = false;
  int delta_score = 0;
};

// Everything the owner of a finished room needs: the §1.4.5 reclaim (room_id) and the match
// records (ACCOUNT_DESIGN.md §2.1, §4), which is exactly what game_end already carries.
struct GameSettlement {
  std::string room_id;
  std::string reason;              // win_ron / win_tsumo / draw_exhausted
  std::vector<SettledSeat> seats;  // always kSeatCount entries, in seat order
};

// Called exactly once, when the room's game reaches game_end, so its owner (LobbyActor) can
// start the post-game reclaim of PROTOCOL §1.4.5 and store the match. Installed by the lobby
// right after Spawn; a room created without one (unit tests) simply never notifies. The
// callback must not re-enter this actor (the lobby's implementation only arms a delayed
// message and hands the result to the storage thread).
using RoomFinishedCallback = std::function<void(const GameSettlement& settlement)>;

// Derives one room's wall seed from the server-level seed and the room's 0-based creation
// index (RoomActor's caller, LobbyActor, passes exactly this).
//
// Why this shape:
//   * it is a pure function of (base_seed, room_index) — no clock, no RNG, no
//     std::hash<std::string> (whose value is not guaranteed stable across runs/builds), so a
//     fixed --seed replays the very same sequence of walls and the e2e statistics stay
//     reproducible;
//   * it is the identity for room_index 0: a single-room server (every test and every
//     `e2e_full_game.py --games N` run) deals exactly the wall it dealt before this change;
//   * odd multiplication + SplitMix64's finalizer are both bijections on 64 bits, so
//     distinct room indices cannot collide, and XOR-ing a fixed base seed keeps two
//     different base seeds apart for the same room index.
inline uint64_t DeriveRoomSeed(uint64_t base_seed, uint32_t room_index) {
  uint64_t mixed = static_cast<uint64_t>(room_index) * 0x9E3779B97F4A7C15ull;  // odd => bijective
  mixed ^= mixed >> 30;
  mixed *= 0xBF58476D1CE4E5B9ull;
  mixed ^= mixed >> 27;
  mixed *= 0x94D049BB133111EBull;
  mixed ^= mixed >> 31;  // finalizer(0) == 0, hence `base_seed` is returned unchanged for room 0
  return base_seed ^ mixed;
}

// GAME_DESIGN v1.4 §5.4: a bot's own decision (discard / meld / self-draw win) is executed
// ai_delay_ms after its window opened, so a human can follow the table. 0 disables the
// pacing and restores the fully synchronous decision path.
constexpr int kDefaultAiDelayMs = 900;
constexpr int kMaxAiDelayMs = 5000;

// Encoding of a scheduled-message token. A window deadline carries the plain envelope seq; an
// AI pacing deadline carries the same seq with kAiPacingTokenBit set, so one handler can serve
// both while the seq stays the invalidation key (PROTOCOL.md §0.5). Envelope seqs are small,
// so the top bit can never appear naturally.
constexpr uint64_t kAiPacingTokenBit = 1ull << 63;
inline uint64_t MakeAiPacingToken(uint64_t window_seq) { return window_seq | kAiPacingTokenBit; }
inline bool IsAiPacingToken(uint64_t token) { return (token & kAiPacingTokenBit) != 0; }
inline uint64_t WindowSeqOfToken(uint64_t token) { return token & ~kAiPacingTokenBit; }

// Process-wide pacing value, set once by main() from --ai-delay-ms. RoomActor reads it when
// the value is not passed explicitly (LobbyActor spawns rooms without carrying it, and
// lobby.* is outside this change); tests pass the delay to the constructor instead.
void SetAiActionDelayMs(int delay_ms);
int AiActionDelayMs();

struct RoomSeat {
  int seat = 0;
  PlayerInfo player;
  bool is_bot = false;
  bool disconnected = false;  // entrusted: the room plays the default action for it
  // Where this seat's frames go (seat_endpoint.h). Empty for a bot, or for a seat with no wire
  // (the deal-only paths in the unit test) — that is the old `sink == nullptr` sentinel.
  ex_actor::ActorRef<SeatEndpoint> endpoint;
};

// One message into RoomActor. `Dispatch` is the room's only entry point, so every caller —
// LobbyActor's sends, the timeout scheduler, the game-end notification and the unit tests —
// builds one of these instead of naming a handler.
struct RoomEnvelope {
  enum class Kind {
    kReady,
    kSync,
    kGameFrame,
    kWindowTimeout,
    kPlayerDisconnected,
    kIdleTimeout,
    kInstallTimeoutScheduler,
    kInstallFinishedCallback,
    kTakeSeat,
  };

  static RoomEnvelope Ready(PlayerInfo player) { return {.kind = Kind::kReady, .player = std::move(player)}; }
  static RoomEnvelope Sync(PlayerInfo player) { return {.kind = Kind::kSync, .player = std::move(player)}; }
  static RoomEnvelope GameFrame(PlayerInfo player, json::Value frame) {
    return {.kind = Kind::kGameFrame, .player = std::move(player), .frame = std::move(frame)};
  }
  // The frozen timeout scheme: |token| is a window seq, or the same seq with the AI pacing bit
  // set (PROTOCOL.md §0.5).
  static RoomEnvelope WindowTimeout(uint64_t token) { return {.kind = Kind::kWindowTimeout, .token = token}; }
  static RoomEnvelope PlayerDisconnected(PlayerInfo player) {
    return {.kind = Kind::kPlayerDisconnected, .player = std::move(player)};
  }
  static RoomEnvelope IdleTimeout(PlayerInfo player) {
    return {.kind = Kind::kIdleTimeout, .player = std::move(player)};
  }
  static RoomEnvelope InstallTimeoutScheduler(TimeoutScheduler scheduler) {
    return {.kind = Kind::kInstallTimeoutScheduler, .scheduler = std::move(scheduler)};
  }
  static RoomEnvelope InstallFinishedCallback(RoomFinishedCallback callback) {
    return {.kind = Kind::kInstallFinishedCallback, .finished = std::move(callback)};
  }
  // Replace one seat before the deal. |accepted| is set on the room's thread.
  static RoomEnvelope TakeSeat(RoomSeat seat, std::shared_ptr<bool> accepted) {
    RoomEnvelope envelope;
    envelope.kind = Kind::kTakeSeat;
    envelope.seat_slot = std::move(seat);
    envelope.accepted = std::move(accepted);
    return envelope;
  }

  Kind kind = Kind::kReady;
  PlayerInfo player;              // kReady / kSync / kGameFrame / kPlayerDisconnected / kIdleTimeout
  json::Value frame;              // kGameFrame
  uint64_t token = 0;             // kWindowTimeout
  TimeoutScheduler scheduler;     // kInstallTimeoutScheduler
  RoomFinishedCallback finished;  // kInstallFinishedCallback
  RoomSeat seat_slot;             // kTakeSeat
  std::shared_ptr<bool> accepted;  // kTakeSeat
};

class RoomActor {
 public:
  // |ai_delay_ms| < 0 means "use the process-wide value" (AiActionDelayMs()).
  RoomActor(std::string room_id, uint64_t seed, int action_timeout_ms, std::vector<RoomSeat> seats,
            std::shared_ptr<WindowTimer> timer, int ai_delay_ms = -1);

  // The room's only entry point, and the only place the gate is taken. Every handler below is
  // private, so no caller can reach room state without passing the gate (async_gate.h explains
  // why that matters). It is a coroutine, so a non-actor caller — the unit test drives the room
  // directly — has to start it (stdexec::sync_wait).
  exec::task<void> Dispatch(RoomEnvelope envelope);

  // Pure claim policy of an automated seat (exposed so it can be unit-tested directly with
  // crafted hands; RoomActor::OpenClaimWindow calls exactly this function for bots, and
  // OnPlayerDisconnected / OnIdleTimeout seats are driven by the same path).
  //   win                            -> always taken
  //   pong / every chow combination  -> taken only when the shanten after the meld (and its
  //                                     mandatory discard) does not get worse than the
  //                                     current shanten ("向听不劣化")
  //   several chow combinations      -> the one with the lowest post-meld shanten; ties keep
  //                                     the order the caller provided (frozen ClaimOptions
  //                                     order), and a pong wins a tie against a chow
  //   nothing acceptable             -> pass
  // An automated seat never claims a kong.
  static Option ChooseClaimAnswer(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                  const std::vector<Option>& options, Tile claimed);

  // Introspection (logs and tests).
  const std::string& room_id() const { return room_id_; }
  bool game_started() const { return game_started_; }
  bool game_finished() const { return phase_ == Phase::kFinished; }
  uint64_t seq() const { return seq_; }
  int remaining() const { return remaining_; }
  int current_seat() const { return current_seat_; }
  int dealer() const { return dealer_; }
  int next_wall_index() const { return next_index_; }
  // "" when no window is open, otherwise "discard" or "claim" (PROTOCOL.md §5.7).
  std::string CurrentWindow() const {
    if (discard_window_open_) return "discard";
    if (claim_open_) return "claim";
    return "";
  }
  const std::vector<Tile>& hand_of(int seat) const { return hands_[static_cast<size_t>(seat)]; }
  const std::vector<Meld>& melds_of(int seat) const { return melds_[static_cast<size_t>(seat)]; }
  const std::vector<Tile>& discards_of(int seat) const { return discards_[static_cast<size_t>(seat)]; }
  int SeatOf(const std::string& player_id) const;

 private:
  // Reached only through Dispatch, i.e. only while gate_ is held.
  // LobbyActor installs the timeout scheduler right after Spawn (PROTOCOL.md §0.5).
  void SetTimeoutScheduler(TimeoutScheduler scheduler);
  // LobbyActor installs the game-end notification right after Spawn (PROTOCOL.md §1.4.5).
  void SetFinishedCallback(RoomFinishedCallback callback);
  // Before the deal: take a bot seat, or put a bot back. No-op once the game has started.
  void TakeSeat(RoomSeat seat, const std::shared_ptr<bool>& accepted);

  // Client intents, already shape-checked by ConnectionActor and room-checked by LobbyActor.
  exec::task<void> Ready(PlayerInfo player);
  exec::task<void> Sync(PlayerInfo player);
  exec::task<void> HandleGameFrame(PlayerInfo player, json::Value frame);

  // Frozen timeout scheme: the message carries the window seq it was scheduled for.
  exec::task<void> OnWindowTimeout(uint64_t window_seq);
  exec::task<void> OnPlayerDisconnected(PlayerInfo player);
  exec::task<void> OnIdleTimeout(PlayerInfo player);

  enum class Phase {
    kIdle,           // created, no game yet
    kTurnStart,      // the current seat must draw (or the wall is exhausted -> drawn game)
    kDiscardWindow,  // a discard window is open for current_seat_
    kClaimWindow,    // a claim window is open for the seats holding claim options
    kFinished,       // game_end has been broadcast
  };

  struct ClaimAnswer {
    bool answered = false;
    std::string action = "pass";
    std::vector<Tile> tiles;
  };

  // Everything game_end needs (PROTOCOL.md §5.10).
  struct Settlement {
    std::string reason;
    std::array<int, kSeatCount> delta {};
    int winner_seat = -1;  // -1: drawn game
    std::vector<Tile> winner_flat_hand;
    std::vector<std::string> winner_types;  // tags + "ron"/"tsumo"
    bool tsumo = false;
    Tile winning_tile {};
    int from_seat = -1;
    std::vector<std::string> tags;
    int multiplier = 0;
    int per_payer = 0;
    int total = 0;
  };

  // ---- turn driving ----------------------------------------------------------------
  exec::task<void> StartGame();
  exec::task<void> Pump();
  exec::task<void> DrawForCurrentSeat();
  exec::task<void> OpenDiscardWindow(bool win_check, std::optional<Tile> drawn_tile);
  exec::task<void> ApplyDiscard(int seat, Tile tile);
  exec::task<void> OpenClaimWindow(int from_seat, Tile tile, const std::array<bool, kSeatCount>& recipients,
                                   const std::array<std::vector<Option>, kSeatCount>& options);
  exec::task<void> ResolveClaimWindow();
  exec::task<void> ApplyMeld(int seat, const std::string& action, const std::vector<Tile>& client_tiles, int from_seat,
                             Tile claimed_tile);
  exec::task<void> EndWithWin(int winner_seat, int from_seat, Tile winning_tile, bool tsumo);
  exec::task<void> EndWithDraw();

  // ---- client frame handlers --------------------------------------------------------
  exec::task<void> HandleDiscard(int seat, json::Value frame);
  exec::task<void> HandleAction(int seat, json::Value frame);

  // ---- helpers ----------------------------------------------------------------------
  uint64_t Transition();
  void ResetClaimWindow();
  // A seat is played by the server when it is a bot, when it is entrusted (disconnected) or
  // when it has no wire to ask (defensive: only seat 0 ever has a sink in this milestone).
  bool IsAutomated(int seat) const;
  bool NeedsPlayerReply(int seat) const {
    return !IsAutomated(seat) && !seats_[static_cast<size_t>(seat)].endpoint.IsEmpty();
  }
  std::optional<Tile> DefaultDiscardFor(int seat) const;

  // --- AI pacing (GAME_DESIGN v1.4 §5.4) ------------------------------------------------
  // Defers one decision to the shared timeout scheduler. Nothing blocks: the actor returns
  // to its mailbox and handles human frames meanwhile; the scheduled message carries the
  // window seq, so a window that moved on makes it stale and it is dropped.
  // Returns false when no scheduler is installed: the caller must then decide inline, because
  // a pending flag without an armed message would wait for a reply that never comes.
  bool ScheduleAiAction(int64_t delay_ms);
  // The automated seat's own-window decision (self-draw win or tile-efficiency discard).
  // Returns false when the game ended and there is nothing left to pump.
  exec::task<bool> PlayAutomatedDiscard();
  exec::task<bool> BotDeclaresTsumo(int seat);
  // Fills in the answers of the automated claim recipients that were deferred.
  void ResolveAutomatedClaimAnswers();
  // A human recipient already declared `win`: no pending bot answer can change that outcome.
  bool HasPendingHumanWin() const;
  // The tile that completes the current seat's hand on its own draw (a tsumo). The dealer's
  // 天和 has no draw record, so the largest tile of the hand stands in for it.
  Tile TsumoWinningTile(int seat) const;
  void OpenWindowTimer();
  exec::task<void> ReplyError(int seat, std::string_view code, std::string_view message);
  // Tells the room's owner the game is over, exactly once (PROTOCOL.md §1.4.5).
  void NotifyFinished(const Settlement& settlement);
  exec::task<void> ReplyFrame(int seat, std::string frame);
  // One seat's share of a broadcast: empty frames and empty endpoints are skipped. Deliver
  // awaits exactly four of these in parallel, so the arity is fixed rather than looped.
  exec::task<void> DeliverSeat(int seat, std::string frame);
  exec::task<void> Deliver(std::array<std::string, kSeatCount> frames);
  std::array<std::string, kSeatCount> StateSyncFrames() const;
  std::string BuildStateSync(int for_seat) const;
  std::string BuildGameStart(int for_seat) const;
  std::string BuildGameEnd(const Settlement& settlement) const;
  exec::task<void> BroadcastSettlement(const Settlement& settlement);

  // ---- state ------------------------------------------------------------------------
  std::string room_id_;
  uint64_t seed_;
  int action_timeout_ms_;
  std::vector<RoomSeat> seats_;
  std::array<std::vector<Tile>, kSeatCount> hands_;
  std::array<std::vector<Meld>, kSeatCount> melds_;
  std::array<std::vector<Tile>, kSeatCount> discards_;
  std::vector<Tile> wall_;
  int next_index_ = 0;
  int remaining_ = 0;
  int dealer_ = 0;
  int current_seat_ = 0;
  uint64_t seq_ = 0;
  Phase phase_ = Phase::kIdle;
  bool game_started_ = false;

  // discard window
  bool discard_window_open_ = false;
  // Whether the open discard window allows a self-draw win at all (false right after a
  // pong/chow, where the hand is not a complete 14-tile form). Set by OpenDiscardWindow.
  bool discard_window_win_check_ = false;
  std::optional<Tile> drawn_tile_;  // draw record of the current seat (timeout default)

  // claim window
  bool claim_open_ = false;
  int claim_from_seat_ = -1;
  Tile claim_tile_ {};
  std::array<bool, kSeatCount> claim_recipient_ {};
  std::array<std::vector<Option>, kSeatCount> claim_options_;
  std::array<ClaimAnswer, kSeatCount> claim_answers_;
  int claim_pending_humans_ = 0;

  TimeoutScheduler timeout_scheduler_;
  RoomFinishedCallback finished_callback_;

  std::shared_ptr<WindowTimer> timer_;
  // AI pacing (GAME_DESIGN v1.4 §5.4). A pending flag means a decision of this window is
  // already scheduled, so Pump() never schedules it twice.
  int ai_delay_ms_ = 0;
  bool ai_discard_pending_ = false;
  bool ai_claim_pending_ = false;
  // Serializes the room's handlers (async_gate.h). Held for the whole of Dispatch, so the state
  // machine keeps its single-transition semantics even once a handler suspends mid-message.
  AsyncGate gate_;
};

}  // namespace mahjong
