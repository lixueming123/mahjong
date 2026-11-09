// Unit tests for LobbyActor's post-game reclaim (PROTOCOL.md §1.4.5).
//
// The test runs the real stack — a spawned LobbyActor, a real WindowTimer and a real
// RoomActor — with the window length and the reclaim delay shrunk, so a whole game auto-plays
// through the frozen timeout path (every human window times out at once) and the reclaim is
// observable in well under a second. No socket is involved: the connection is a real
// Connection over an invalid fd, which is enough to carry the outbox and the close request.

#include "lobby.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

#include <exec/async_scope.hpp>
#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "actor_scope.h"
#include "connection.h"
#include "ex_actor/api.h"
#include "json.h"
#include "player.h"
#include "player_actor.h"
#include "room.h"
#include "timer.h"

namespace mahjong {
namespace {

constexpr int kFastWindowMs = 1;    // every human window expires immediately (§6.4 default)
constexpr int kFastReclaimMs = 40;  // stands in for the frozen 30s of §1.4.5

PlayerInfo MakePlayer() {
  PlayerInfo player;
  player.player_id = "p-test";
  player.name = "Tester";
  return player;
}

// Blocks until the lobby answered, returning the handler's value.
template <class Sender>
auto WaitValue(Sender&& sender) {
  return std::get<0>(*stdexec::sync_wait(std::forward<Sender>(sender)));
}

size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
  size_t count = 0;
  for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

// The lobby answers create_room with room_joined and keeps no public accessor for the id, so a
// test reads the name it handed out from the wire. Room ids are random (lobby.cc: FormatRoomId),
// which is exactly what the reconnect/restart tests need to observe.
std::string JoinedRoomId(const std::shared_ptr<Connection>& conn) {
  std::string outbox;
  conn->DrainOutbox(&outbox);
  const std::string first_frame = outbox.substr(0, outbox.find('\n'));
  std::string error;
  std::optional<json::Value> frame = json::Value::Parse(first_frame, &error);
  const json::Value* room_id = frame.has_value() ? frame->Find("room_id") : nullptr;
  if (room_id == nullptr || !room_id->IsString()) {
    ADD_FAILURE() << "room_joined missing from the outbox: " << outbox << " (" << error << ")";
    return std::string();
  }
  return room_id->AsString();
}

class LobbyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Bots must decide inline: this test never sleeps for AI pacing.
    SetAiActionDelayMs(0);
    // v1.6（C7）：局间停顿同样压到 1ms，一场 4..8 局在亚秒内跑完；回收时序单独控制。
    SetInterHandDelayMs(1);
    ex_actor::Init(/*thread_pool_size=*/2);
    timer_ = std::make_shared<WindowTimer>();
  }

  void TearDown() override {
    // Same teardown order as main(): the timer first (pending actions post into actors), then
    // the actor world.
    timer_->Stop();
    stdexec::sync_wait(ActorSendScope().on_empty());
    ex_actor::Shutdown();
  }

  // Waits until |predicate| holds, polling through the lobby mailbox. Returns false on timeout.
  template <class Predicate>
  bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
  }

  // The lobby only ever talks to a seat endpoint, so every test connection gets a real
  // PlayerActor wired to it — exactly what the gateway does, minus the socket. The
  // connector's own actor ref is what the lobby answers on.
  ex_actor::ActorRef<SeatEndpoint> SpawnConnector(const std::shared_ptr<Connection>& conn,
                                                  ex_actor::ActorRef<LobbyActor> lobby) {
    ex_actor::ActorRef<PlayerActor> connector =
        WaitValue(ex_actor::Spawn<PlayerActor>(conn, player_ids_, /*db=*/DbService {}, sessions_, lobby));
    stdexec::sync_wait(connector.Send<&PlayerActor::Dispatch>(PlayerEnvelope::SetSelfRef(connector)));
    return connector;
  }

  std::shared_ptr<WindowTimer> timer_;
  std::shared_ptr<PlayerIdAllocator> player_ids_ = std::make_shared<PlayerIdAllocator>();
  std::shared_ptr<SessionRegistry> sessions_ = std::make_shared<SessionRegistry>();
};

