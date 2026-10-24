// Probe: can an actor's message entry live in a CRTP base class?
//
// The design this checks (game/server "one door per actor"): the framework may only ever enter
// an actor through a single inherited `Entries`-like method, which takes the per-actor gate
// before running the real, *private* handler. If that works, every actor needs two lines
// (inherit + implement the private handler) instead of a hand-written entry.
//
// What has to be true for this to be usable:
//   1. `&Derived::InheritedEntry` is accepted by `ActorRef<Derived>::Send/SendLocal`;
//   2. the framework runs it on the right instance — it casts the instance pointer to the
//      *method pointer's* class (the base), so this depends on the base subobject sitting at
//      offset 0 (single, non-virtual inheritance);
//   3. the real handler stays private, so `SendLocal<&Derived::Handle>` cannot bypass the gate;
//   4. no `kActorMethods` tuple is needed on either class for the local path;
//   5. the entry may be a coroutine that suspends inside the handler and still return a value.

#include <concepts>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "ex_actor/api.h"

namespace ex = stdexec;

namespace {

class Peer {
 public:
  int Ping() { return 42; }
};

// The entry the framework sees. Deliberately a base-class member, reached as
// `&CounterActor::Entries`.
template <class Impl, class... Args>
class SerializedEntry {
 public:
  stdexec::task<int> Entries(Args... args) {
    ++entered_;
    co_return co_await static_cast<Impl*>(this)->Handle(std::move(args)...);
  }

  int entered() const { return entered_; }

 private:
  // Per-instance, like the real gate: two actors must not share it.
  int entered_ = 0;
};

class CounterActor : public SerializedEntry<CounterActor, int> {
 public:
  explicit CounterActor(int start) : count_(start) {}

  int GetCount() const { return count_; }
  int EnteredCount() const { return entered(); }

 private:
  // The real handler. Private + friend, so it cannot be called through the framework and the
  // gate can never be bypassed. Awaits another actor *inside* the handler, i.e. it suspends
  // while the entry is on the stack.
  stdexec::task<int> Handle(int delta) {
    if (peer_.IsEmpty()) {
      peer_ = co_await ex_actor::Spawn<Peer>();
    }
    int pong = co_await peer_.SendLocal<&Peer::Ping>();
    count_ += delta;
    co_return count_ + pong;
  }

  friend class SerializedEntry<CounterActor, int>;

  int count_ = 0;
  ex_actor::ActorRef<Peer> peer_;
};

// (1) A base-class member function pointer is invocable through the derived type, which is
// what `SendLocal`'s static_assert checks.
static_assert(std::is_invocable_v<decltype(&CounterActor::Entries), CounterActor*, int>);

// (3) `Handle` must not be reachable from the outside.
template <class T>
concept PubliclyReachableHandle = requires(T& actor) { actor.Handle(1); };
static_assert(!PubliclyReachableHandle<CounterActor>, "the private handler leaked; the gate could be bypassed");

TEST(SerializedEntryProbeTest, InheritedEntryRunsThroughSendLocal) {
  auto coroutine = []() -> stdexec::task<void> {
    auto actor = co_await ex_actor::Spawn<CounterActor>(10);
    // 10 + 5 + 42: the entry ran on the right instance and the handler's suspension worked.
    int result = co_await actor.SendLocal<&CounterActor::Entries>(5);
    EXPECT_EQ(result, 57);
    EXPECT_EQ(co_await actor.SendLocal<&CounterActor::GetCount>(), 15);
    EXPECT_EQ(co_await actor.SendLocal<&CounterActor::EnteredCount>(), 1);
  };
  ex_actor::Init(/*thread_pool_size=*/2);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(SerializedEntryProbeTest, InheritedEntryRunsThroughGenericSend) {
  auto coroutine = []() -> stdexec::task<void> {
    auto actor = co_await ex_actor::Spawn<CounterActor>(0);
    int result = co_await actor.Send<&CounterActor::Entries>(5);
    EXPECT_EQ(result, 47);
    EXPECT_EQ(co_await actor.Send<&CounterActor::GetCount>(), 5);
  };
  ex_actor::Init(/*thread_pool_size=*/2);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(SerializedEntryProbeTest, EntryStateIsPerActorInstance) {
  auto coroutine = []() -> stdexec::task<void> {
    auto first = co_await ex_actor::Spawn<CounterActor>(0);
    auto second = co_await ex_actor::Spawn<CounterActor>(0);
    EXPECT_EQ(co_await first.SendLocal<&CounterActor::Entries>(1), 43);
    EXPECT_EQ(co_await first.SendLocal<&CounterActor::Entries>(1), 44);
    EXPECT_EQ(co_await second.SendLocal<&CounterActor::Entries>(1), 43);
    EXPECT_EQ(co_await first.SendLocal<&CounterActor::EnteredCount>(), 2);
    EXPECT_EQ(co_await second.SendLocal<&CounterActor::EnteredCount>(), 1);
  };
  ex_actor::Init(/*thread_pool_size=*/2);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

}  // namespace
