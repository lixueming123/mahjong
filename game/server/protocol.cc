#include "protocol.h"

#include <string>
#include <utility>

namespace mahjong::protocol {

std::string ErrorFrame(std::string_view code, std::string_view message) {
  json::Value frame = json::Value::Obj();
  frame.Set("type", json::Value::Str("error"));
  frame.Set("code", json::Value::Str(std::string(code)));
  frame.Set("message", json::Value::Str(std::string(message)));
  return frame.Dump();
}

std::string ErrorFrame(std::string_view code, std::string_view message, json::Value detail) {
  json::Value frame = json::Value::Obj();
  frame.Set("type", json::Value::Str("error"));
  frame.Set("code", json::Value::Str(std::string(code)));
  frame.Set("message", json::Value::Str(std::string(message)));
  frame.Set("detail", std::move(detail));
  return frame.Dump();
}

std::string LoginOkFrame(std::string_view player_id, std::string_view name) {
  json::Value frame = json::Value::Obj();
  frame.Set("type", json::Value::Str("login_ok"));
  frame.Set("player_id", json::Value::Str(std::string(player_id)));
  frame.Set("name", json::Value::Str(std::string(name)));
  return frame.Dump();
}

std::string PongFrame() {
  json::Value frame = json::Value::Obj();
  frame.Set("type", json::Value::Str("pong"));
  return frame.Dump();
}

namespace {

json::Value RecentJson(const StatsSummary& summary) {
  json::Value recent = json::Value::Arr();
  for (const StatsEntry& entry : summary.recent) {
    json::Value item = json::Value::Obj();
    item.Set("room_id", json::Value::Str(entry.room_id));
    item.Set("reason", json::Value::Str(entry.reason));
    item.Set("mode", json::Value::Str(entry.mode));
    item.Set("delta_score", json::Value::Int(entry.delta_score));
    item.Set("win", json::Value::Bool(entry.win));
    item.Set("ended_at", json::Value::Int(entry.ended_at));
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
  frame.Set("type", json::Value::Str("stats"));
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

}  // namespace mahjong::protocol
