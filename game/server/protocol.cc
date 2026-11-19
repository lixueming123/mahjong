#include "protocol.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace mahjong::protocol {

std::string ErrorFrame(ErrorCode code, std::string_view message) {
  json::Value frame = json::Value::Obj();
  frame.Set("type", json::Value::Int(WireOf(FrameType::kError)));
  frame.Set("code", json::Value::Int(WireOf(code)));
  frame.Set("message", json::Value::Str(std::string(message)));
  return frame.Dump();
}

std::string ErrorFrame(ErrorCode code, std::string_view message, json::Value detail) {
  json::Value frame = json::Value::Obj();
  frame.Set("type", json::Value::Int(WireOf(FrameType::kError)));
  frame.Set("code", json::Value::Int(WireOf(code)));
  frame.Set("message", json::Value::Str(std::string(message)));
  frame.Set("detail", std::move(detail));
  return frame.Dump();
}

std::string LoginOkFrame(std::string_view player_id, std::string_view name, std::optional<bool> in_room) {
  json::Value frame = json::Value::Obj();
  frame.Set("type", json::Value::Int(WireOf(FrameType::kLoginOk)));
  frame.Set("player_id", json::Value::Str(std::string(player_id)));
  frame.Set("name", json::Value::Str(std::string(name)));
  // Only account logins report whether the player is already in a room.
  // The guest path leaves the key out, keeping its frozen frame shape.
  if (in_room.has_value()) {
    frame.Set("in_room", json::Value::Bool(*in_room));
  }
  return frame.Dump();
}

std::string PongFrame() {
  json::Value frame = json::Value::Obj();
  frame.Set("type", json::Value::Int(WireOf(FrameType::kPong)));
  return frame.Dump();
}

namespace {

json::Value RecentJson(const StatsSummary& summary) {
  json::Value recent = json::Value::Arr();
  for (const StatsEntry& entry : summary.recent) {
    json::Value item = json::Value::Obj();
    item.Set("room_id", json::Value::Str(entry.room_id));
    item.Set("reason", json::Value::Int(WireOf(entry.reason)));
    item.Set("mode", json::Value::Int(WireOf(entry.mode)));
    item.Set("delta_score", json::Value::Int(entry.delta_score));
    item.Set("win", json::Value::Bool(entry.win));
    item.Set("ended_at", json::Value::Int(entry.ended_at));
    // Only written when true, so a client that predates replays sees the exact frame
    // it saw before (this is the same "omit when false" rule state_sync uses for `autoplay`).
    if (entry.has_replay) {
      item.Set("has_replay", json::Value::Bool(true));
    }
    recent.Append(std::move(item));
  }
  return recent;
}

// 一种玩法的完整统计对象（`classic` / `riichi` 子对象共用这个形状）
json::Value SummaryJson(const StatsSummary& summary) {
  json::Value obj = json::Value::Obj();
  obj.Set("games", json::Value::Int(summary.games));
  obj.Set("wins", json::Value::Int(summary.wins));
  obj.Set("draws", json::Value::Int(summary.draws));
  obj.Set("net_score", json::Value::Int(summary.net_score));
  obj.Set("recent", RecentJson(summary));
  return obj;
}

}  // namespace

std::string StatsFrame(const StatsSnapshot& snapshot) {
  json::Value frame = json::Value::Obj();
  frame.Set("type", json::Value::Int(WireOf(FrameType::kStats)));
  // 兼容层：顶层四字段 + recent 继续表示 classic（老客户端读这些键，帧形状不变）
  frame.Set("games", json::Value::Int(snapshot.classic.games));
  frame.Set("wins", json::Value::Int(snapshot.classic.wins));
  frame.Set("draws", json::Value::Int(snapshot.classic.draws));
  frame.Set("net_score", json::Value::Int(snapshot.classic.net_score));
  frame.Set("recent", RecentJson(snapshot.classic));
  // 新增：按玩法分开的完整统计
  frame.Set("classic", SummaryJson(snapshot.classic));
  frame.Set("riichi", SummaryJson(snapshot.riichi));
  return frame.Dump();
}

