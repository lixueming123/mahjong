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

std::string StatsFrame(const StatsSummary& summary) {
  json::Value frame = json::Value::Obj();
  frame.Set("type", json::Value::Str("stats"));
  frame.Set("games", json::Value::Int(summary.games));
  frame.Set("wins", json::Value::Int(summary.wins));
  frame.Set("draws", json::Value::Int(summary.draws));
  frame.Set("net_score", json::Value::Int(summary.net_score));
  json::Value recent = json::Value::Arr();
  for (const StatsEntry& entry : summary.recent) {
    json::Value item = json::Value::Obj();
    item.Set("room_id", json::Value::Str(entry.room_id));
    item.Set("reason", json::Value::Str(entry.reason));
    item.Set("delta_score", json::Value::Int(entry.delta_score));
    item.Set("win", json::Value::Bool(entry.win));
    item.Set("ended_at", json::Value::Int(entry.ended_at));
    recent.Append(std::move(item));
  }
  frame.Set("recent", std::move(recent));
  return frame.Dump();
}

}  // namespace mahjong::protocol
