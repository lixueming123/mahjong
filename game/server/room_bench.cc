// room_bench.cc — a load test for RoomActor: what one game costs, and how many rooms one core
// can carry.
//
// Why it exists: the room is where the game's own CPU time is spent, so every capacity question
// ("how many tables fit on this box?") is a question about this file's numbers. The frame-level
// micro-benchmark that justified the seats[] hoist answered "how fast is one frame"; this one
// answers "how fast is a whole game, over the real actor path".
//
// What it measures, in order:
//   1. framework baselines — one actor message (awaited round trip, and a one-way burst),
//      AsyncGate, std::mutex. Everything below is read against these.
//   2. three room shapes, each a full game played to game_end:
//        bots    — 1 wire-less human + 3 bots: the state machine and the rules, no delivery;
//        solo    — 1 wired human + 3 bots: the shipping shape (one human, three AI seats);
//        table4  — 4 wired humans: the maximum framing and fan-out the protocol allows.
//   3. scaling — K rooms of the shipping shape on the production 4-thread pool, K = 1..16.
//
// The client stand-in (AutoClient) is a SeatEndpoint, exactly like PlayerActor, but it
// answers the room instead of writing into a socket outbox: it replies to every action_request
// with a legal frame (discard the tile it was just handed, pass on every claim). That is what a
// real client does, minus the network — so these are the server's own numbers, not an RTT. It
// posts its reply with ActorSendScope().spawn(), i.e. fire-and-forget, which is also what
// PlayerActor does; awaiting the room from inside a seat handler would deadlock, because the
// room holds its gate and is parked waiting for that very handler.
//
// Pacing is 0 (ai_delay_ms = 0) on purpose: the benchmark wants the room's ceiling, and every
// bot decision then happens inside the message that opened its window. Production runs at 900 ms
// per bot action, where the same CPU work is spread across a game's wall-clock seconds — the
// `us/game` column is what turns one into the other (see the capacity note the program prints).
//
// Logging is off: the room logs every meld and every game, and at tens of thousands of games
// that logging *is* the measurement.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <variant>
#include <vector>

#include <exec/task.hpp>
#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

#include "actor_scope.h"
#include "ex_actor/api.h"
#include "json.h"
#include "mahjong_rules.h"
#include "player.h"
#include "room.h"
#include "seat_endpoint.h"

namespace ex = stdexec;
namespace mj = mahjong;

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kThreadPoolSize = 4;  // main.cc's pool; the scaling numbers are read against it
constexpr uint64_t kBaseSeed = 20251005;
constexpr int kActionTimeoutMs = 15000;

bool g_trace = false;
bool g_spin_wait = false;             // poll instead of blocking in a condition variable
int g_wait_ms = 5000;                 // how long a batch may take before it is called stalled
std::atomic<int64_t> g_ready_ns {0};  // when the current batch's Ready messages were posted

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// CPU time of the whole process. macOS has no CLOCK_PROCESS_CPUTIME_ID and getrusage is the
// portable answer; nothing but this benchmark is running, so process time is benchmark time.
double CpuSeconds() {
  rusage usage {};
  getrusage(RUSAGE_SELF, &usage);
  const double user = static_cast<double>(usage.ru_utime.tv_sec) + static_cast<double>(usage.ru_utime.tv_usec) / 1e6;
  const double sys = static_cast<double>(usage.ru_stime.tv_sec) + static_cast<double>(usage.ru_stime.tv_usec) / 1e6;
  return user + sys;
}

// ---------------------------------------------------------------------------------------
// Tiny scanners over one outbound frame. The bench client stands in for a client on the wire:
// it needs the frame kind, one field and one array head, and scanning for those by hand keeps
// the harness's own cost off the measurement it is trying to take (a real client parses the
// whole frame; the server never does).
// ---------------------------------------------------------------------------------------
std::string StringField(const std::string& text, std::string_view key) {
  const std::string pattern = "\"" + std::string(key) + "\":\"";
  const size_t at = text.find(pattern);
  if (at == std::string::npos) {
    return {};
  }
  const size_t from = at + pattern.size();
  const size_t to = text.find('"', from);
  return to == std::string::npos ? std::string() : text.substr(from, to - from);
}

int64_t IntField(const std::string& text, std::string_view key) {
  const std::string pattern = "\"" + std::string(key) + "\":";
  const size_t at = text.find(pattern);
  if (at == std::string::npos) {
    return -1;
  }
  return std::strtoll(text.c_str() + at + pattern.size(), nullptr, 10);
}

