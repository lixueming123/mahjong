// Wire protocol constants and frame builders.
//
// Every enum value in this header is copied from the frozen spec `game/docs/PROTOCOL.md`
// (v2.0 §1.3.1 keeps the frozen integer tables). Implementations MUST NOT invent message
// types, fields or error codes (§0.1): if a needed value is missing, raise a requirement
// review instead of adding it here.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "json.h"
#include "mahjong_rules.h"
#include "protocol_enums.h"
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
// The "type" field of every client frame is an integer wire value (PROTOCOL v2.0 §1.3.1);
// the frozen FrameType enum and the kClientFrameTypes list live in protocol_enums.h, which
// the routing table (player_actor.cc) asserts its completeness against at compile time.

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

// ---- §4.6 action vocabulary ------------------------------------------------------------
// The eight actions are the wire vocabulary (v1.6: 5 → 7, +ankan/kakan；日麻: +riichi),
// frozen as the Action enum in protocol_enums.h. Whether an action is legal *in the current
// window* is a game-level question that stays in room.cc (ILLEGAL_ACTION); an unknown /
// out-of-range integer is rejected in frame_parser.cc (BAD_REQUEST).

// ---- §5.2 error frame -----------------------------------------------------------------
std::string ErrorFrame(ErrorCode code, std::string_view message);
std::string ErrorFrame(ErrorCode code, std::string_view message, json::Value detail);

// ---- §5.1 login_ok / §5.11 pong -------------------------------------------------------
std::string LoginOkFrame(std::string_view player_id, std::string_view name, std::optional<bool> in_room = std::nullopt);
std::string PongFrame();

// ---- §5.13 stats (ACCOUNT_DESIGN.md §3.4) ----------------------------------------------
// Either half of this pair is built by the DB layer and serialized here, so the storage layer
// and the wire share one shape instead of two that can drift.
struct StatsEntry {
  std::string room_id;
  RoundEndReason reason = RoundEndReason::kInvalid;
  int delta_score = 0;
  bool win = false;
  int64_t ended_at = 0;  // Unix seconds
  // classic / riichi（日麻接入：两种玩法的分数不可比）。放最后：既有聚合初始化不动。
  WireMode mode = WireMode::kClassic;
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

// ---- 内部枚举 ↔ 线枚举转换（v2.0）-------------------------------------------------------
// mahjong_rules.h 的 MeldType 是**内部**枚举（0 基、无 INVALID）；线值用 protocol_enums.h 的
// WireMeldType（1..5）。转换只发生在线边界（出站帧 / DB 落库），内部逻辑永远比较内部枚举。
inline WireMeldType ToWire(MeldType type) {
  switch (type) {
    case MeldType::kPong:
      return WireMeldType::kPong;
    case MeldType::kChow:
      return WireMeldType::kChow;
    case MeldType::kKong:
      return WireMeldType::kKong;
    case MeldType::kAnkan:
      return WireMeldType::kAnkan;
    case MeldType::kKakan:
      return WireMeldType::kKakan;
  }
  return WireMeldType::kInvalid;
}

}  // namespace mahjong::protocol
