#include "replay_store.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "actor_scope.h"
#include "base64.h"
#include "db.h"
#include "gzip.h"

namespace mahjong {
namespace {

namespace fs = std::filesystem;

// The room id comes off the wire, so it is checked for shape before it becomes a path: `r-` plus
// 13 lowercase base36 is the only thing lobby.cc ever mints (FormatRoomId), and anything else is
// refused rather than escaped. That makes `../` and absolute paths structurally impossible.
bool IsRoomIdShape(std::string_view id) {
  if (id.size() != 15 || id.substr(0, 2) != "r-") {
    return false;
  }
  for (const char c : id.substr(2)) {
    const bool base36 = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
    if (!base36) {
      return false;
    }
  }
  return true;
}

}  // namespace

ReplayActor::ReplayActor(std::string dir) : dir_(std::move(dir)) {}

stdexec::task<void> ReplayActor::Store(std::string room_id, std::string contents) {
  if (dir_.empty()) {
    co_return;  // recording disabled
  }
  const fs::path directory(dir_);
  std::error_code error;
  fs::create_directories(directory, error);
  if (error) {
    spdlog::error("replay {}: cannot create {}: {}", room_id, directory.string(), error.message());
    co_return;
  }
  // Compressed before touching the disk: a round's file is 1.5 MB of JSON and ~60 KB once gzipped,
  // and the read path (below) hands the same bytes to the wire without re-compressing them.
  const std::string compressed = GzipCompress(contents);
  if (compressed.empty()) {
    spdlog::error("replay {}: gzip failed, the recording is not written", room_id);
    co_return;
  }
  const fs::path target = directory / ReplayRecorder::FileNameFor(room_id);
  // Temp file then rename: a client may ask for this very room while its last round is being
  // written, and a half-written stream is a decode error rather than a clean "not ready".
  const fs::path temp = target.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      spdlog::error("replay {}: cannot open {} for writing", room_id, temp.string());
      co_return;
    }
    out.write(compressed.data(), static_cast<std::streamsize>(compressed.size()));
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

stdexec::task<ReplayLoad> ReplayActor::Load(std::string room_id, uint64_t requester_account_id) {
  ReplayLoad refused;  // found == false: the one answer every refusal collapses into (§4.13)

  // A guest has no history, hence no stats list to click and nothing to claim. Checked before the
  // query so the common refusal costs no round trip.
  if (requester_account_id == 0 || dir_.empty() || !IsRoomIdShape(room_id)) {
    co_return refused;
  }

  // §4.13 asks whether the requester sat in this game. That is a `matches` question now: the file
  // is gzip and its seat table is no longer readable without inflating it, and the same fact is
  // already in the database, written by this very account's shard at the end of every round.
  DbService& db = GetDbService();
  if (db.IsEmpty()) {
    // No storage means no way to answer the authorization question, and guessing "allowed" would
    // hand the recording to anyone who has the room id. Same refusal as "no such replay".
    spdlog::warn("replay {}: account storage is unavailable, refusing the request", room_id);
    co_return refused;
  }
  const ReplayAccessOutcome access = co_await db.ReplayAccess(room_id, requester_account_id);
  if (!access.ok || !access.allowed) {
    co_return refused;
  }
  co_return LoadFile(room_id);
}

ReplayLoad ReplayActor::LoadFile(const std::string& room_id) const {
  ReplayLoad result;  // found == false
  // The current name first, then the pre-v2.6 one: a server that was upgraded mid-deployment has
  // both kinds on disk, and a recording must not become unreadable just because nobody ran a
  // migration over it. `FileNameFor` is what Store writes; the legacy name is read-only.
  std::ifstream in(fs::path(dir_) / ReplayRecorder::FileNameFor(room_id), std::ios::binary);
  if (!in) {
    in.clear();
    in.open(fs::path(dir_) / ReplayRecorder::LegacyFileNameFor(room_id), std::ios::binary);
  }
  if (!in) {
    return result;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  std::string bytes = buffer.str();
  if (bytes.empty()) {
    return result;
  }

  // A recording written before the store compressed is still served, as plain text: the client
  // distinguishes the two by the `encoding` key (§5.18), so an old file needs no migration and a
  // new client keeps playing recordings made by the previous build.
  if (!LooksGzip(bytes)) {
    spdlog::info("replay {}: serving an uncompressed recording", room_id);
    result.found = true;
    result.payload = std::move(bytes);
    return result;
  }
  // gzip + base64 rather than the inflated text: base64 is what makes a binary payload sliceable
  // into §5.18 frames, and 40 KB of base64 replaces 1.5 MB of JSON on the wire.
  result.found = true;
  result.encoding = protocol::kReplayEncodingGzip;
  result.payload = Base64Encode(bytes);
  if (result.payload.empty()) {
    spdlog::error("replay {}: base64 failed, refusing the request", room_id);
    result.found = false;
  }
  return result;
}

}  // namespace mahjong