// The first entry of a "hand" array, used exactly once per game: the dealer's first discard has
// no draw record, so the dealt hand is the only legal source for it. Tile codes are always two
// characters ("9s" / "1z"), so the head of the array is enough.
std::string FirstHandTile(const std::string& text) {
  const size_t at = text.find("\"hand\":[\"");
  if (at == std::string::npos) {
    return {};
  }
  return text.substr(at + 9, 2);
}

int64_t Percentile(std::vector<int64_t> samples, double fraction) {
  if (samples.empty()) {
    return 0;
  }
  std::sort(samples.begin(), samples.end());
  const size_t index = static_cast<size_t>(fraction * static_cast<double>(samples.size() - 1) + 0.5);
  return samples[std::min(index, samples.size() - 1)];
}

// ---------------------------------------------------------------------------------------
// The client stand-in
// ---------------------------------------------------------------------------------------

struct ClientStats {
  std::atomic<int64_t> frames {0};
  std::atomic<int64_t> bytes {0};
  std::atomic<int64_t> max_seq {0};
  // Written only by this client's own handlers, which the framework keeps serialized (this
  // handler never suspends, so no two of them overlap) and read after the whole batch drained.
  std::vector<int64_t> turnarounds_ns;
  // The timeout dump: what this seat last heard, and the room's own numbers as of that frame.
  std::vector<std::string> last_types;
  int64_t last_seq = -1;
  int64_t last_remaining = -1;
  int64_t last_current_seat = -1;
  // Arrival times, so a stalled batch can be told apart from a slow one.
  int64_t first_frame_ns = 0;
  int64_t game_end_ns = 0;
  int64_t last_frame_ns = 0;
  int64_t milestone_ns[4] = {0, 0, 0, 0};  // arrival of frame #50, #100, #150, #200
};

class AutoClient : public mj::SeatEndpoint {
 public:
  AutoClient(std::string player_id, std::shared_ptr<ClientStats> stats) : stats_(std::move(stats)) {
    player_.player_id = std::move(player_id);
    player_.name = "Human";
  }

  // The room's ref, delivered after both actors exist (PlayerActor has the same handshake).
  void SetRoom(ex_actor::ActorRef<mj::RoomActor> room) { room_ = std::move(room); }

  exec::task<void> Dispatch(mj::PlayerEnvelope envelope) override {
    // A room only ever delivers a seat frame or a seat error (seat_endpoint.h); the connection's
    // own kinds are not this client's business.
    const std::string* payload = nullptr;
    if (auto* m = std::get_if<mj::PlayerEnvelope::SeatFrameMsg>(&envelope.msg)) {
      payload = &m->frame;
    } else if (auto* m = std::get_if<mj::PlayerEnvelope::SeatErrorMsg>(&envelope.msg)) {
      payload = &m->frame;
    }
    if (payload == nullptr) {
      co_return;
    }
    const int64_t frame_index = stats_->frames.fetch_add(1, std::memory_order_relaxed) + 1;
    stats_->bytes.fetch_add(static_cast<int64_t>(payload->size()), std::memory_order_relaxed);

    // A discard we posted earlier came back as the table's discard_event: the round trip a
    // player actually feels, including the four-seat fan-out.
    if (discard_probe_ns_ != 0) {
      stats_->turnarounds_ns.push_back(NowNs() - discard_probe_ns_);
      discard_probe_ns_ = 0;
    }

    const std::string& text = *payload;
    const std::string type = StringField(text, "type");
    stats_->last_types.push_back(type);  // a rolling window of the last few kinds
    if (stats_->last_types.size() > 12) {
      stats_->last_types.erase(stats_->last_types.begin());
    }
    const int64_t now = NowNs();
    stats_->last_frame_ns = now;
    if (stats_->first_frame_ns == 0) {
      stats_->first_frame_ns = now;
    }
    if (frame_index % 50 == 0 && frame_index / 50 >= 1 && frame_index / 50 <= 4) {
      stats_->milestone_ns[frame_index / 50 - 1] = now;
    }
    if (type == "game_end") {
      stats_->game_end_ns = now;
    }
    if (type == "state_sync") {
      stats_->max_seq.store(IntField(text, "seq"), std::memory_order_relaxed);
      stats_->last_seq = IntField(text, "seq");
      stats_->last_remaining = IntField(text, "remaining");
      stats_->last_current_seat = IntField(text, "current_seat");
      co_return;
    }
    if (type == "draw") {
      drawn_ = StringField(text, "tile");  // discarding the drawn tile is always legal
      co_return;
    }
    if (type == "game_start") {
      if (first_tile_.empty()) {
        first_tile_ = FirstHandTile(text);
      }
      co_return;
    }
    if (type != "action_request") {
      co_return;
    }

    const int64_t seq = IntField(text, "seq");
    const bool claim = StringField(text, "window") == "claim";
    std::string reply;
    if (claim) {
      reply = "{\"type\":\"action\",\"action\":\"pass\",\"tiles\":[],\"seq\":" + std::to_string(seq) + "}";
    } else {
      const std::string tile = drawn_.empty() ? first_tile_ : drawn_;
      if (tile.empty()) {
        co_return;  // unreachable: a discard window always has a draw record or a dealt hand
      }
      reply = "{\"type\":\"discard\",\"tile\":\"" + tile + "\",\"seq\":" + std::to_string(seq) + "}";
      discard_probe_ns_ = NowNs();
    }
    std::string error;
    std::optional<mj::json::Value> frame = mj::json::Value::Parse(reply, &error);
    if (!frame.has_value()) {
      co_return;
    }
    // Fire-and-forget, never awaited: see the file comment.
    mj::ActorSendScope().spawn(
        room_.SendLocal<&mj::RoomActor::Dispatch>(mj::RoomEnvelope::GameFrame(player_, std::move(*frame))));
    co_return;
  }

