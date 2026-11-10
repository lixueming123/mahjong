// RoomActorFor<GameT>: the only concrete room. A game plugs in here and nowhere else.
//
// All this class does is own the Game and hand the framework's spawn hook to the shell — which
// is the whole point: adding a game is a Game implementation, one `using` alias and one registry
// entry (game_registry.h), never an edit to the state machine.
//
// `game()` returns GameT& rather than Game& on purpose (covariant override): a test that holds a
// `RoomActorFor<ClassicMahjongGame>` can read the game's own introspection (seq, hands, phase)
// without a cast, while the shell still only ever sees `Game&`.

#pragma once

#include <utility>

#include "ex_actor/api.h"
#include "game.h"
#include "room_actor.h"

namespace mahjong {

template <class GameT>
class RoomActorFor final : public RoomActor {
 public:
  using RoomActor::RoomActor;

  // ex_actor hands every actor its own ref right after construction; the shell needs it to post
  // a deadline back to itself. It is forwarded, not stored and re-derived, because the shell
  // holds the narrowed `ActorRef<RoomEndpoint>` — the concrete type stops here.
  void ExActorOnSpawned(const ex_actor::ActorRuntimeInfo<RoomActorFor>& info) {
    BindRuntime(ex_actor::ActorRef<RoomEndpoint>(info.self_actor_ref));
  }

  GameT& game() override { return game_; }

 private:
  GameT game_;
};

}  // namespace mahjong
