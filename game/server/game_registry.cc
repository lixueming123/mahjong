#include "game_registry.h"

#include <utility>

#include "classic/classic_mahjong_game.h"
// 临时：日麻还没有搬进 Game 插件（S4），它的注册项仍然指向老房间。
#include "room.h"
#include "room_actor_for.h"

namespace mahjong {
namespace {

int& AiDelayStorage() {
  static int value = kDefaultAiDelayMs;
  return value;
}

int& InterHandDelayStorage() {
  static int value = kDefaultInterHandDelayMs;
  return value;
}

// Classic takes no configuration: C15 freezes it at four rounds, so the wire's length selector is
// accepted and ignored rather than rejected (a client that asks for a hanchan still gets the
// classic match, which is what it always got).
std::string ClassicOptions(GameLength /*length*/) { return std::string(); }

stdexec::task<ex_actor::ActorRef<RoomEndpoint>> SpawnClassicRoom(RoomPlan plan) {
  // The concrete room is named exactly once per game, here. Everything after this call site sees
  // `ActorRef<RoomEndpoint>`.
  ex_actor::ActorRef<RoomActorFor<ClassicMahjongGame>> room =
      co_await ex_actor::Spawn<RoomActorFor<ClassicMahjongGame>>(
          std::move(plan.launch.room_id), std::move(plan.game_id), std::move(plan.options_json), plan.launch.seed,
          plan.launch.action_timeout_ms, plan.ai_delay_ms, plan.inter_hand_delay_ms, std::move(plan.launch.seats),
          std::move(plan.launch.timer), std::move(plan.launch.owner), TimeoutScheduler {}, RoomFinishedCallback {});
  co_return room;
}

// S4 之前日麻仍由老房间提供：这一项只是把启动信息翻译成它的构造参数，等
// RiichiMahjongGame 落地就换成 RoomActorFor<RiichiMahjongGame> 并删掉老文件。
std::string LegacyRiichiOptions(GameLength length) { return std::string(); }

stdexec::task<ex_actor::ActorRef<RoomEndpoint>> SpawnLegacyRiichiRoom(RoomPlan plan) {
  ex_actor::ActorRef<LegacyRoomActor> room = co_await ex_actor::Spawn<LegacyRoomActor>(
      std::move(plan.launch.room_id), plan.launch.seed, plan.launch.action_timeout_ms, std::move(plan.launch.seats),
      std::move(plan.launch.timer), plan.ai_delay_ms, plan.inter_hand_delay_ms, std::move(plan.launch.owner),
      TimeoutScheduler {}, RoomFinishedCallback {}, GameMode::kRiichi, plan.length);
  co_return room;
}

// The table. Adding a game is one row here plus a `Game` implementation; nothing else in the
// server names it.
constexpr GameEntry kGames[] = {
    {.mode = GameMode::kClassic, .id = "classic", .options = &ClassicOptions, .spawn = &SpawnClassicRoom},
    {.mode = GameMode::kRiichi, .id = "riichi", .options = &LegacyRiichiOptions, .spawn = &SpawnLegacyRiichiRoom},
};

}  // namespace

void SetAiActionDelayMs(int delay_ms) { AiDelayStorage() = delay_ms; }

int AiActionDelayMs() { return AiDelayStorage(); }

void SetInterHandDelayMs(int delay_ms) { InterHandDelayStorage() = delay_ms; }

int InterHandDelayMs() { return InterHandDelayStorage(); }

const GameEntry* FindGame(GameMode mode) {
  for (const GameEntry& entry : kGames) {
    if (entry.mode == mode) {
      return &entry;
    }
  }
  return nullptr;
}

stdexec::task<ex_actor::ActorRef<RoomEndpoint>> LaunchRoom(const GameEntry& entry, RoomLaunch launch,
                                                           GameLength length) {
  RoomPlan plan;
  plan.game_id = std::string(entry.id);
  plan.options_json = entry.options == nullptr ? std::string() : entry.options(length);
  plan.length = length;
  // The knobs are the process's, the defaults are the game's: -1 means "nothing was configured",
  // so the value stored here (itself defaulted) is what every room runs at.
  plan.ai_delay_ms = AiActionDelayMs();
  plan.inter_hand_delay_ms = InterHandDelayMs();
  plan.launch = std::move(launch);
  co_return co_await entry.spawn(std::move(plan));
}

}  // namespace mahjong
