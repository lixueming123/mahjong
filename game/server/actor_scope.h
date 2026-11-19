// The scope that owns every fire-and-forget actor message, so shutdown can wait for them.
//
// A suspended handler's coroutine lives outside the actor, and the framework's AsyncDestroy only
// waits for activations, so tearing the world down with a handler parked in `co_await` would reset
// the instance under it. Every non-awaited send goes through this scope; main() drains it before
// ex_actor::Shutdown().
//
// `spawn` is mutex-guarded inside exec::async_scope, so the timer thread may use it too.
// `on_empty()` returns a fresh sender each call, so a test can await it in every TearDown.

#pragma once

#include <exec/async_scope.hpp>

namespace mahjong {

// One instance per process (an inline function's local static).
inline exec::async_scope& ActorSendScope() {
  static exec::async_scope scope;
  return scope;
}

}  // namespace mahjong