std::string StatsFrame(const StatsSummary& summary) {
  StatsSnapshot snapshot;
  snapshot.classic = summary;
  return StatsFrame(snapshot);
}

// ---- replay ---------------------------------------------------------------------------

std::string ReplayFrame(std::string_view room_id, int chunk, int chunks, std::string_view encoding,
                        std::string_view data) {
  json::Value frame = json::Value::Obj(6);
  frame.Set("type", json::Value::Int(WireOf(FrameType::kReplay)));
  frame.Set("room_id", json::Value::Str(std::string(room_id)));
  frame.Set("chunk", json::Value::Int(chunk));
  frame.Set("chunks", json::Value::Int(chunks));
  // An empty encoding means the joined `data` IS the replay text (a file written before
  // the store compressed), so the key is omitted then and the old frame shape is unchanged.
  if (!encoding.empty()) {
    frame.Set("encoding", json::Value::Str(std::string(encoding)));
  }
  frame.Set("data", json::Value::Str(std::string(data)));
  return frame.Dump();
}

std::string ReplayErrorFrame(std::string_view room_id, ErrorCode code) {
  // The failure shape carries no chunk/chunks/data, so a client never has to guess
  // whether an empty `data` means "failed" or "an empty file".
  json::Value frame = json::Value::Obj(3);
  frame.Set("type", json::Value::Int(WireOf(FrameType::kReplay)));
  frame.Set("room_id", json::Value::Str(std::string(room_id)));
  frame.Set("error", json::Value::Int(WireOf(code)));
  return frame.Dump();
}

std::vector<std::string> SplitReplayFrames(std::string_view room_id, std::string_view encoding,
                                           std::string_view payload, size_t max_bytes) {
  if (max_bytes == 0) {
    max_bytes = kMaxReplayChunkBytes;
  }
  // |cuts| holds only the END offset of each slice, so its size IS the chunk count. It used to
  // start with a 0 sentinel and be indexed as `begin = cuts[i - 1]`, which emitted one empty
  // `chunk = 0` frame per answer and over-counted `chunks` by one.
  //
  // Cut points are byte offsets, but a cut may only land on a UTF-8 lead byte: a frame whose
  // `data` held half a character would make the whole NDJSON line invalid for the client, and a
  // legacy (uncompressed) replay text contains player names. Base64 is pure ASCII, so the walk
  // never moves for a v2.6 payload — it stays because both encodings share this splitter.
  std::vector<size_t> cuts;
  size_t offset = 0;
  while (offset < payload.size()) {
    size_t end = std::min(offset + max_bytes, payload.size());
    // Walk back to the start of the character |end| would land inside. A continuation byte has
    // the top two bits 10; back up over at most three of them.
    while (end < payload.size() && end > offset && (static_cast<unsigned char>(payload[end]) & 0xC0) == 0x80) {
      --end;
    }
    if (end == offset) {
      end = std::min(offset + max_bytes, payload.size());  // pathological input: never loop forever
      while (end < payload.size() && (static_cast<unsigned char>(payload[end]) & 0xC0) == 0x80) {
        ++end;
      }
    }
    cuts.push_back(end);
    offset = end;
  }
  // An empty payload still needs one frame: `chunks` must be >= 1, and the client treats a
  // zero-length answer as a protocol error otherwise.
  if (cuts.empty()) {
    cuts.push_back(0);
  }
  const int chunks = static_cast<int>(cuts.size());
  std::vector<std::string> frames;
  frames.reserve(cuts.size());
  for (size_t index = 0; index < cuts.size(); ++index) {
    const size_t begin = index == 0 ? 0 : cuts[index - 1];
    const size_t end = cuts[index];
    frames.push_back(
        ReplayFrame(room_id, static_cast<int>(index), chunks, encoding, payload.substr(begin, end - begin)));
  }
  return frames;
}

}  // namespace mahjong::protocol
