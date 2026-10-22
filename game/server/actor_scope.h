// The scope that owns every fire-and-forget actor message, so shutdown can wait for them.
//
// Why it exists: a handler that suspends keeps its coroutine alive *outside* the actor (the
// framework's `AsyncDestroy` only waits for activation tasks, not for a handler parked in a
// `co_await` — include/ex_actor/internal/actor.h), and the room now suspends for real when it
// fans a broadcast out to four seats. Tearing the actor world down with a handler still parked
// would reset the actor's instance while the coroutine frame still points at it.
//
// So every send that is not awaited goes through this one scope, and `main()` drains it before
// `ex_actor::Shutdown()`:
//
//     gateway.Stop();          // stop dispatching
//     window_timer->Stop();    // stop the timer thread's sends (joins)
//     sync_wait(db.Send<&DbActor::Stop>());     // DB results/records already posted
//     sync_wait(ActorSendScope().on_empty());   // every handler chain has returned
//     ex_actor::Shutdown();
//
// `spawn` is mutex-guarded inside exec::async_scope, so producers on the timer thread (and
// DbActor's pool thread) may use it too. `on_empty()` returns a fresh sender each call, so
// tests can await it in every TearDown.

#pragma once

#include <exec/async_scope.hpp>

namespace mahjong {

// One instance per process (an inline function's local static).
inline exec::async_scope& ActorSendScope() {
  static exec::async_scope scope;
  return scope;
}

}  // namespace mahjong
