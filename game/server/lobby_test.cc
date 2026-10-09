// Unit tests for LobbyActor's post-game reclaim (PROTOCOL.md §1.4.5).
//
// The test runs the real stack — a spawned LobbyActor, a real WindowTimer and a real
// RoomActor — with the window length and the reclaim delay shrunk, so a whole game auto-plays
// through the frozen timeout path (every human window times out at once) and the reclaim is
// observable in well under a second. No socket is involved: the connection is a real
// Connection over an invalid fd, which is enough to carry the outbox and the close request.

#include "actor_scope.h"
#include "lobby.h"

#include "connection_actor.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

#include <exec/async_scope.hpp>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "connection.h"
#include "ex_actor/api.h"
#include "player.h"
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

class LobbyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Bots must decide inline: this test never sleeps for AI pacing.
    SetAiActionDelayMs(0);
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
  // ConnectionActor wired to it — exactly what the gateway does, minus the socket. The
  // connector's own actor ref is what the lobby answers on.
  ex_actor::ActorRef<SeatEndpoint> SpawnConnector(const std::shared_ptr<Connection>& conn,
                                                  ex_actor::ActorRef<LobbyActor> lobby) {
    ex_actor::ActorRef<ConnectionActor> connector = WaitValue(
        ex_actor::Spawn<ConnectionActor>(conn, player_ids_, /*accounts=*/nullptr, sessions_, lobby));
    stdexec::sync_wait(connector.SendLocal<&ConnectionActor::Dispatch>(ConnectionEnvelope::SetSelfRef(connector)));
    return connector;
  }

  std::shared_ptr<WindowTimer> timer_;
  std::shared_ptr<PlayerIdAllocator> player_ids_ = std::make_shared<PlayerIdAllocator>();
  std::shared_ptr<SessionRegistry> sessions_ = std::make_shared<SessionRegistry>();
};

// §1.4.5: game_end + the reclaim delay is followed by the room leaving the registry and its
// connection being closed — even though the client never disconnected.
TEST_F(LobbyTest, ReclaimsARoomAndItsConnectionAfterGameEnd) {
  ex_actor::ActorRef<LobbyActor> lobby =
      WaitValue(ex_actor::Spawn<LobbyActor>(/*seed=*/20251005, kFastWindowMs, timer_, kFastReclaimMs));
  stdexec::sync_wait(lobby.SendLocal<&LobbyActor::SetSelfRef>(lobby));

  // An invalid fd: the outbox and the close request still work, and no socket is touched.
  auto conn = std::make_shared<Connection>(/*fd=*/-1, /*id=*/1, /*wake_write_fd=*/-1);
  ASSERT_FALSE(conn->close_requested());

  stdexec::sync_wait(lobby.SendLocal<&LobbyActor::CreateRoom>(SpawnConnector(conn, lobby), MakePlayer()));
  ASSERT_EQ(WaitValue(lobby.SendLocal<&LobbyActor::room_count>()), 1u);

  // Ready starts the game; with a 1ms window every human window times out, so the round plays
  // itself to game_end through the real scheduler.
  stdexec::sync_wait(
      lobby.SendLocal<&LobbyActor::Ready>(SpawnConnector(conn, lobby), MakePlayer()));

  const auto room_count = [&lobby]() { return WaitValue(lobby.SendLocal<&LobbyActor::room_count>()); };
  ASSERT_TRUE(WaitFor([&room_count]() { return room_count() == 0; })) << "the finished room was never reclaimed";
  EXPECT_TRUE(conn->close_requested()) << "the reclaim must close the room's connection";
}

