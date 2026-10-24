// reflect-cpp support for json::Value.
//
// ex_actor serializes message arguments with reflect-cpp before they can cross a node boundary
// (`ActorRef::Send`). A json::Value travels as its rendered text: the protocol frames are JSON
// text on the wire anyway, so the round trip loses nothing the game sends. Kept in its own
// header so json.h stays free of reflect-cpp — the pure JSON parser is also built by targets
// that do not link the actor runtime.

#pragma once

#include <optional>
#include <string>
#include <utility>

#include <rfl.hpp>

#include "json.h"

namespace rfl {
template <>
struct Reflector<mahjong::json::Value> {
  using ReflType = std::string;

  static mahjong::json::Value to(const ReflType& text) {
    std::string error;
    std::optional<mahjong::json::Value> parsed = mahjong::json::Value::Parse(text, &error);
    return parsed.has_value() ? std::move(*parsed) : mahjong::json::Value::Null();
  }

  static ReflType from(const mahjong::json::Value& value) { return value.Dump(); }
};
}  // namespace rfl
