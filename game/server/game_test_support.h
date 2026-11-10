// Frame helpers shared by the **game** tests (classic and riichi alike). Test-only; nothing in
// the server includes it.
//
// It lives next to game.h rather than under classic/ because none of it knows about any rules:
// it reads JSON frames, pulls tiles out of them, and folds a byte stream into an FNV-1a hash.
// Both games' transcripts are hashed with the same function, which is what makes the two
// goldens comparable rather than merely self-consistent.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "json.h"
#include "mahjong_rules.h"
#include "protocol.h"

namespace mahjong::game_test {

inline protocol::FrameType TypeOf(const json::Value& frame) {
  const json::Value* type = frame.Find("type");
  if (type == nullptr || !type->IsInteger()) {
    return protocol::FrameType::kInvalid;
  }
  return protocol::ParseFrameType(type->AsInt()).value_or(protocol::FrameType::kInvalid);
}

inline int64_t IntOf(const json::Value& frame, const std::string& key) {
  const json::Value* value = frame.Find(key);
  return value != nullptr && value->IsNumber() ? value->AsInt() : 0;
}

inline std::string StrOf(const json::Value& frame, const std::string& key) {
  const json::Value* value = frame.Find(key);
  return value != nullptr && value->IsString() ? value->AsString() : std::string();
}

inline const json::Value* OptionOf(const json::Value& request, protocol::Action action) {
  const json::Value* options = request.Find("options");
  if (options == nullptr || !options->IsArray()) {
    return nullptr;
  }
  for (const json::Value& option : options->AsArray()) {
    if (IntOf(option, "action") == protocol::WireOf(action)) {
      return &option;
    }
  }
  return nullptr;
}

inline std::vector<Tile> TilesOf(const json::Value& frame, const std::string& key) {
  std::vector<Tile> tiles;
  const json::Value* array = frame.Find(key);
  if (array == nullptr || !array->IsArray()) {
    return tiles;
  }
  for (const json::Value& entry : array->AsArray()) {
    if (std::optional<Tile> tile = ParseTile(entry.AsString()); tile.has_value()) {
      tiles.push_back(*tile);
    }
  }
  return tiles;
}

// FNV-1a 64: stable across runs, builds and standard libraries, which std::hash is not.
inline uint64_t Fnv1a64(std::string_view text) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (const char byte : text) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 0x100000001b3ull;
  }
  return hash;
}

}  // namespace mahjong::game_test