// §1.4.5 (v1.6): the room survives every game_end of one match — the reclaim is armed only by
// the match_over settlement (C10) — and after match_end + the reclaim delay the room and its
// connection are gone, even though the client never disconnected.
TEST_F(LobbyTest, ReclaimsARoomAndItsConnectionAfterMatchEnd) {
  ex_actor::ActorRef<LobbyActor> lobby =
      WaitValue(ex_actor::Spawn<LobbyActor>(/*seed=*/20251005, kFastWindowMs, timer_, kFastReclaimMs));
  stdexec::sync_wait(lobby.Send<&LobbyActor::SetSelfRef>(lobby));

  // An invalid fd: the outbox and the close request still work, and no socket is touched.
  auto conn = std::make_shared<Connection>(/*fd=*/-1, /*id=*/1, /*wake_write_fd=*/-1);
  ASSERT_FALSE(conn->close_requested());

  stdexec::sync_wait(lobby.Send<&LobbyActor::CreateRoom>(SpawnConnector(conn, lobby), MakePlayer()));
  ASSERT_EQ(WaitValue(lobby.Send<&LobbyActor::room_count>()), 1u);

  // Ready starts the match; with a 1ms window every human window times out, so the whole match
  // (4..8 rounds) plays itself through the real scheduler.
  stdexec::sync_wait(lobby.Send<&LobbyActor::Ready>(SpawnConnector(conn, lobby), MakePlayer()));

  const auto room_count = [&lobby]() { return WaitValue(lobby.Send<&LobbyActor::room_count>()); };
  // C10 专项：局终（game_end）不回收 —— 房间必须跨局存活足够久（这里等前 3 局的时间量级，
  // 房间仍在注册表里）。
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(room_count(), 1u) << "game_end must not reclaim the room (v1.6)";
  // 场终 + 回收延迟之后归零。
  ASSERT_TRUE(WaitFor([&room_count]() { return room_count() == 0; })) << "the finished room was never reclaimed";
  EXPECT_TRUE(conn->close_requested()) << "the reclaim must close the room's connection";
}

// PROTOCOL v2.2 §5.1: the lobby answers an account login and, when the account is still
// seated in a started room, rebinds the new connection to that seat instead of creating a room.
TEST_F(LobbyTest, LoggedInAccountRebindsItsSeatedRoom) {
  ex_actor::ActorRef<LobbyActor> lobby =
      WaitValue(ex_actor::Spawn<LobbyActor>(/*seed=*/20251005, kFastWindowMs, timer_, kFastReclaimMs));
  stdexec::sync_wait(lobby.Send<&LobbyActor::SetSelfRef>(lobby));

  const PlayerInfo player = MakePlayer();
  auto first_conn = std::make_shared<Connection>(/*fd=*/-1, /*id=*/1, /*wake_write_fd=*/-1);
  const ex_actor::ActorRef<SeatEndpoint> first = SpawnConnector(first_conn, lobby);
  stdexec::sync_wait(lobby.Send<&LobbyActor::CreateRoom>(first, player));
  const std::string room_id = JoinedRoomId(first_conn);
  ASSERT_FALSE(room_id.empty());
  stdexec::sync_wait(lobby.Send<&LobbyActor::Ready>(first, player));

  // The refresh: the old socket goes away, the seat is entrusted, then a fresh connection logs
  // in with the same account and must take the seat back.
  stdexec::sync_wait(lobby.Send<&LobbyActor::OnConnectionClosed>(player, first));
  auto second_conn = std::make_shared<Connection>(/*fd=*/-1, /*id=*/2, /*wake_write_fd=*/-1);
  const ex_actor::ActorRef<SeatEndpoint> second = SpawnConnector(second_conn, lobby);
  stdexec::sync_wait(lobby.Send<&LobbyActor::OnLoggedIn>(second, player, /*resume=*/true));

  std::string outbox;
  second_conn->DrainOutbox(&outbox);
  EXPECT_NE(outbox.find("\"in_room\":true"), std::string::npos) << outbox;
  EXPECT_NE(outbox.find("\"room_id\":\"" + room_id + "\""), std::string::npos) << outbox;
}

