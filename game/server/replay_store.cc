#include "replay_store.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "actor_scope.h"
#include "player_actor.h"

namespace mahjong {

ReplayActor::ReplayActor(std::string dir) : dir_(std::move(dir)) {}

stdexec::task<void> ReplayActor::Store(std::string room_id, std::string contents) {
  if (dir_.empty()) {
    co_return;  // recording disabled
  }
  namespace fs = std::filesystem;
  const fs::path directory(dir_);
  std::error_code error;
  fs::create_directories(directory, error);
  if (error) {
    spdlog::error("replay {}: cannot create {}: {}", room_id, directory.string(), error.message());
    co_return;
  }
  const fs::path target = directory / ReplayRecorder::FileNameFor(room_id);
  // Temp file then rename: a client may ask for this very room while its last round is being
  // written, and a half-written JSON is a parse error rather than a clean "not ready".
  const fs::path temp = target.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      spdlog::error("replay {}: cannot open {} for writing", room_id, temp.string());
      co_return;
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!out) {
      spdlog::error("replay {}: write to {} failed", room_id, temp.string());
      out.close();
      fs::remove(temp, error);
      co_return;
    }
  }
  fs::rename(temp, target, error);
  if (error) {
    spdlog::error("replay {}: cannot rename {} -> {}: {}", room_id, temp.string(), target.string(), error.message());
    fs::remove(temp, error);
  }
  co_return;
}

stdexec::task<void> ReplayActor::Load(std::string room_id, uint64_t requester_account_id,
                                      ex_actor::ActorRef<PlayerActor> reply) {
  bool ok = false;
  std::string data = LoadFile(room_id, requester_account_id, &ok);
  if (reply.IsEmpty()) {
    co_return;
  }
  ActorSendScope().spawn(
      reply.Send<&PlayerActor::Dispatch>(PlayerEnvelope::OnReplayResult(std::move(room_id), ok, std::move(data))));
  co_return;
}

std::string ReplayActor::LoadFile(const std::string& room_id, uint64_t requester_account_id, bool* ok) const {
  *ok = false;
  if (dir_.empty()) {
    return {};
  }
  // The room id comes off the wire, so it is checked for shape before it becomes a path: `r-` plus
  // 13 lowercase base36 is the only thing lobby.cc ever mints (FormatRoomId), and anything else is
  // refused rather than escaped. That makes `../` and absolute paths structurally impossible.
  std::string_view id = room_id;
  if (id.size() != 15 || id.substr(0, 2) != "r-") {
    return {};
  }
  for (const char c : id.substr(2)) {
    const bool base36 = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
    if (!base36) {
      return {};
    }
  }
  std::ifstream in(std::filesystem::path(dir_) / ReplayRecorder::FileNameFor(room_id), std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  std::string text = buffer.str();
  if (text.empty()) {
    return {};
  }

  // Authorization (§4.13): the requester must have held a seat in this game. Parsed from the
  // file rather than a database index, so a replay is self-contained and needs no extra table to
  // be authoritative about who may read it.
  const std::optional<json::Value> replay = json::Value::Parse(text, nullptr);
  if (!replay.has_value()) {
    spdlog::warn("replay {}: unparseable file, refusing the request", room_id);
    return {};
  }
  const json::Value* seats = replay->Find("seats");
  const json::Value* format = replay->Find("format");
  if (format == nullptr || format->AsString() != "mahjong-replay" || seats == nullptr || !seats->IsArray()) {
    spdlog::warn("replay {}: not a replay file, refusing the request", room_id);
    return {};
  }
  bool authorized = false;
  for (const json::Value& seat : seats->AsArray()) {
    const json::Value* account = seat.Find("account_id");
    const json::Value* is_bot = seat.Find("is_bot");
    if (account == nullptr || !account->IsInteger()) {
      continue;
    }
    if (is_bot != nullptr && is_bot->AsBool()) {
      continue;
    }
    if (static_cast<uint64_t>(account->AsInt()) == requester_account_id && requester_account_id != 0) {
      authorized = true;
      break;
    }
  }
  if (!authorized) {
    // Same answer as "no such room": telling a stranger that the room exists would leak that a
    // particular id was played, and §4.13 asks for one code for both.
    return {};
  }
  *ok = true;
  return text;
}

}  // namespace mahjong
