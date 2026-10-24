#include <cassert>
#include "ex_actor/api.h"
struct Counter { int Add(int x) { return count += x; } int count = 0; };
stdexec::task<void> Main() {
  ex_actor::Init(/*thread_pool_size=*/2);
  ex_actor::ActorRef<Counter> actor = co_await ex_actor::Spawn<Counter>();
  int r = co_await actor.SendLocal<&Counter::Add>(41);
  assert(r == 41);
  ex_actor::Shutdown();
}
int main() { stdexec::sync_wait(Main()); }