// The reclaim is idempotent and must not touch anything else: a second delivery (a late timer,
// or a room that was already dropped with its owner) is a no-op.
TEST_F(LobbyTest, ReapRoomIsIdempotentAndLeavesOtherRoomsAlone) {
  ex_actor::ActorRef<LobbyActor> lobby =
      WaitValue(ex_actor::Spawn<LobbyActor>(/*seed=*/7, kFastWindowMs, timer_, kFastReclaimMs));
  stdexec::sync_wait(lobby.SendLocal<&LobbyActor::SetSelfRef>(lobby));

  auto first = std::make_shared<Connection>(/*fd=*/-1, /*id=*/1, /*wake_write_fd=*/-1);
  auto second = std::make_shared<Connection>(/*fd=*/-1, /*id=*/2, /*wake_write_fd=*/-1);
  PlayerInfo alice;
  alice.player_id = "p-alice";
  alice.name = "Alice";
  PlayerInfo bob;
  bob.player_id = "p-bob";
  bob.name = "Bob";

  stdexec::sync_wait(
      lobby.SendLocal<&LobbyActor::CreateRoom>(SpawnConnector(first, lobby), alice));
  stdexec::sync_wait(
      lobby.SendLocal<&LobbyActor::CreateRoom>(SpawnConnector(second, lobby), bob));
  ASSERT_EQ(WaitValue(lobby.SendLocal<&LobbyActor::room_count>()), 2u);

  stdexec::sync_wait(lobby.SendLocal<&LobbyActor::ReapRoom>(std::string("r-0001")));
  EXPECT_EQ(WaitValue(lobby.SendLocal<&LobbyActor::room_count>()), 1u);
  EXPECT_TRUE(first->close_requested());
  EXPECT_FALSE(second->close_requested()) << "reaping one room must not touch another";

  stdexec::sync_wait(lobby.SendLocal<&LobbyActor::ReapRoom>(std::string("r-0001")));  // idempotent
  EXPECT_EQ(WaitValue(lobby.SendLocal<&LobbyActor::room_count>()), 1u);
  EXPECT_FALSE(second->close_requested());
}

