// The deadline-token encoding, shared by every game.
//
// A `Deadline` (game.h) carries an opaque 64-bit token; the room arms it and hands the same value
// back on `OnTimeout`. What the value *means* is the game's business — but every game so far packs
// it the same way, so that one handler can serve several kinds of deadline without a second map:
//
//     bits 63..61 : kind tag (AI pacing / round advance / match end), 0 = a plain window deadline
//     bits 60..0  : the window seq the deadline belongs to
//
// The seq is the invalidation key (PROTOCOL.md §0.5): envelope seqs are small, so the top bits can
// never appear naturally in one, and a deadline whose seq no longer matches the current window is
// simply stale — the game returns an empty step and nothing happens.
//
// Why a header of its own: this is a *convention*, not part of the contract. The room treats the
// token as opaque (it never unmasks it), so putting these here keeps the shell honestly ignorant
// while two games still agree on the wire-free detail between themselves.

#pragma once

#include <cstdint>

namespace mahjong {

// GAME_DESIGN v1.4 §5.4: a bot's decision is executed ai_delay_ms after its window opened.
constexpr uint64_t kAiPacingTokenBit = 1ull << 63;
inline uint64_t MakeAiPacingToken(uint64_t window_seq) { return window_seq | kAiPacingTokenBit; }
inline bool IsAiPacingToken(uint64_t token) { return (token & kAiPacingTokenBit) != 0; }

// v1.6（C6 局间推进令牌）: the round-to-round advance deadline carries the seq the round ended on,
// tagged with its own bit. It MUST be armed AFTER the round-end transition, so the token holds the
// brand-new seq: the last window's timeout (armed pre-transition, plain tag) fails the first
// criterion, the advance token passes it — the two never collide, and that is only true because of
// the arm-after-transition ordering.
constexpr uint64_t kRoundAdvanceTokenBit = 1ull << 62;
inline uint64_t MakeRoundAdvanceToken(uint64_t window_seq) { return window_seq | kRoundAdvanceTokenBit; }
inline bool IsRoundAdvanceToken(uint64_t token) { return (token & kRoundAdvanceTokenBit) != 0; }

// v1.8（SETTLEMENT_BROADCAST_RULINGS §2.1 / C8）: the **match-end** deadline carries its own bit.
// The round-advance bit cannot be reused: its handler requires `phase == round_end &&
// round_advance_pending` and only starts the next round — a match-end delay armed on that bit
// would silently do nothing (the "静默失效" trap). Same arm-after-transition ordering applies: the
// token must hold the post-round seq so the last window's timeout cannot collide with it.
constexpr uint64_t kMatchEndTokenBit = 1ull << 61;
inline uint64_t MakeMatchEndToken(uint64_t window_seq) { return window_seq | kMatchEndTokenBit; }
inline bool IsMatchEndToken(uint64_t token) { return (token & kMatchEndTokenBit) != 0; }

// The window seq a token belongs to, with every kind tag masked off.
inline uint64_t WindowSeqOfToken(uint64_t token) {
  return token & ~(kAiPacingTokenBit | kRoundAdvanceTokenBit | kMatchEndTokenBit);
}

}  // namespace mahjong
