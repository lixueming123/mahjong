// Probe: awaiting a *nested* stdexec::task from a coroutine handler that was dispatched as an
// actor message.
//
// Written while chasing a SIGTRAP in the mahjong sample, where a coroutine handler awaited a
// private helper coroutine. All three shapes below turn out to be FINE — the real cause was
// elsewhere and worth remembering:
//
//   **A function declared `stdexec::task<T>` that contains no coroutine keyword is not a coroutine.**
//   In the sample, moving the gate up into `Dispatch` left one helper (`LobbyActor::SetSelfRef`)
//   with no `co_await` at all, so it silently stopped being a coroutine, fell off the end of a
//   non-void function and handed back an uninitialized task. `co_await`ing that traps in
//   `~basic_task()` (it destroys a garbage coroutine handle), which looks exactly like a
//   framework problem from the crash log. Keep at least one `co_await`/`co_return` in every
//   task-returning function, or give it a `void` return type.
//
// These three cases stay as regression tests for the nested-await pattern itself:
//
//   A. the nested helper never suspends
//   B. the nested helper suspends once
//   C. a plain (non-coroutine) helper
//
// Everything here uses only the public API.

#include <gtest/gtest.h>

#include "ex_actor/api.h"

namespace ex = stdexec;

namespace {

class NestedProbe {
 public:
  // A. never suspends.
  stdexec::task<void> NestedThatNeverSuspends() { co_return; }

  // B. suspends once (a real await) before completing.
  stdexec::task<void> NestedThatSuspends() {
    co_await ex::schedule(ex::inline_scheduler {});
    co_return;
  }

  // C. not a coroutine at all.
  void PlainHelper() { ++calls_; }

  stdexec::task<void> DispatchNeverSuspending() {
    co_await NestedThatNeverSuspends();
    ++calls_;
  }

  stdexec::task<void> DispatchSuspending() {
    co_await NestedThatSuspends();
    ++calls_;
  }

  stdexec::task<void> DispatchPlain() {
    PlainHelper();
    co_return;
  }

  int calls() const { return calls_; }

 private:
  int calls_ = 0;
};

TEST(NestedAwaitProbeTest, PlainHelperInsideADispatchedCoroutine) {
  auto coroutine = []() -> stdexec::task<void> {
    auto actor = co_await ex_actor::Spawn<NestedProbe>();
    co_await actor.SendLocal<&NestedProbe::DispatchPlain>();
    EXPECT_EQ(co_await actor.SendLocal<&NestedProbe::calls>(), 1);
  };
  ex_actor::Init(/*thread_pool_size=*/2);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(NestedAwaitProbeTest, SuspendingNestedTaskInsideADispatchedCoroutine) {
  auto coroutine = []() -> stdexec::task<void> {
    auto actor = co_await ex_actor::Spawn<NestedProbe>();
    co_await actor.SendLocal<&NestedProbe::DispatchSuspending>();
    EXPECT_EQ(co_await actor.SendLocal<&NestedProbe::calls>(), 1);
  };
  ex_actor::Init(/*thread_pool_size=*/2);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(NestedAwaitProbeTest, NonSuspendingNestedTaskInsideADispatchedCoroutine) {
  auto coroutine = []() -> stdexec::task<void> {
    auto actor = co_await ex_actor::Spawn<NestedProbe>();
    co_await actor.SendLocal<&NestedProbe::DispatchNeverSuspending>();
    EXPECT_EQ(co_await actor.SendLocal<&NestedProbe::calls>(), 1);
  };
  ex_actor::Init(/*thread_pool_size=*/2);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

}  // namespace
