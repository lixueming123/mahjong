// Game::ReplayHands() — the god view a recording is built from.
//
// Why not compare against `state_sync.hand`: within one GameStep a bot can make several moves (it
// decides inline when ai_delay_ms is 0), so the hand track — a per-step snapshot — legitimately
// differs from any single frame inside that step. The authoritative thing to compare against is the
// game's own `hand_of(seat)`, which is what the hook reads in the first place; this test is what
// proves the hook really is that, and not a re-render of something public.

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "classic/classic_mahjong_game.h"
#include "game.h"
#include "mahjong_rules.h"
#include "riichi/riichi_mahjong_game.h"

namespace mahjong {
namespace {

std::string JoinTiles(const std::vector<Tile>& tiles, bool red_fives = false) {
  std::string out;
  for (const Tile tile : tiles) {
    if (!out.empty()) {
      out.push_back(',');
    }
    out += FormatTile(tile, red_fives);
  }
  return out;
}

size_t TileCount(const std::string& hand) {
  return hand.empty() ? 0 : static_cast<size_t>(std::count(hand.begin(), hand.end(), ',')) + 1;
}

// Both games are wired the same way: four seats, no bots, no pacing.
template <class GameT>
void SetupGame(GameT& game, const std::string& room_id, const std::string& options_json = {}) {
  GameContext context;
  context.room_id = room_id;
  context.seed = 0x5DEECE66Dull;
  context.action_timeout_ms = 15000;
  context.inter_hand_delay_ms = 0;
  context.ai_delay_ms = 0;
  context.deadlines_supported = true;
  context.options_json = options_json;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    context.seats.push_back(GameSeat {.seat = seat,
                                      .is_bot = false,
                                      .wired = true,
                                      .player_id = "p-" + std::to_string(seat),
                                      .name = "P" + std::to_string(seat)});
  }
  static_cast<void>(game.Setup(context));
}

TEST(ReplayHandsTest, ClassicReportsEachSeatsConcealedHand) {
  ClassicMahjongGame game;
  SetupGame(game, "r-0000000000001");

  const std::array<std::string, kSeatCount> hands = game.ReplayHands();
  for (int seat = 0; seat < kSeatCount; ++seat) {
    EXPECT_EQ(hands[static_cast<size_t>(seat)], JoinTiles(game.hand_of(seat))) << "seat " << seat;
  }
  // The deal is 14 tiles for the dealer and 13 for the rest: a track that were public information
  // could not know this.
  EXPECT_EQ(TileCount(hands[static_cast<size_t>(game.dealer())]), 14u);
  const int other_seat = game.dealer() == 0 ? 1 : 0;
  EXPECT_EQ(TileCount(hands[static_cast<size_t>(other_seat)]), 13u);
}

// Riichi is the game where red fives exist, so the same hook must spell them `0m`/`0p`/`0s` —
// exactly the way its own `game_start.hand` does (`TilesToJson(..., red=true)`). A mismatch would
// make the replay's 透视 view show `5m` where the player saw `0m`.
TEST(ReplayHandsTest, RiichiSpellsRedFivesLikeTheWire) {
  RiichiMahjongGame game;
  SetupGame(game, "r-0000000000002", "{\"length\":0}");

  const std::array<std::string, kSeatCount> hands = game.ReplayHands();
  bool saw_red_five = false;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    const std::string expected = JoinTiles(game.hand_of(seat), /*red_fives=*/true);
    EXPECT_EQ(hands[static_cast<size_t>(seat)], expected) << "seat " << seat;
    saw_red_five = saw_red_five || hands[static_cast<size_t>(seat)].find('0') != std::string::npos;
  }
  // Not an assertion on the deal — a given seed may legitimately hold no red five — only a note,
  // because the spelling itself is asserted above either way.
  if (!saw_red_five) {
    GTEST_LOG_(INFO) << "note: this seed dealt no red five, so the 0m/0p/0s spelling was not exercised";
  }
}

}  // namespace
}  // namespace mahjong