 private:
  std::shared_ptr<ClientStats> stats_;
  mj::PlayerInfo player_;
  ex_actor::ActorRef<mj::RoomActor> room_;
  std::string drawn_;       // the last tile this seat drew
  std::string first_tile_;  // the head of the dealt hand (dealer's first discard only)
  int64_t discard_probe_ns_ = 0;
};

// A do-nothing actor: the framework baseline, and the target of the diagnostic poker below.
class CounterActor {
 public:
  explicit CounterActor(std::shared_ptr<std::atomic<int64_t>> count) : count_(std::move(count)) {}

  void Bump() { count_->fetch_add(1, std::memory_order_relaxed); }

 private:
  std::shared_ptr<std::atomic<int64_t>> count_;
};

// Waits for every room of one batch to reach game_end.
class Batch {
 public:
  explicit Batch(int rooms) : remaining_(rooms) {}

  std::atomic<int> finished_seen {0};  // how often the room actually called back

  void OnFinished() {
    finished_seen.fetch_add(1, std::memory_order_relaxed);
    if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
      cv_.notify_all();
    }
  }

  // |spin| picks a polling wait. Which one the driver uses turns out to matter a great deal (see
  // the file comment): a driver blocked in a condition variable can leave the actor pool with no
  // thread to run the next message, and the batch only moves again once the driver wakes up.
  bool WaitFor(int timeout_ms, bool spin = false) {
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    if (spin) {
      while (Clock::now() < deadline) {
        if (Done()) {
          return true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      return Done();
    }
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return done_; });
  }

  bool Done() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_;
  }

  int Remaining() const { return remaining_.load(std::memory_order_relaxed); }

 private:
  mutable std::mutex mutex_;
  std::atomic<int> remaining_;
  std::condition_variable cv_;
  bool done_ = false;
};

// Once a round has stalled, the actor world holds a room that will only ever run again if a
// message reaches it. Any send still in flight to that room is therefore uncompletable, and
// waiting on the send scope from then on would hang the benchmark instead of reporting it. So
// after the first stall the drain is skipped and the process exits without tearing the actor
// world down (see the end of main).
std::atomic<bool> g_stalled_once {false};

// A diagnostic, not part of the measurement: a thread that enqueues harmless messages into the
// actor system every few milliseconds. It can poke an unrelated actor, and/or every room in the
// batch being measured. Which of the two unsticks a stalled batch says where the stall lives: an
// unrelated actor means the pool lost a wakeup, a room means the room's own delivery chain died.
std::atomic<bool> g_poker_stop {false};
bool g_poke_rooms = false;
ex_actor::ActorRef<CounterActor> g_poker;
std::mutex g_rooms_mutex;
std::vector<ex_actor::ActorRef<mj::RoomActor>> g_rooms;

void StartPoker() {
  g_poker_stop.store(false);
  std::thread([] {
    const mj::PlayerInfo nobody {.player_id = "poke-no-such-player"};
    while (!g_poker_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      if (g_poker_stop.load()) {
        break;
      }
      mj::ActorSendScope().spawn(g_poker.SendLocal<&CounterActor::Bump>());
      if (g_poke_rooms) {
        std::lock_guard<std::mutex> lock(g_rooms_mutex);
        for (const ex_actor::ActorRef<mj::RoomActor>& room : g_rooms) {
          // A seat the room does not know: SeatOf() returns -1 and the handler returns without
          // touching any state. A pure "is the room still running messages" probe.
          mj::ActorSendScope().spawn(
              room.SendLocal<&mj::RoomActor::Dispatch>(mj::RoomEnvelope::PlayerDisconnected(nobody)));
        }
      }
    }
  }).detach();
}

