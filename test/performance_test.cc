#include <iostream>
#include <chrono>
#include <vector>
#include <atomic>
#include <gtest/gtest.h>
#include "ex_actor/api.h"
#include "ex_actor/internal/actor_registry.h"
#include "exec/async_scope.hpp"

namespace ex = stdexec;

class PingActor {
 public:
  PingActor() : count_(0) {}

  void Ping() { count_++; }
  void PingWithReply(ex_actor::ActorRef<PingActor> self_ref) { count_++; }
  int GetCount() const { return count_; }
  int AddAndGet(int value) { count_ += value; return count_; }

 private:
  int count_;
};

class PerfTest : public testing::Test {
 protected:
  void SetUp() override {
    auto pool = std::make_shared<ex_actor::WorkStealingThreadPool>(1);
    ex_actor::Init(pool->GetScheduler());
    ex_actor::HoldResource(pool);
  }

  void TearDown() override {
    ex_actor::Shutdown();
  }
};

TEST_F(PerfTest, ActorCreation) {
  auto test = []() -> exec::task<void> {
    auto start = std::chrono::high_resolution_clock::now();

    const int kActorCount = 10000;
    for (int i = 0; i < kActorCount; ++i) {
      co_await ex_actor::Spawn<PingActor>();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "[Perf] Created " << kActorCount << " actors in " << duration << " ms\n";
    std::cout << "[Perf] Throughput: " << (kActorCount * 1000.0 / duration) << " actors/sec\n";

    co_return;
  };

  ex::sync_wait(test());
}

TEST_F(PerfTest, MessagePassingLatency) {
  auto test = []() -> exec::task<void> {
    auto actor = co_await ex_actor::Spawn<PingActor>();

    const int kIterations = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < kIterations; ++i) {
      co_await actor.template Send<&PingActor::Ping>();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    std::cout << "[Perf] Sent " << kIterations << " messages in " << duration << " us\n";
    std::cout << "[Perf] Latency: " << (duration * 1.0 / kIterations) << " us/msg\n";

    co_return;
  };

  ex::sync_wait(test());
}

TEST_F(PerfTest, MessageThroughput) {
  auto test = []() -> exec::task<void> {
    auto actor = co_await ex_actor::Spawn<PingActor>();

    const int kIterations = 100000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < kIterations; ++i) {
      co_await actor.template Send<&PingActor::Ping>();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "[Perf] Throughput test: " << kIterations << " messages in " << duration << " ms\n";
    std::cout << "[Perf] Throughput: " << (kIterations * 1000.0 / duration) << " msgs/sec\n";

    co_return;
  };

  ex::sync_wait(test());
}

TEST_F(PerfTest, ConcurrentActors) {
  auto test = []() -> exec::task<void> {
    const int kActorCount = 100;
    const int kIterations = 1000;

    std::vector<ex_actor::ActorRef<PingActor>> actors;
    actors.reserve(kActorCount);

    for (int i = 0; i < kActorCount; ++i) {
      actors.push_back(co_await ex_actor::Spawn<PingActor>());
    }

    auto start = std::chrono::high_resolution_clock::now();

    for (int round = 0; round < kIterations; ++round) {
      for (int i = 0; i < kActorCount; ++i) {
        co_await actors[i].template Send<&PingActor::Ping>();
      }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    int total_messages = kActorCount * kIterations;
    std::cout << "[Perf] " << kActorCount << " concurrent actors, " << kIterations << " rounds\n";
    std::cout << "[Perf] Total messages: " << total_messages << " in " << duration << " ms\n";
    std::cout << "[Perf] Throughput: " << (total_messages * 1000.0 / duration) << " msgs/sec\n";

    co_return;
  };

  ex::sync_wait(test());
}

TEST_F(PerfTest, RequestResponse) {
  auto test = []() -> exec::task<void> {
    auto actor = co_await ex_actor::Spawn<PingActor>();

    const int kIterations = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < kIterations; ++i) {
      int result = co_await actor.template Send<&PingActor::AddAndGet>(1);
      (void)result;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    std::cout << "[Perf] Request-Response: " << kIterations << " calls in " << duration << " us\n";
    std::cout << "[Perf] Latency: " << (duration * 1.0 / kIterations) << " us/call\n";

    co_return;
  };

  ex::sync_wait(test());
}

TEST_F(PerfTest, ManyToMany) {
  auto test = []() -> exec::task<void> {
    const int kSenderCount = 50;
    const int kReceiverCount = 50;
    const int kMessagesPerSender = 100;

    std::vector<ex_actor::ActorRef<PingActor>> receivers;
    for (int i = 0; i < kReceiverCount; ++i) {
      receivers.push_back(co_await ex_actor::Spawn<PingActor>());
    }

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < kSenderCount; ++i) {
      auto sender = co_await ex_actor::Spawn<PingActor>();
      for (int j = 0; j < kMessagesPerSender; ++j) {
        auto& receiver = receivers[j % kReceiverCount];
        co_await sender.template Send<&PingActor::Ping>();
        co_await receiver.template Send<&PingActor::Ping>();
      }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    int total_messages = kSenderCount * kMessagesPerSender * 2;
    std::cout << "[Perf] Many-to-Many: " << kSenderCount << " senders -> " << kReceiverCount << " receivers\n";
    std::cout << "[Perf] Total messages: " << total_messages << " in " << duration << " ms\n";
    std::cout << "[Perf] Throughput: " << (total_messages * 1000.0 / duration) << " msgs/sec\n";

    co_return;
  };

  ex::sync_wait(test());
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