// PROTOCOL §4.2 / §1.4.1: create_room claims the player before Spawn returns. The claim is the
// `creating_` reservation, so two create_rooms written back to back (which is what a client
// that does not wait for room_joined does) cannot both succeed.
TEST_F(LobbyTest, PipelinedCreateRoomKeepsOneRoomPerPlayer) {
  ex_actor::ActorRef<LobbyActor> lobby =
      WaitValue(ex_actor::Spawn<LobbyActor>(/*seed=*/99, kFastWindowMs, timer_, kFastReclaimMs));
  stdexec::sync_wait(lobby.SendLocal<&LobbyActor::SetSelfRef>(lobby));
  auto conn = std::make_shared<Connection>(/*fd=*/-1, /*id=*/1, /*wake_write_fd=*/-1);

  // Neither send is awaited, so both reach the lobby before the first room finished spawning.
  ActorSendScope().spawn(lobby.SendLocal<&LobbyActor::CreateRoom>(SpawnConnector(conn, lobby), MakePlayer()));
  ActorSendScope().spawn(lobby.SendLocal<&LobbyActor::CreateRoom>(SpawnConnector(conn, lobby), MakePlayer()));

  const auto room_count = [&lobby]() { return WaitValue(lobby.SendLocal<&LobbyActor::room_count>()); };
  ASSERT_TRUE(WaitFor([&room_count]() { return room_count() == 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let a second (wrong) room appear
  EXPECT_EQ(room_count(), 1u) << "one player must own exactly one room";

  std::string outbox;
  conn->DrainOutbox(&outbox);
  EXPECT_EQ(CountOccurrences(outbox, "room_joined"), 1) << outbox;
  EXPECT_EQ(CountOccurrences(outbox, "BAD_REQUEST"), 1) << "the second create_room must be rejected: " << outbox;
}

// PROTOCOL §1.4.3 / GAME_DESIGN §8.3: a lobby disconnect destroys the room. That must hold
// whether the disconnect is observed while the room is still spawning or after it was
// registered — otherwise the room outlives its owner forever (nothing else would reap it).
TEST_F(LobbyTest, DisconnectAroundCreateRoomLeavesNoRoom) {
  ex_actor::ActorRef<LobbyActor> lobby =
      WaitValue(ex_actor::Spawn<LobbyActor>(/*seed=*/4242, kFastWindowMs, timer_, kFastReclaimMs));
  stdexec::sync_wait(lobby.SendLocal<&LobbyActor::SetSelfRef>(lobby));
  auto conn = std::make_shared<Connection>(/*fd=*/-1, /*id=*/1, /*wake_write_fd=*/-1);

  stdexec::sync_wait(lobby.SendLocal<&LobbyActor::CreateRoom>(SpawnConnector(conn, lobby), MakePlayer()));
  stdexec::sync_wait(
      lobby.SendLocal<&LobbyActor::OnConnectionClosed>(MakePlayer()));

  const auto room_count = [&lobby]() { return WaitValue(lobby.SendLocal<&LobbyActor::room_count>()); };
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
    ex_actor::ActorRef<ConnectionActor> connector =
        WaitValue(ex_actor::Spawn<ConnectionActor>(conn, player_ids, /*accounts=*/nullptr, sessions, lobby));
    stdexec::sync_wait(connector.SendLocal<&ConnectionActor::Dispatch>(ConnectionEnvelope::SetSelfRef(connector)));
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
  stdexec::sync_wait(lobby.SendLocal<&LobbyActor::SetSelfRef>(lobby));

  std::vector<ex_actor::ActorRef<SeatEndpoint>> seats;
  seats.reserve(kRooms);
  for (int i = 0; i < kRooms; ++i) {
    seats.push_back(spawn_connector(i, lobby));
  }

  const auto started = std::chrono::steady_clock::now();
  for (int i = 0; i < kRooms; ++i) {
    stdexec::sync_wait(lobby.SendLocal<&LobbyActor::CreateRoom>(seats[static_cast<size_t>(i)], make_player(i)));
  }
  const auto sequential_us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
  ASSERT_EQ(WaitValue(lobby.SendLocal<&LobbyActor::room_count>()), static_cast<size_t>(kRooms));

  const auto lookups_started = std::chrono::steady_clock::now();
  exec::async_scope lookups;
  for (int i = 0; i < kLookups; ++i) {
    lookups.spawn(lobby.SendLocal<&LobbyActor::room_count>() | stdexec::then([](size_t) {}));
  }
  stdexec::sync_wait(lookups.on_empty());
  const auto lookup_us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - lookups_started).count();

  // A second lobby, so the parallel creates are not queued behind the sequential ones.
  ex_actor::ActorRef<LobbyActor> parallel_lobby =
      WaitValue(ex_actor::Spawn<LobbyActor>(/*seed=*/2, kFastWindowMs, timer, kFastReclaimMs));
  stdexec::sync_wait(parallel_lobby.SendLocal<&LobbyActor::SetSelfRef>(parallel_lobby));
  std::vector<ex_actor::ActorRef<SeatEndpoint>> parallel_seats;
  parallel_seats.reserve(kRooms);
  for (int i = 0; i < kRooms; ++i) {
    parallel_seats.push_back(spawn_connector(kRooms + i, parallel_lobby));
  }

  const auto parallel_started = std::chrono::steady_clock::now();
  exec::async_scope creates;
  for (int i = 0; i < kRooms; ++i) {
    creates.spawn(parallel_lobby.SendLocal<&LobbyActor::CreateRoom>(parallel_seats[static_cast<size_t>(i)],
                                                                    make_player(kRooms + i)));
  }
  stdexec::sync_wait(creates.on_empty());
  const auto parallel_us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - parallel_started).count();
  ASSERT_EQ(WaitValue(parallel_lobby.SendLocal<&LobbyActor::room_count>()), static_cast<size_t>(kRooms));

  std::cout << "BENCH sequential CreateRoom x" << kRooms << ": " << (sequential_us / 1000) << " ms ("
            << (sequential_us / kRooms) << " us/op)\n";
  std::cout << "BENCH parallel   CreateRoom x" << kRooms << ": " << (parallel_us / 1000) << " ms ("
            << (parallel_us / kRooms) << " us/op)\n";
  std::cout << "BENCH room_count burst x" << kLookups << ": " << (lookup_us / 1000) << " ms ("
            << (lookup_us / kLookups) << " us/op)\n";

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
      stdexec::sync_wait(actors.back().SendLocal<&RoomActor::Dispatch>(
          RoomEnvelope::InstallTimeoutScheduler([](int64_t, uint64_t) {})));
      PlayerInfo player;
      player.player_id = "p-" + std::to_string(i);
      player.name = "n";
      stdexec::sync_wait(actors.back().SendLocal<&RoomActor::Dispatch>(RoomEnvelope::Ready(std::move(player))));
    }

    const auto started = std::chrono::steady_clock::now();
    exec::async_scope games;
    for (int i = 0; i < rooms; ++i) {
      PlayerInfo player;
      player.player_id = "p-" + std::to_string(i);
      player.name = "n";
      games.spawn(actors[static_cast<size_t>(i)].SendLocal<&RoomActor::Dispatch>(
          RoomEnvelope::PlayerDisconnected(std::move(player))));
    }
    stdexec::sync_wait(games.on_empty());
    const auto us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();

    uint64_t seq_sum = 0;
    for (const ex_actor::ActorRef<RoomActor>& room : actors) {
      ASSERT_TRUE(WaitValue(room.SendLocal<&RoomActor::game_finished>()));
      seq_sum += WaitValue(room.SendLocal<&RoomActor::seq>());
    }
    std::cout << "BENCH " << rooms << " rooms on " << kPool << " workers: " << us << " us ("
              << (us / rooms) << " us/room, mean seq " << (seq_sum / static_cast<uint64_t>(rooms)) << ")\n";
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