// ---------------------------------------------------------------------------------------
// Room shapes
// ---------------------------------------------------------------------------------------

enum class Shape { kBots, kSolo, kTable4 };

const char* ShapeName(Shape shape) {
  switch (shape) {
    case Shape::kBots:
      return "bots   (1 wire-less human + 3 bots, no delivery)";
    case Shape::kSolo:
      return "solo   (1 wired human + 3 bots, shipping shape)";
    case Shape::kTable4:
      return "table4 (4 wired humans, max fan-out)";
  }
  return "?";
}

int WiredSeats(Shape shape) {
  switch (shape) {
    case Shape::kBots:
      return 0;
    case Shape::kSolo:
      return 1;
    case Shape::kTable4:
      return mj::kSeatCount;
  }
  return 0;
}

struct RoomRun {
  ex_actor::ActorRef<mj::RoomActor> room;
  mj::PlayerInfo ready_player;  // seat 0, the seat that starts the game
  std::vector<mj::PlayerInfo> wired_players;
  std::vector<std::shared_ptr<ClientStats>> stats;
};

// A stalled batch is the one failure mode worth diagnosing automatically: dump what every seat
// last heard, and ask the room for its own numbers (a room waiting for a reply has released its
// gate, so a `sync` message still gets answered).
void DumpStalled(const std::vector<RoomRun>& runs, const Batch& batch) {
  // A `sync` probe answers one question: is the room idle (and simply never called back), or is
  // it parked inside a handler with the gate held (in which case the probe is never served)?
  std::vector<int64_t> before;
  for (const RoomRun& run : runs) {
    before.push_back(run.stats.empty() ? 0 : run.stats.front()->frames.load(std::memory_order_relaxed));
    for (const mj::PlayerInfo& player : run.wired_players) {
      mj::ActorSendScope().spawn(
          run.room.SendLocal<&mj::RoomActor::Dispatch>(mj::RoomEnvelope::Sync(player)));
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  printf("  finished-callback invocations: %d\n", batch.finished_seen.load());
  for (size_t index = 0; index < runs.size(); ++index) {
    const RoomRun& run = runs[index];
    const int64_t after = run.stats.empty() ? 0 : run.stats.front()->frames.load(std::memory_order_relaxed);
    printf("  room %zu: sync probe answered %lld frame(s) -> %s\n", index,
           static_cast<long long>(after - before[index]),
           after - before[index] > 0 ? "idle, callback never ran" : "PARKED with the gate held");
    printf("  room %zu: room says seq=%lld remaining=%lld current_seat=%lld\n", index,
           static_cast<long long>(run.stats.empty() ? -1 : run.stats.front()->last_seq),
           static_cast<long long>(run.stats.empty() ? -1 : run.stats.front()->last_remaining),
           static_cast<long long>(run.stats.empty() ? -1 : run.stats.front()->last_current_seat));
    for (size_t seat = 0; seat < run.stats.size(); ++seat) {
      const ClientStats& stats = *run.stats[seat];
      printf("    seat %zu: %lld frames, last_remaining=%lld last_current_seat=%lld  heard:", seat,
             static_cast<long long>(stats.frames.load()), static_cast<long long>(stats.last_remaining),
             static_cast<long long>(stats.last_current_seat));
      for (const std::string& type : stats.last_types) {
        printf(" %s", type.c_str());
      }
      printf("\n      first frame at %.1f ms, game_end at %.1f ms, last frame at %.1f ms (since Ready)\n",
             static_cast<double>(stats.first_frame_ns - g_ready_ns.load()) / 1e6,
             stats.game_end_ns == 0 ? -1.0 : static_cast<double>(stats.game_end_ns - g_ready_ns.load()) / 1e6,
             static_cast<double>(stats.last_frame_ns - g_ready_ns.load()) / 1e6);
      printf("      frames #50/#100/#150/#200 at:");
      for (int64_t milestone : stats.milestone_ns) {
        printf(" %s", milestone == 0 ? "-" : std::to_string((milestone - g_ready_ns.load()) / 1000000).c_str());
      }
      printf(" ms\n");
    }
  }
}

RoomRun SpawnRoom(const std::string& room_id, uint64_t seed, Shape shape, std::shared_ptr<Batch> batch,
                  int action_timeout_ms) {
  RoomRun run;
  const int wired = WiredSeats(shape);
  std::vector<mj::RoomSeat> seats;
  std::vector<ex_actor::ActorRef<AutoClient>> clients;
  for (int seat = 0; seat < mj::kSeatCount; ++seat) {
    mj::RoomSeat slot;
    slot.seat = seat;
    const bool human = seat == 0 || wired == mj::kSeatCount;
    slot.is_bot = !human;
    if (slot.is_bot) {
      slot.player.player_id = room_id + "-bot" + std::to_string(seat);
      slot.player.name = "AI";
    } else {
      slot.player.player_id = room_id + "-p" + std::to_string(seat);
      slot.player.name = "Human";
    }
    if (seat == 0) {
      run.ready_player = slot.player;
    }
    seats.push_back(std::move(slot));
  }
  for (int seat = 0; seat < wired; ++seat) {
    auto stats = std::make_shared<ClientStats>();
    run.stats.push_back(stats);
    if (g_trace) {
      fprintf(stderr, "    spawn client %d\n", seat);
    }
    ex_actor::ActorRef<AutoClient> client =
        std::get<0>(ex::sync_wait(ex_actor::Spawn<AutoClient>(seats[static_cast<size_t>(seat)].player.player_id, stats))
                        .value());
    seats[static_cast<size_t>(seat)].endpoint = client;
    clients.push_back(client);
    run.wired_players.push_back(seats[static_cast<size_t>(seat)].player);
  }

  if (g_trace) {
    fprintf(stderr, "    spawn room (%d wired seats)\n", wired);
  }
  run.room = std::get<0>(
      ex::sync_wait(ex_actor::Spawn<mj::RoomActor>(room_id, seed, action_timeout_ms, std::move(seats), nullptr, 0))
          .value());
  for (const ex_actor::ActorRef<AutoClient>& client : clients) {
    if (g_trace) {
      fprintf(stderr, "    setroom\n");
    }
    ex::sync_wait(client.SendLocal<&AutoClient::SetRoom>(run.room));
  }
  // No window deadlines: the client replies immediately, and a timer that never fires keeps the
  // run deterministic. The room only needs a scheduler to exist (ai_delay_ms = 0 means no bot
  // pacing either), otherwise OpenWindowTimer logs an error per human window.
  if (g_trace) {
    fprintf(stderr, "    install scheduler\n");
  }
  ex::sync_wait(run.room.SendLocal<&mj::RoomActor::Dispatch>(
      mj::RoomEnvelope::InstallTimeoutScheduler([](int64_t, uint64_t) {})));
  if (g_trace) {
    fprintf(stderr, "    install finished callback\n");
  }
  ex::sync_wait(run.room.SendLocal<&mj::RoomActor::Dispatch>(
      mj::RoomEnvelope::InstallFinishedCallback([batch](const mj::GameSettlement&) { batch->OnFinished(); })));
  return run;
}

struct RunResult {
  int rooms = 0;
  int games = 0;
  double wall_s = 0;
  double cpu_s = 0;
  int64_t frames = 0;
  int64_t bytes = 0;
  int64_t transitions = 0;  // final seq, i.e. window transitions of one game
  int stalled = 0;          // rounds that hit the watchdog (see RunBest)
  std::vector<int64_t> turnarounds_ns;

  double GamesPerSecond() const { return wall_s > 0 ? static_cast<double>(games) / wall_s : 0; }
  double UsPerGame() const { return games > 0 ? wall_s * 1e6 / static_cast<double>(games) : 0; }
  double CpuUsPerGame() const { return games > 0 ? cpu_s * 1e6 / static_cast<double>(games) : 0; }
  double FramesPerGame() const { return games > 0 ? static_cast<double>(frames) / static_cast<double>(games) : 0; }
  double BytesPerGame() const { return games > 0 ? static_cast<double>(bytes) / static_cast<double>(games) : 0; }
};

// One round: |rooms| rooms, each playing exactly one game to game_end.
std::optional<RunResult> RunRound(Shape shape, int rooms, uint64_t base_seed, int action_timeout_ms) {
  auto batch = std::make_shared<Batch>(rooms);
  std::vector<RoomRun> runs;
  runs.reserve(static_cast<size_t>(rooms));
  for (int index = 0; index < rooms; ++index) {
    const std::string room_id = "bench-" + std::to_string(index);
    runs.push_back(SpawnRoom(room_id, mj::DeriveRoomSeed(base_seed, static_cast<uint32_t>(index)), shape, batch,
                             action_timeout_ms));
  }

  RunResult result;
  result.rooms = rooms;
  const double cpu_before = CpuSeconds();
  const Clock::time_point start = Clock::now();
  g_ready_ns.store(NowNs());
  {
    std::lock_guard<std::mutex> lock(g_rooms_mutex);
    g_rooms.clear();
    for (const RoomRun& run : runs) {
      g_rooms.push_back(run.room);
    }
  }
  for (const RoomRun& run : runs) {
    mj::ActorSendScope().spawn(
        run.room.SendLocal<&mj::RoomActor::Dispatch>(mj::RoomEnvelope::Ready(run.ready_player)));
  }
  // A game takes tens of milliseconds at ai_delay 0, so a quarter of a minute is ~300x the
  // expected time: if it has not finished by then it never will, and the dump below is the
  // diagnosis rather than a wait.
  const bool done = batch->WaitFor(g_wait_ms, g_spin_wait);
  const Clock::time_point end = Clock::now();
  result.cpu_s = CpuSeconds() - cpu_before;
  result.wall_s = std::chrono::duration<double>(end - start).count();
  if (!done) {
    printf("  !!! timed out after %.1fs (%d/%d games finished)\n", result.wall_s, rooms - batch->Remaining(), rooms);
    DumpStalled(runs, *batch);
    return std::nullopt;
  }

  result.games = rooms;
  for (const RoomRun& run : runs) {
    for (const std::shared_ptr<ClientStats>& stats : run.stats) {
      result.frames += stats->frames.load(std::memory_order_relaxed);
      result.bytes += stats->bytes.load(std::memory_order_relaxed);
      result.transitions = std::max(result.transitions, stats->max_seq.load(std::memory_order_relaxed));
      result.turnarounds_ns.insert(result.turnarounds_ns.end(), stats->turnarounds_ns.begin(),
                                   stats->turnarounds_ns.end());
    }
  }
  // Every message this bench sent by hand has now been handled; leaving them in scope would let
  // a parked handler outlive the actor world at Shutdown (see actor_scope.h). Skipped once a
  // round has stalled, for the reason given at g_stalled_once.
  if (!g_stalled_once.load()) {
    ex::sync_wait(mj::ActorSendScope().on_empty());
  }
  return result;
}

// Best of |rounds|, because one round is one sample and this machine's scheduler is noisy.
// The sample count is set so that every configuration plays roughly the same number of games:
// a run of K rooms is K games, and the run is long enough that one noisy round cannot win.
RunResult RunBest(Shape shape, int rooms, uint64_t base_seed, int action_timeout_ms, int target_games) {
  const int rounds = std::max(1, target_games / std::max(rooms, 1));
  RunResult best;
  for (int round = 0; round < rounds; ++round) {
    std::optional<RunResult> one = RunRound(shape, rooms, base_seed, action_timeout_ms);
    if (!one.has_value()) {
      // A stalled round is the framework's problem, not the room's: the round is discarded and
      // counted, and the shape still reports the rounds that did run. (See the file comment on
      // the lost-activation race this bench found.)
      ++best.stalled;
      g_stalled_once.store(true);
      continue;
    }
    if (best.games == 0 || one->GamesPerSecond() > best.GamesPerSecond()) {
      const int stalled = best.stalled;
      best = std::move(*one);
      best.stalled = stalled;
    }
    if (g_trace) {
      fprintf(stderr, "  [%s rooms=%d round %d/%d] %.1f ms, %.1f ms/game, %.1f games/s\n", ShapeName(shape), rooms,
              round + 1, rounds, one->wall_s * 1e3, one->UsPerGame() / 1e3, one->GamesPerSecond());
    }
  }
  return best;
}

void PrintDetail(Shape shape, const RunResult& result, double paced_game_seconds) {
  printf("\n== %s ==\n", ShapeName(shape));
  printf("  %d room(s), %d game(s) played to game_end\n", result.rooms, result.games);
  if (result.stalled > 0) {
    printf("  !! %d round(s) hit the %d ms watchdog and were discarded (lost-activation race)\n", result.stalled,
           g_wait_ms);
  }
  printf("  wall       %8.2f ms total   %8.3f ms/game\n", result.wall_s * 1e3, result.UsPerGame() / 1e3);
  printf("  throughput %8.1f games/s\n", result.GamesPerSecond());
  printf("  cpu        %8.3f ms/game\n", result.CpuUsPerGame() / 1e3);
  printf("  transitions %7lld seq/game\n", static_cast<long long>(result.transitions));
  if (result.frames > 0) {
    printf("  delivery   %8.0f frames/game  %.1f KB/game  (all seats)\n", result.FramesPerGame(),
           result.BytesPerGame() / 1024.0);
    printf("  turnaround  p50 %6.1f us   p99 %7.1f us   n=%zu\n",
           static_cast<double>(Percentile(result.turnarounds_ns, 0.50)) / 1e3,
           static_cast<double>(Percentile(result.turnarounds_ns, 0.99)) / 1e3, result.turnarounds_ns.size());
  }
  if (paced_game_seconds > 0 && result.CpuUsPerGame() > 0) {
    const double cpu_per_game_s = result.CpuUsPerGame() / 1e6;
    printf("  capacity   %.0f rooms/core at %.0fs game wall time (%.3f s cpu per game)\n",
           paced_game_seconds / cpu_per_game_s, paced_game_seconds, cpu_per_game_s);
  }
}

void PrintScaling(Shape shape, const std::vector<int>& room_counts, uint64_t base_seed, int action_timeout_ms,
                  int target_games) {
  printf("\n== scaling: %s, production pool of %d threads ==\n", ShapeName(shape), kThreadPoolSize);
  printf("  %5s %7s %9s %10s %11s %10s %11s\n", "rooms", "games", "wall ms", "games/s", "us/game", "cpu ms/gm",
         "cpu cores");
  for (int rooms : room_counts) {
    const RunResult result = RunBest(shape, rooms, base_seed, action_timeout_ms, target_games);
    if (result.games == 0) {
      continue;
    }
    // cpu cores = CPU seconds burned per wall second: how much of the pool this batch actually
    // used. It saturates at kThreadPoolSize.
    const double cpu_cores = result.wall_s > 0 ? result.cpu_s / result.wall_s : 0;
    printf("  %5d %7d %9.1f %10.1f %11.1f %10.3f %11.2f%s\n", result.rooms, result.games, result.wall_s * 1e3,
           result.GamesPerSecond(), result.UsPerGame(), result.CpuUsPerGame() / 1e3, cpu_cores,
           result.stalled > 0 ? "   (+stalled round)" : "");
  }
}

// ---------------------------------------------------------------------------------------
// Framework baselines
// ---------------------------------------------------------------------------------------

void PrintBaselines(ex_actor::ActorRef<CounterActor> counter, std::shared_ptr<std::atomic<int64_t>> count) {
  printf("\n== framework baselines ==\n");
  {
    std::mutex mutex;
    constexpr long kN = 2000000;
    const Clock::time_point start = Clock::now();
    for (long i = 0; i < kN; ++i) {
      std::lock_guard<std::mutex> lock(mutex);
    }
    const Clock::time_point end = Clock::now();
    printf("  %-44s %9.1f ns\n", "std::mutex lock+unlock (uncontended)",
           std::chrono::duration<double, std::nano>(end - start).count() / static_cast<double>(kN));
  }

  {
    // The room's gate, uncontended: what the whole "one handler at a time" guarantee costs per
    // message. A bare task of the same shape is measured first, because `co_await` on an
    // exec::task has a fixed cost of its own and the gate number is meaningless without it.
    constexpr long kN = 200000;
    const Clock::time_point nop_start = Clock::now();
    ex::sync_wait([&]() -> exec::task<void> {
      for (long i = 0; i < kN; ++i) {
        co_await []() -> exec::task<void> { co_return; }();
      }
    }());
    const Clock::time_point nop_end = Clock::now();
    printf("  %-44s %9.1f ns\n", "  (a bare nested exec::task, for comparison)",
           std::chrono::duration<double, std::nano>(nop_end - nop_start).count() / static_cast<double>(kN));

    static mj::AsyncGate gate;
    const Clock::time_point start = Clock::now();
    ex::sync_wait([&]() -> exec::task<void> {
      for (long i = 0; i < kN; ++i) {
        mj::AsyncGate::Guard guard = co_await gate.Acquire();
      }
    }());
    const Clock::time_point end = Clock::now();
    printf("  %-44s %9.1f ns\n", "AsyncGate acquire+release (uncontended)",
           std::chrono::duration<double, std::nano>(end - start).count() / static_cast<double>(kN));
  }

  {
    // One awaited message: enqueue, wake a pool thread, run the handler, hand the result back.
    constexpr long kN = 20000;
    const Clock::time_point start = Clock::now();
    for (long i = 0; i < kN; ++i) {
      ex::sync_wait(counter.SendLocal<&CounterActor::Bump>());
    }
    const Clock::time_point end = Clock::now();
    printf("  %-44s %9.1f ns\n", "one actor message, awaited round trip",
           std::chrono::duration<double, std::nano>(end - start).count() / static_cast<double>(kN));
  }

  {
    // One-way: what the room's fan-out and the connections' inbound frames actually cost.
    constexpr long kN = 200000;
    count->store(0);
    const Clock::time_point start = Clock::now();
    for (long i = 0; i < kN; ++i) {
      mj::ActorSendScope().spawn(counter.SendLocal<&CounterActor::Bump>());
    }
    ex::sync_wait(mj::ActorSendScope().on_empty());
    const Clock::time_point end = Clock::now();
    printf("  %-44s %9.1f ns (%lld handled)\n", "one actor message, one-way burst",
           std::chrono::duration<double, std::nano>(end - start).count() / static_cast<double>(kN),
           static_cast<long long>(count->load()));
  }
}

// The wall-clock length of one game in production, where every bot action waits ai_delay_ms
// (GAME_DESIGN §5.4). ~11 bot actions out of every 15 windows is the shape this benchmark sees,
// so one window is roughly 0.9 s of the game's wall time: that is the divisor for "rooms/core".
double PacedGameSeconds(const RunResult& result, int ai_delay_ms) {
  return static_cast<double>(result.transitions) * static_cast<double>(ai_delay_ms) / 1000.0;
}

}  // namespace

int main(int argc, char** argv) {
  int max_rooms = 16;
  int target_games = 8;
  bool scaling = true;
  bool detail = true;
  bool poke = false;
  bool only_oneshape = false;
  Shape only_shape = Shape::kSolo;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--max-rooms" && i + 1 < argc) {
      max_rooms = std::atoi(argv[++i]);
    } else if (arg == "--games" && i + 1 < argc) {
      target_games = std::atoi(argv[++i]);
    } else if (arg == "--shape" && i + 1 < argc) {
      const std::string name = argv[++i];
      only_oneshape = true;
      only_shape = name == "bots" ? Shape::kBots : (name == "solo" ? Shape::kSolo : Shape::kTable4);
    } else if (arg == "--trace") {
      g_trace = true;
    } else if (arg == "--poke") {
      poke = true;
    } else if (arg == "--poke-rooms") {
      poke = true;
      g_poke_rooms = true;
    } else if (arg == "--spin") {
      g_spin_wait = true;
    } else if (arg == "--wait-ms" && i + 1 < argc) {
      g_wait_ms = std::atoi(argv[++i]);
    } else if (arg == "--detail-only") {
      detail = false;
      scaling = false;
    } else if (arg == "--scaling-only") {
      detail = false;
    }
  }
  setvbuf(stdout, nullptr, _IONBF, 0);  // a load test that buffers its progress is unreadable
  spdlog::set_level(spdlog::level::off);
  ex_actor::Init(kThreadPoolSize);

  printf("room_bench: mahjong RoomActor load test (pool=%u, seed=%llu, ai_delay=0)\n", kThreadPoolSize,
         static_cast<unsigned long long>(kBaseSeed));
  auto count = std::make_shared<std::atomic<int64_t>>(0);
  g_poker = std::get<0>(ex::sync_wait(ex_actor::Spawn<CounterActor>(count)).value());
  PrintBaselines(g_poker, count);
  if (poke) {
    printf("  (diagnostic: a watchdog thread enqueues one no-op message every 5 ms)\n");
    StartPoker();
  }

  constexpr int kAiDelayMs = 900;  // GAME_DESIGN §5.4 default, for the capacity line only

  std::vector<Shape> shapes {Shape::kBots, Shape::kSolo, Shape::kTable4};
  if (only_oneshape) {
    shapes = {only_shape};
  }
  if (detail) {
    for (Shape shape : shapes) {
      const RunResult result = RunBest(shape, 1, kBaseSeed, kActionTimeoutMs, target_games);
      PrintDetail(shape, result, PacedGameSeconds(result, kAiDelayMs));
    }
  }

  if (scaling) {
    std::vector<int> room_counts;
    for (int rooms = 1; rooms <= max_rooms; rooms *= 2) {
      room_counts.push_back(rooms);
    }
    for (Shape shape : shapes) {
      PrintScaling(shape, room_counts, kBaseSeed, kActionTimeoutMs, target_games);
    }
  }

  g_poker_stop.store(true);
  if (g_stalled_once.load()) {
    // A frozen room cannot be unwound: its handlers are parked and would be resumed by a
    // message that is never coming, so Shutdown() would tear the instance out from under them.
    // The numbers are already on stdout (unbuffered); exit without unwinding.
    printf("\n(run ended after a stalled round: not tearing down the actor world)\n");
    std::fflush(stdout);
    std::_Exit(0);
  }
  ex::sync_wait(mj::ActorSendScope().on_empty());
  ex_actor::Shutdown();
  return 0;
}
