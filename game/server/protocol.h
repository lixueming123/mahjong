// Wire protocol constants and frame builders.
//
// Every value in this header is copied from the frozen spec `game/docs/PROTOCOL.md`.
// Implementations MUST NOT invent message types, fields or error codes (§0.1): if a
// needed value is missing, raise a requirement review instead of adding it here.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "json.h"
#include "mahjong_rules.h"
#include "utf8.h"

namespace mahjong::protocol {

// ---- §1.1 transport -------------------------------------------------------------------
inline constexpr uint16_t kDefaultPort = 9527;
inline constexpr char kListenAddress[] = "127.0.0.1";
inline constexpr int kListenBacklog = 128;

// ---- §1.2 / §1.7 framing and quotas ---------------------------------------------------
inline constexpr size_t kMaxFrameBytes = 65536;
inline constexpr int kMaxBadFrames = 10;
inline constexpr int kMaxIllegalOps = 10;
inline constexpr int kMaxInvalidNames = 3;
inline constexpr int kRateLimitPerSecond = 100;
inline constexpr int kRateLimitStormPerSecond = 500;
// Frames buffered without a terminator before the connection is dropped (§1.2.3).
inline constexpr size_t kMaxUnframedBytes = 65536;
// How long the gateway keeps a connection open to flush a final error frame before
// closing it (PROTOCOL §1.2.2 asks for a best-effort FRAME_TOO_LARGE before closing).
inline constexpr int kCloseFlushTimeoutMs = 300;

// ---- §1.6 heartbeat -------------------------------------------------------------------
inline constexpr int kIdleTimeoutMs = 20000;
inline constexpr int kPollTimeoutMs = 100;
// Upper bound of bytes read from one socket per poll iteration, so a flooding client
// cannot stall the single-threaded loop.
inline constexpr size_t kMaxReadBytesPerIteration = 256 * 1024;

// ---- §1.8 startup parameters ----------------------------------------------------------
inline constexpr int kDefaultActionTimeoutMs = 15000;
inline constexpr int kMinActionTimeoutMs = 1000;
inline constexpr int kMaxActionTimeoutMs = 60000;

// ---- §4 client frame types ------------------------------------------------------------
// The "type" field of every client frame. Declared here because the routing table
// (player_actor.cc) plus its tests are the two places that must agree on them: a typo in a
// string literal there would silently become an unknown frame type at runtime.
inline constexpr char kFrameLogin[] = "login";
inline constexpr char kFrameRegister[] = "register";
inline constexpr char kFramePing[] = "ping";
inline constexpr char kFrameStatsRequest[] = "stats_request";
inline constexpr char kFrameListRooms[] = "list_rooms";
inline constexpr char kFrameCreateRoom[] = "create_room";
inline constexpr char kFrameJoinRoom[] = "join_room";
inline constexpr char kFrameReady[] = "ready";
inline constexpr char kFrameSync[] = "sync";
inline constexpr char kFrameDiscard[] = "discard";
inline constexpr char kFrameAction[] = "action";

// The same list as a range, so the routing table can assert its own completeness at compile
// time: adding a frame type here without a row in the routing table (kFrameRoutes in
// player_agent.h, asserted by the static_assert in player_actor.cc) is a build error rather
// than a frame that silently answers "unknown frame type" in production.
inline constexpr std::string_view kClientFrameTypes[] = {
    kFrameLogin,   kFrameRegister,  kFramePing,   kFrameStatsRequest, kFrameListRooms, kFrameCreateRoom,
    kFrameJoinRoom, kFrameReady,    kFrameSync,   kFrameDiscard,      kFrameAction,
};

// ---- §3.2 server frame types ----------------------------------------------------------
// Copied from PROTOCOL.md §3.2 / §5.17 (v1.10). Deliberately NOT in kClientFrameTypes:
// the lobby broadcasts it, no client may send it. The older server frames still spell
// their type inline next to their builder; this one is named because two call sites emit it.
inline constexpr char kFrameRoomUpdate[] = "room_update";

// ---- §7.1 error codes (exactly these 14; STALE_SEQ is deliberately absent) -------------
// The last three come from ACCOUNT_DESIGN.md §3.5 (PROTOCOL.md §7.1 was amended from 11 to
// 14 codes); the semantics and thresholds of the original 11 are unchanged.
inline constexpr char kCodeInvalidName[] = "INVALID_NAME";
inline constexpr char kCodeNotLoggedIn[] = "NOT_LOGGED_IN";
inline constexpr char kCodeRoomNotFound[] = "ROOM_NOT_FOUND";
inline constexpr char kCodeNotYourTurn[] = "NOT_YOUR_TURN";
inline constexpr char kCodeNoPendingAction[] = "NO_PENDING_ACTION";
inline constexpr char kCodeIllegalAction[] = "ILLEGAL_ACTION";
inline constexpr char kCodeUnknownTile[] = "UNKNOWN_TILE";
inline constexpr char kCodeBadRequest[] = "BAD_REQUEST";
inline constexpr char kCodeFrameTooLarge[] = "FRAME_TOO_LARGE";
inline constexpr char kCodeRateLimited[] = "RATE_LIMITED";
inline constexpr char kCodeInternal[] = "INTERNAL";
inline constexpr char kCodeAuthFailed[] = "AUTH_FAILED";
inline constexpr char kCodeUsernameTaken[] = "USERNAME_TAKEN";
inline constexpr char kCodeAlreadyOnline[] = "ALREADY_ONLINE";

// ---- ACCOUNT_DESIGN.md §3.1: login/register field rules --------------------------------
// Account names and passwords are deliberately ASCII-only, which makes a SQL injection
// through them structurally impossible (the DB layer escapes as well, see db.h).
inline constexpr size_t kMinAccountNameChars = 3;
inline constexpr size_t kMaxAccountNameChars = 16;
inline constexpr size_t kMinPasswordChars = 6;
inline constexpr size_t kMaxPasswordChars = 64;
inline constexpr size_t kMaxNicknameChars = 16;

inline bool IsValidAccountName(std::string_view username) {
  if (username.size() < kMinAccountNameChars || username.size() > kMaxAccountNameChars) {
    return false;
  }
  for (char c : username) {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!alnum && c != '_') {
      return false;
    }
  }
  return true;
}