// The reclaim is idempotent and must not touch anything else: a second delivery (a late timer,
// or a room that was already dropped with its owner) is a no-op.
TEST_F(LobbyTest, ReapRoomIsIdempotentAndLeavesOtherRoomsAlone) {
  ex_actor::ActorRef<LobbyActor> lobby =
      WaitValue(ex_actor::Spawn<LobbyActor>(/*seed=*/7, kFastWindowMs, timer_, kFastReclaimMs));
  stdexec::sync_wait(lobby.Send<&LobbyActor::SetSelfRef>(lobby));

  auto first = std::make_shared<Connection>(/*fd=*/-1, /*id=*/1, /*wake_write_fd=*/-1);
  auto second = std::make_shared<Connection>(/*fd=*/-1, /*id=*/2, /*wake_write_fd=*/-1);
  PlayerInfo alice;
  alice.player_id = "p-alice";
  alice.name = "Alice";
  PlayerInfo bob;
  bob.player_id = "p-bob";
  bob.name = "Bob";

  stdexec::sync_wait(lobby.Send<&LobbyActor::CreateRoom>(SpawnConnector(first, lobby), alice));
  stdexec::sync_wait(lobby.Send<&LobbyActor::CreateRoom>(SpawnConnector(second, lobby), bob));
  ASSERT_EQ(WaitValue(lobby.Send<&LobbyActor::room_count>()), 2u);

  const std::string first_room = JoinedRoomId(first);
  ASSERT_FALSE(first_room.empty());

  stdexec::sync_wait(lobby.Send<&LobbyActor::ReapRoom>(first_room));
  EXPECT_EQ(WaitValue(lobby.Send<&LobbyActor::room_count>()), 1u);
  EXPECT_TRUE(first->close_requested());
  EXPECT_FALSE(second->close_requested()) << "reaping one room must not touch another";

  stdexec::sync_wait(lobby.Send<&LobbyActor::ReapRoom>(first_room));  // idempotent
  EXPECT_EQ(WaitValue(lobby.Send<&LobbyActor::room_count>()), 1u);
  EXPECT_FALSE(second->close_requested());
}

// ACCOUNT_DESIGN.md §4 keys `match_results` by (room_id, account_id), so a room id that a
// restart (or another server sharing the database) handed out a second time would make a fresh
// match look like a duplicate write and be dropped. The lobby therefore packs a random
// per-process tag into the id; two lobbies stand in for two server runs, and a third room in
// the first lobby proves the per-lobby sequence also advances.
TEST_F(LobbyTest, RoomIdsAreUniqueAcrossLobbiesAndRooms) {
  ex_actor::ActorRef<LobbyActor> first_lobby =
      WaitValue(ex_actor::Spawn<LobbyActor>(/*seed=*/11, kFastWindowMs, timer_, kFastReclaimMs));
  stdexec::sync_wait(first_lobby.Send<&LobbyActor::SetSelfRef>(first_lobby));
  ex_actor::ActorRef<LobbyActor> second_lobby =
      WaitValue(ex_actor::Spawn<LobbyActor>(/*seed=*/12, kFastWindowMs, timer_, kFastReclaimMs));
  stdexec::sync_wait(second_lobby.Send<&LobbyActor::SetSelfRef>(second_lobby));

  auto make_player = [](std::string player_id) {
    PlayerInfo player;
    player.player_id = std::move(player_id);
    player.name = "n";
    return player;
  };
  auto first_conn = std::make_shared<Connection>(/*fd=*/-1, /*id=*/1, /*wake_write_fd=*/-1);
  auto second_conn = std::make_shared<Connection>(/*fd=*/-1, /*id=*/2, /*wake_write_fd=*/-1);
  auto third_conn = std::make_shared<Connection>(/*fd=*/-1, /*id=*/3, /*wake_write_fd=*/-1);

  stdexec::sync_wait(
      first_lobby.Send<&LobbyActor::CreateRoom>(SpawnConnector(first_conn, first_lobby), make_player("p-alice")));
  stdexec::sync_wait(
      second_lobby.Send<&LobbyActor::CreateRoom>(SpawnConnector(second_conn, second_lobby), make_player("p-bob")));
  stdexec::sync_wait(
      first_lobby.Send<&LobbyActor::CreateRoom>(SpawnConnector(third_conn, first_lobby), make_player("p-carol")));

  const std::string first_id = JoinedRoomId(first_conn);
  const std::string second_id = JoinedRoomId(second_conn);
  const std::string third_id = JoinedRoomId(third_conn);
  ASSERT_FALSE(first_id.empty());
  ASSERT_FALSE(second_id.empty());
  ASSERT_FALSE(third_id.empty());
  EXPECT_NE(first_id, second_id) << "two server runs must not hand out the same room id";
  EXPECT_NE(first_id, third_id) << "the per-lobby sequence must advance";

  // PROTOCOL §4.2's frozen shape: "r-" + 13 lowercase base-36 digits (15 chars, inside the
  // VARCHAR(16) of both match tables).
  for (const std::string& id : {first_id, second_id, third_id}) {
    ASSERT_EQ(id.size(), 15u) << id;
    ASSERT_EQ(id.rfind("r-", 0), 0u) << id;
    for (size_t i = 2; i < id.size(); ++i) {
      const char c = id[i];
      EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) << id;
    }
  }
}

