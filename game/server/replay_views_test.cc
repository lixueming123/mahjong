// Game::ReplaySeatViews() — the per-seat view a recording is built from.
//
// Why not compare against `state_sync.hand`: within one GameStep a bot can make several moves (it
// decides inline when ai_delay_ms is 0), so the track — a per-step snapshot — legitimately differs
// from any single frame inside that step. The authoritative thing to compare against is the game's
// own `hand_of(seat)`, which is what the hook reads in the first place; this test is what proves
// the hook really is that, and not a re-render of something public.
//
// The hook is also deliberately opaque to the framework (see game.h), so these assertions are the
// only place that pins what mahjong puts in a seat's view: the player's own reducer reads exactly
// this shape.

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "classic/classic_mahjong_game.h"
#include "game.h"
#include "json.h"
#include "mahjong_rules.h"
#include "riichi/riichi_mahjong_game.h"

namespace mahjong {
namespace {

// A view as the player reads it: `hand` is the seat's concealed tiles in wire spelling.
json::Value ParseView(const std::string& text) {
  const std::optional<json::Value> parsed = json::Value::Parse(text, nullptr);
  EXPECT_TRUE(parsed.has_value()) << "a seat view must be rendered JSON: " << text;
  return parsed.value_or(json::Value::Obj());
}

std::vector<std::string> HandOfView(const std::string& text) {
  std::vector<std::string> tiles;
  const json::Value view = ParseView(text);
  const json::Value* hand = view.Find("hand");
  if (hand == nullptr || !hand->IsArray()) {
    return tiles;
  }
  for (const json::Value& tile : hand->AsArray()) {
    tiles.push_back(tile.AsString());
  }
  return tiles;
}

std::vector<std::string> JoinTiles(const std::vector<Tile>& tiles, bool red_fives = false) {
  std::vector<std::string> out;
  out.reserve(tiles.size());
  for (const Tile tile : tiles) {
    out.push_back(FormatTile(tile, red_fives));
  }
  return out;
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

TEST(ReplaySeatViewsTest, ClassicReportsEachSeatsConcealedHand) {
  ClassicMahjongGame game;
  SetupGame(game, "r-0000000000001");

  const SeatViews views = game.ReplaySeatViews();
  for (int seat = 0; seat < kSeatCount; ++seat) {
    EXPECT_EQ(HandOfView(views[static_cast<size_t>(seat)]), JoinTiles(game.hand_of(seat))) << "seat " << seat;
  }
  // The deal is 14 tiles for the dealer and 13 for the rest: a view that were public information
  // could not know this.
  EXPECT_EQ(HandOfView(views[static_cast<size_t>(game.dealer())]).size(), 14u);
  const int other_seat = game.dealer() == 0 ? 1 : 0;
  EXPECT_EQ(HandOfView(views[static_cast<size_t>(other_seat)]).size(), 13u);
}

// Riichi is the game where red fives exist, so the same hook must spell them `0m`/`0p`/`0s` —
// exactly the way its own `game_start.hand` does (`TilesToJson(..., red=true)`). A mismatch would
// make the replay's 透视 view show `5m` where the player saw `0m`.
TEST(ReplaySeatViewsTest, RiichiSpellsRedFivesLikeTheWire) {
  RiichiMahjongGame game;
  SetupGame(game, "r-0000000000002", "{\"length\":0}");

  const SeatViews views = game.ReplaySeatViews();
  bool saw_red_five = false;
  for (int seat = 0; seat < kSeatCount; ++seat) {
    const std::string& text = views[static_cast<size_t>(seat)];
    EXPECT_EQ(HandOfView(text), JoinTiles(game.hand_of(seat), /*red_fives=*/true)) << "seat " << seat;
    saw_red_five = saw_red_five || text.find('0') != std::string::npos;
  }
  // Not an assertion on the deal — a given seed may legitimately hold no red five — only a note,
  // because the spelling itself is asserted above either way.
  if (!saw_red_five) {
    GTEST_LOG_(INFO) << "note: this seed dealt no red five, so the 0m/0p/0s spelling was not exercised";
  }
}

// A view is spliced into the file verbatim (replay.cc), so it must be an object and must not carry
// escaped quotes of its own.
TEST(ReplaySeatViewsTest, EverySeatViewIsAnObject) {
  ClassicMahjongGame game;
  SetupGame(game, "r-0000000000003");
  for (const std::string& text : game.ReplaySeatViews()) {
    ASSERT_FALSE(text.empty()) << "mahjong always has a view to record";
    EXPECT_TRUE(ParseView(text).IsObject());
    EXPECT_EQ(text.find("\\\""), std::string::npos) << text;
    EXPECT_NE(text.find("\"melds\":"), std::string::npos) << text;
  }
}

}  // namespace
}  // namespace mahjong