inline bool IsValidPassword(std::string_view password) {
  if (password.size() < kMinPasswordChars || password.size() > kMaxPasswordChars) {
    return false;
  }
  for (char raw : password) {
    auto c = static_cast<unsigned char>(raw);
    if (c < 0x20 || c > 0x7E) {
      return false;
    }
  }
  return true;
}

// The frozen nickname rule of PROTOCOL.md §4.1 / GAME_DESIGN §3.5, shared by the guest login
// path and by `register`'s optional nickname so the two can never drift apart.
inline bool IsValidNickname(std::string_view name) {
  size_t code_points = utf8::CountCodePoints(name);
  return code_points >= 1 && code_points <= kMaxNicknameChars && !utf8::HasControlCharacter(name);
}

// ---- §2.1 tile codes ------------------------------------------------------------------
// The tile alphabet is owned by the rules library (RULESET §1), so the gateway delegates to
// it instead of hard-coding m/p/s: a future tile-set change (e.g. adding the honour tiles)
// then propagates here automatically and can never leave the gateway rejecting legal tiles.
inline bool IsValidTileCode(std::string_view tile) { return ParseTile(tile).has_value(); }

// ---- §4.6 action names ----------------------------------------------------------------
// The eight names are the wire vocabulary (v1.6: 5 → 7, +ankan/kakan；日麻: +riichi), so the
// vocabulary lives here next to the tile alphabet. Whether an action is legal *in the current
// window* is a game-level question that stays in room.cc (ILLEGAL_ACTION); this only says the
// client spelled a real action.
inline bool IsKnownActionName(std::string_view action) {
  return action == "pass" || action == "pong" || action == "chow" || action == "kong" ||
         action == "ankan" || action == "kakan" || action == "win" || action == "riichi";
}

// ---- §5.2 error frame -----------------------------------------------------------------
std::string ErrorFrame(std::string_view code, std::string_view message);
std::string ErrorFrame(std::string_view code, std::string_view message, json::Value detail);

// ---- §5.1 login_ok / §5.11 pong -------------------------------------------------------
std::string LoginOkFrame(std::string_view player_id, std::string_view name);
std::string PongFrame();

// ---- §5.13 stats (ACCOUNT_DESIGN.md §3.4) ----------------------------------------------
// Either half of this pair is built by the DB layer and serialized here, so the storage layer
// and the wire share one shape instead of two that can drift.
struct StatsEntry {
  std::string room_id;
  std::string reason;
  int delta_score = 0;
  bool win = false;
  int64_t ended_at = 0;  // Unix seconds
  // classic / riichi（日麻接入：两种玩法的分数不可比）。放最后：既有聚合初始化不动。
  std::string mode = "classic";
};

struct StatsSummary {
  int games = 0;
  int wins = 0;
  int draws = 0;
  int net_score = 0;
  std::vector<StatsEntry> recent;  // newest first, at most kMaxRecentMatches entries
};

// 按玩法分开的统计。classic 的四个字段同时是 stats 帧的**顶层字段**（老客户端不受影响），
// 日麻放在 `riichi` 子对象里：两套玩法的 net_score 语义不同（零和得失分 vs 25000 起持点），
// 混在一个数字里会直接误导玩家。
struct StatsSnapshot {
  StatsSummary classic;
  StatsSummary riichi;
};

inline constexpr size_t kMaxRecentMatches = 10;

std::string StatsFrame(const StatsSnapshot& snapshot);
// 兼容重载：只填 classic 的调用（内部与单测）等价于「riichi 桶为空」。
std::string StatsFrame(const StatsSummary& summary);

}  // namespace mahjong::protocol