// PROTOCOL §4.2 / §1.4.1: create_room claims the player before Spawn returns. The claim is the
// `creating_` reservation, so two create_rooms written back to back (which is what a client
// that does not wait for room_joined does) cannot both succeed.
TEST_F(LobbyTest, PipelinedCreateRoomKeepsOneRoomPerPlayer) {
  ex_actor::ActorRef<LobbyActor> lobby =
      WaitValue(ex_actor::Spawn<LobbyActor>(/*seed=*/99, kFastWindowMs, timer_, kFastReclaimMs));
  stdexec::sync_wait(lobby.Send<&LobbyActor::SetSelfRef>(lobby));
  auto conn = std::make_shared<Connection>(/*fd=*/-1, /*id=*/1, /*wake_write_fd=*/-1);

  // Neither send is awaited, so both reach the lobby before the first room finished spawning.
  ActorSendScope().spawn(lobby.Send<&LobbyActor::CreateRoom>(SpawnConnector(conn, lobby), MakePlayer()));
  ActorSendScope().spawn(lobby.Send<&LobbyActor::CreateRoom>(SpawnConnector(conn, lobby), MakePlayer()));

  const auto room_count = [&lobby]() { return WaitValue(lobby.Send<&LobbyActor::room_count>()); };
  ASSERT_TRUE(WaitFor([&room_count]() { return room_count() == 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let a second (wrong) room appear
  EXPECT_EQ(room_count(), 1u) << "one player must own exactly one room";

  std::string outbox;
  conn->DrainOutbox(&outbox);
  // The wire is integer-typed (PROTOCOL v2.0 §1.3.1), so counting needle strings pins the frame
  // kinds by their frozen wire values: one room_joined, one BAD_REQUEST-coded error frame.
  EXPECT_EQ(CountOccurrences(outbox, "\"type\":" + std::to_string(protocol::WireOf(protocol::FrameType::kRoomJoined))),
            1)
      << outbox;
  EXPECT_EQ(CountOccurrences(outbox, "\"code\":" + std::to_string(protocol::WireOf(protocol::ErrorCode::kBadRequest))),
            1)
      << "the second create_room must be rejected: " << outbox;
}

// PROTOCOL §1.4.3 / GAME_DESIGN §8.3: a lobby disconnect destroys the room. That must hold
// whether the disconnect is observed while the room is still spawning or after it was
// registered — otherwise the room outlives its owner forever (nothing else would reap it).
TEST_F(LobbyTest, DisconnectAroundCreateRoomLeavesNoRoom) {
  ex_actor::ActorRef<LobbyActor> lobby =
      WaitValue(ex_actor::Spawn<LobbyActor>(/*seed=*/4242, kFastWindowMs, timer_, kFastReclaimMs));
  stdexec::sync_wait(lobby.Send<&LobbyActor::SetSelfRef>(lobby));
  auto conn = std::make_shared<Connection>(/*fd=*/-1, /*id=*/1, /*wake_write_fd=*/-1);

  stdexec::sync_wait(lobby.Send<&LobbyActor::CreateRoom>(SpawnConnector(conn, lobby), MakePlayer()));
  stdexec::sync_wait(lobby.Send<&LobbyActor::OnConnectionClosed>(MakePlayer(), ex_actor::ActorRef<SeatEndpoint> {}));

  const auto room_count = [&lobby]() { return WaitValue(lobby.Send<&LobbyActor::room_count>()); };
  ASSERT_TRUE(WaitFor([&room_count]() { return room_count() == 0; }))
      << "the room of a disconnected owner must not stay in the registry";
}

// Times the lobby registry under the real gate. CreateRoom's Spawn runs outside the lock, so
// a parallel burst should overlap; room_count is only the lock plus one map lookup.
TEST(LobbyPerf, RegistryLock) {
  constexpr int kRooms = 100;
  constexpr int kLookups = 2000;
  SetAiActionDelayMs(0);
  ex_actor::Init(/*thread_pool_size=*/8);
  auto timer = std::make_shared<WindowTimer>();
  auto player_ids = std::make_shared<PlayerIdAllocator>();
  auto sessions = std::make_shared<SessionRegistry>();

  auto spawn_connector = [&](int id, ex_actor::ActorRef<LobbyActor> lobby) {
    auto conn = std::make_shared<Connection>(/*fd=*/-1, id, /*wake_write_fd=*/-1);
    ex_actor::ActorRef<PlayerActor> connector =
        WaitValue(ex_actor::Spawn<PlayerActor>(conn, player_ids, /*db=*/DbService {}, sessions, lobby));
    stdexec::sync_wait(connector.Send<&PlayerActor::Dispatch>(PlayerEnvelope::SetSelfRef(connector)));
    return connector;
  };
  auto make_player = [](int id) {
    PlayerInfo player;
    player.player_id = "p-" + std::to_string(id);
    player.name = "n";
    return player;
  };

  ex_actor::ActorRef<LobbyActor> lobby =
      WaitValue(ex_actor::Spawn<LobbyActor>(/*seed=*/1, kFastWindowMs, timer, kFastReclaimMs));
  stdexec::sync_wait(lobby.Send<&LobbyActor::SetSelfRef>(lobby));

  std::vector<ex_actor::ActorRef<SeatEndpoint>> seats;
  seats.reserve(kRooms);
  for (int i = 0; i < kRooms; ++i) {
    seats.push_back(spawn_connector(i, lobby));
  }

  const auto started = std::chrono::steady_clock::now();
  for (int i = 0; i < kRooms; ++i) {
    stdexec::sync_wait(lobby.Send<&LobbyActor::CreateRoom>(seats[static_cast<size_t>(i)], make_player(i)));
  }
  const auto sequential_us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
  ASSERT_EQ(WaitValue(lobby.Send<&LobbyActor::room_count>()), static_cast<size_t>(kRooms));

  const auto lookups_started = std::chrono::steady_clock::now();
  exec::async_scope lookups;
  for (int i = 0; i < kLookups; ++i) {
    lookups.spawn(lobby.Send<&LobbyActor::room_count>() | stdexec::then([](size_t) {}));
  }
  stdexec::sync_wait(lookups.on_empty());
  const auto lookup_us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - lookups_started).count();

  // A second lobby, so the parallel creates are not queued behind the sequential ones.
  ex_actor::ActorRef<LobbyActor> parallel_lobby =
      WaitValue(ex_actor::Spawn<LobbyActor>(/*seed=*/2, kFastWindowMs, timer, kFastReclaimMs));
  stdexec::sync_wait(parallel_lobby.Send<&LobbyActor::SetSelfRef>(parallel_lobby));
  std::vector<ex_actor::ActorRef<SeatEndpoint>> parallel_seats;
  parallel_seats.reserve(kRooms);
  for (int i = 0; i < kRooms; ++i) {
    parallel_seats.push_back(spawn_connector(kRooms + i, parallel_lobby));
  }

  const auto parallel_started = std::chrono::steady_clock::now();
  exec::async_scope creates;
  for (int i = 0; i < kRooms; ++i) {
    creates.spawn(
        parallel_lobby.Send<&LobbyActor::CreateRoom>(parallel_seats[static_cast<size_t>(i)], make_player(kRooms + i)));
  }
  stdexec::sync_wait(creates.on_empty());
  const auto parallel_us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - parallel_started)
          .count();
  ASSERT_EQ(WaitValue(parallel_lobby.Send<&LobbyActor::room_count>()), static_cast<size_t>(kRooms));

  std::cout << "BENCH sequential CreateRoom x" << kRooms << ": " << (sequential_us / 1000) << " ms ("
            << (sequential_us / kRooms) << " us/op)\n";
  std::cout << "BENCH parallel   CreateRoom x" << kRooms << ": " << (parallel_us / 1000) << " ms ("
            << (parallel_us / kRooms) << " us/op)\n";
  std::cout << "BENCH room_count burst x" << kLookups << ": " << (lookup_us / 1000) << " ms (" << (lookup_us / kLookups)
            << " us/op)\n";

  timer->Stop();
  stdexec::sync_wait(ActorSendScope().on_empty());
  ex_actor::Shutdown();
}

// Full bot-played rounds on the shared pool. The human seat is entrusted after the deal, so
// each room runs its whole game inside one message and only meets the others at co_await.
TEST(RoomPerf, ConcurrentRooms) {
  constexpr int kPool = 8;
  SetAiActionDelayMs(0);
  ex_actor::Init(/*thread_pool_size=*/kPool);
  spdlog::set_level(spdlog::level::warn);

  auto make_seats = [](int id) {
    std::vector<RoomSeat> seats;
    RoomSeat human;
    human.seat = 0;
    human.player.player_id = "p-" + std::to_string(id);
    human.player.name = "n";
    human.is_bot = false;
    seats.push_back(std::move(human));
    for (int seat = 1; seat < kSeatCount; ++seat) {
      RoomSeat bot;
      bot.seat = seat;
      bot.player.player_id = "bot-" + std::to_string(seat);
      bot.player.name = "AI";
      bot.is_bot = true;
      seats.push_back(std::move(bot));
    }
    return seats;
  };

  auto play = [&](int rooms) {
    std::vector<ex_actor::ActorRef<RoomActor>> actors;
    actors.reserve(static_cast<size_t>(rooms));
    for (int i = 0; i < rooms; ++i) {
      actors.push_back(WaitValue(ex_actor::Spawn<RoomActor>("r-" + std::to_string(i), static_cast<uint64_t>(i + 1),
                                                            /*action_timeout_ms=*/15000, make_seats(i), nullptr,
                                                            /*ai_delay_ms=*/0)));
      // v1.6：**不装**调度器 —— 本基准的座位全部自动化，局间推进走"无调度器即 inline"回退
      // （B2 C6），一场 4..8 局在同一消息内打完；装了丢弃型调度器反而会让推进令牌石沉大海。
      PlayerInfo player;
      player.player_id = "p-" + std::to_string(i);
      player.name = "n";
      stdexec::sync_wait(actors.back().Send<&RoomActor::Dispatch>(RoomEnvelope::Ready(std::move(player))));
    }

    const auto started = std::chrono::steady_clock::now();
    exec::async_scope games;
    for (int i = 0; i < rooms; ++i) {
      PlayerInfo player;
      player.player_id = "p-" + std::to_string(i);
      player.name = "n";
      games.spawn(actors[static_cast<size_t>(i)].Send<&RoomActor::Dispatch>(
          RoomEnvelope::PlayerDisconnected(std::move(player), /*conn=*/ {})));
    }
    stdexec::sync_wait(games.on_empty());
    const auto us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();

    uint64_t seq_sum = 0;
    for (const ex_actor::ActorRef<RoomActor>& room : actors) {
      ASSERT_TRUE(WaitValue(room.Send<&RoomActor::game_finished>()));
      seq_sum += WaitValue(room.Send<&RoomActor::seq>());
    }
    std::cout << "BENCH " << rooms << " rooms on " << kPool << " workers: " << us << " us (" << (us / rooms)
              << " us/room, mean seq " << (seq_sum / static_cast<uint64_t>(rooms)) << ")\n";
  };

  play(1);
  play(8);
  play(32);

  stdexec::sync_wait(ActorSendScope().on_empty());
  ex_actor::Shutdown();
  spdlog::set_level(spdlog::level::info);
}

}  // namespace
}  // namespace mahjong
