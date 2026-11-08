// Frozen wire enum values for PROTOCOL v2.0 (game/docs/PROTOCOL.md §1.3.1).
//
// The single source of truth is game/protocol/enums.json. This header keeps a checked-in
// typed copy; game/tests/protocol_enums_consistency_test.py verifies both the member values
// and the name tables against the JSON, and against the Python/JS/doc tables. Every domain
// reserves 0 = kInvalid; real values are contiguous 1..N. NEVER renumber a value: append at
// the end of a domain and bump the protocol version instead.
//
// Deliberately dependency-free: mahjong_rules.h needs Action, so including any project
// header here would create a cycle. Conversions to/from the internal MeldType/GameMode/
// GameLength enums live in protocol.h, which may include both sides.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace mahjong::protocol {

// ---- frame_type: C→S 1..11 (kClientFrameTypes order), S→C 12..27 (§3.2 table order) -----
enum class FrameType : uint8_t {
  kInvalid = 0,
  kLogin = 1,
  kRegister = 2,
  kPing = 3,
  kStatsRequest = 4,
  kListRooms = 5,
  kCreateRoom = 6,
  kJoinRoom = 7,
  kReady = 8,
  kSync = 9,
  kDiscard = 10,
  kAction = 11,
  kLoginOk = 12,
  kError = 13,
  kRoomJoined = 14,
  kGameStart = 15,
  kDraw = 16,
  kDiscardEvent = 17,
  kActionRequest = 18,
  kActionEvent = 19,
  kStateSync = 20,
  kGameEnd = 21,
  kPong = 22,
  kStats = 23,
  kRoomList = 24,
  kMatchEnd = 25,
  kTenpai = 26,
  kRoomUpdate = 27,
};
inline constexpr int kFrameTypeMax = 27;
inline constexpr int kClientFrameTypeMax = 11;
inline constexpr std::array<std::string_view, kFrameTypeMax + 1> kFrameTypeNames = {
    "INVALID",     "login",      "register",       "ping",          "stats_request",
    "list_rooms",  "create_room", "join_room",     "ready",         "sync",
    "discard",     "action",      "login_ok",      "error",         "room_joined",
    "game_start",  "draw",        "discard_event", "action_request", "action_event",
    "state_sync",  "game_end",    "pong",          "stats",         "room_list",
    "match_end",   "tenpai",      "room_update",
};
inline constexpr std::array<FrameType, kClientFrameTypeMax> kClientFrameTypes = {
    FrameType::kLogin,        FrameType::kRegister, FrameType::kPing,
    FrameType::kStatsRequest, FrameType::kListRooms, FrameType::kCreateRoom,
    FrameType::kJoinRoom,     FrameType::kReady,    FrameType::kSync,
    FrameType::kDiscard,      FrameType::kAction,
};

// ---- action (§4.6 + 日麻 riichi) ---------------------------------------------------------
enum class Action : uint8_t {
  kInvalid = 0,
  kPass = 1,
  kPong = 2,
  kChow = 3,
  kKong = 4,
  kAnkan = 5,
  kKakan = 6,
  kWin = 7,
  kRiichi = 8,
};
inline constexpr int kActionMax = 8;
inline constexpr std::array<std::string_view, kActionMax + 1> kActionNames = {
    "INVALID", "pass", "pong", "chow", "kong", "ankan", "kakan", "win", "riichi",
};

// ---- window (§5.7 + §12.10 chankan) ----------------------------------------------------
enum class Window : uint8_t {
  kInvalid = 0,
  kDiscard = 1,
  kClaim = 2,
  kChankan = 3,
};
inline constexpr int kWindowMax = 3;
inline constexpr std::array<std::string_view, kWindowMax + 1> kWindowNames = {
    "INVALID", "discard", "claim", "chankan",
};

// ---- mode / length (§12.1) --------------------------------------------------------------
enum class WireMode : uint8_t {
  kInvalid = 0,
  kClassic = 1,
  kRiichi = 2,
};
inline constexpr int kModeMax = 2;
inline constexpr std::array<std::string_view, kModeMax + 1> kModeNames = {
    "INVALID", "classic", "riichi",
};

enum class WireLength : uint8_t {
  kInvalid = 0,
  kTonpu = 1,
  kHanchan = 2,
};
inline constexpr int kLengthMax = 2;
inline constexpr std::array<std::string_view, kLengthMax + 1> kLengthNames = {
    "INVALID", "tonpu", "hanchan",
};

// ---- melds[].type (§5.9) ----------------------------------------------------------------
// A separate domain from Action: `pong` is 2 in Action but 1 here. Do not unify.
enum class WireMeldType : uint8_t {
  kInvalid = 0,
  kPong = 1,
  kChow = 2,
  kKong = 3,
  kAnkan = 4,
  kKakan = 5,
};
inline constexpr int kMeldTypeMax = 5;
inline constexpr std::array<std::string_view, kMeldTypeMax + 1> kMeldTypeNames = {
    "INVALID", "pong", "chow", "kong", "ankan", "kakan",
};

// ---- game_end.reason / stats.recent[].reason (§5.10) -----------------------------------
enum class RoundEndReason : uint8_t {
  kInvalid = 0,
  kWinRon = 1,
  kWinTsumo = 2,
  kDrawExhausted = 3,
};
inline constexpr int kReasonMax = 3;
inline constexpr std::array<std::string_view, kReasonMax + 1> kReasonNames = {
    "INVALID", "win_ron", "win_tsumo", "draw_exhausted",
};

// ---- error.code (§7.1, exactly 14; STALE_SEQ deliberately absent) ----------------------
enum class ErrorCode : uint8_t {
  kInvalid = 0,
  kInvalidName = 1,
  kNotLoggedIn = 2,
  kRoomNotFound = 3,
  kNotYourTurn = 4,
  kNoPendingAction = 5,
  kIllegalAction = 6,
  kUnknownTile = 7,
  kBadRequest = 8,
  kFrameTooLarge = 9,
  kRateLimited = 10,
  kInternal = 11,
  kAuthFailed = 12,
  kUsernameTaken = 13,
  kAlreadyOnline = 14,
};
inline constexpr int kErrorCodeMax = 14;
inline constexpr std::array<std::string_view, kErrorCodeMax + 1> kErrorCodeNames = {
    "INVALID",         "INVALID_NAME",   "NOT_LOGGED_IN", "ROOM_NOT_FOUND",
    "NOT_YOUR_TURN",   "NO_PENDING_ACTION", "ILLEGAL_ACTION", "UNKNOWN_TILE",
    "BAD_REQUEST",     "FRAME_TOO_LARGE", "RATE_LIMITED",  "INTERNAL",
    "AUTH_FAILED",     "USERNAME_TAKEN", "ALREADY_ONLINE",
};

// ---- generic helpers --------------------------------------------------------------------
// One trait per domain keeps NameOf/ParseWire/FromName to a single implementation; the
// consistency test still parses each enum class block and each kXNames array separately.
template <typename E>
struct WireTable;

template <>
struct WireTable<FrameType> {
  static constexpr int kMax = kFrameTypeMax;
  static constexpr const std::array<std::string_view, kFrameTypeMax + 1>* kNames = &kFrameTypeNames;
};
template <>
struct WireTable<Action> {
  static constexpr int kMax = kActionMax;
  static constexpr const std::array<std::string_view, kActionMax + 1>* kNames = &kActionNames;
};
template <>
struct WireTable<Window> {
  static constexpr int kMax = kWindowMax;
  static constexpr const std::array<std::string_view, kWindowMax + 1>* kNames = &kWindowNames;
};
template <>
struct WireTable<WireMode> {
  static constexpr int kMax = kModeMax;
  static constexpr const std::array<std::string_view, kModeMax + 1>* kNames = &kModeNames;
};
template <>
struct WireTable<WireLength> {
  static constexpr int kMax = kLengthMax;
  static constexpr const std::array<std::string_view, kLengthMax + 1>* kNames = &kLengthNames;
};
template <>
struct WireTable<WireMeldType> {
  static constexpr int kMax = kMeldTypeMax;
  static constexpr const std::array<std::string_view, kMeldTypeMax + 1>* kNames = &kMeldTypeNames;
};
template <>
struct WireTable<RoundEndReason> {
  static constexpr int kMax = kReasonMax;
  static constexpr const std::array<std::string_view, kReasonMax + 1>* kNames = &kReasonNames;
};
template <>
struct WireTable<ErrorCode> {
  static constexpr int kMax = kErrorCodeMax;
  static constexpr const std::array<std::string_view, kErrorCodeMax + 1>* kNames = &kErrorCodeNames;
};

template <typename E>
constexpr int WireOf(E value) {
  return static_cast<int>(value);
}

template <typename E>
constexpr std::optional<E> ParseWire(int64_t value) {
  if (value <= 0 || value > WireTable<E>::kMax) {
    return std::nullopt;
  }
  return static_cast<E>(value);
}

// Unknown / out-of-range values become the reserved INVALID sentinel. Used only for logs,
// free-text error messages and DB boundary translation — never for control flow.
template <typename E>
constexpr std::string_view NameOf(E value) {
  const int index = WireOf(value);
  if (index <= 0 || index > WireTable<E>::kMax) {
    return "INVALID";
  }
  return (*WireTable<E>::kNames)[static_cast<size_t>(index)];
}

template <typename E>
constexpr std::optional<E> FromName(std::string_view name) {
  const auto& names = *WireTable<E>::kNames;
  for (size_t index = 1; index < names.size(); ++index) {
    if (names[index] == name) {
      return static_cast<E>(index);
    }
  }
  return std::nullopt;
}

// Named wrappers keep call sites readable: ParseFrameType(v), ParseAction(v), ...
constexpr std::optional<FrameType> ParseFrameType(int64_t value) {
  return ParseWire<FrameType>(value);
}
constexpr std::optional<Action> ParseAction(int64_t value) {
  return ParseWire<Action>(value);
}
constexpr std::optional<Window> ParseWindow(int64_t value) {
  return ParseWire<Window>(value);
}
constexpr std::optional<WireMode> ParseWireMode(int64_t value) {
  return ParseWire<WireMode>(value);
}
constexpr std::optional<WireLength> ParseWireLength(int64_t value) {
  return ParseWire<WireLength>(value);
}
constexpr std::optional<WireMeldType> ParseWireMeldType(int64_t value) {
  return ParseWire<WireMeldType>(value);
}
constexpr std::optional<RoundEndReason> ParseReason(int64_t value) {
  return ParseWire<RoundEndReason>(value);
}
constexpr std::optional<ErrorCode> ParseErrorCode(int64_t value) {
  return ParseWire<ErrorCode>(value);
}

constexpr bool IsClientFrameType(FrameType type) {
  const int value = WireOf(type);
  return value >= 1 && value <= kClientFrameTypeMax;
}
constexpr bool IsServerFrameType(FrameType type) {
  const int value = WireOf(type);
  return value > kClientFrameTypeMax && value <= kFrameTypeMax;
}

// ---- compile-time invariants ------------------------------------------------------------
// These guard the header itself; the cross-language guard is the consistency test.
static_assert(std::is_same_v<std::underlying_type_t<FrameType>, uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<Action>, uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<Window>, uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<WireMode>, uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<WireLength>, uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<WireMeldType>, uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<RoundEndReason>, uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<ErrorCode>, uint8_t>);

static_assert(WireOf(FrameType::kRoomUpdate) == kFrameTypeMax);
static_assert(WireOf(Action::kRiichi) == kActionMax);
static_assert(WireOf(Window::kChankan) == kWindowMax);
static_assert(WireOf(WireMode::kRiichi) == kModeMax);
static_assert(WireOf(WireLength::kHanchan) == kLengthMax);
static_assert(WireOf(WireMeldType::kKakan) == kMeldTypeMax);
static_assert(WireOf(RoundEndReason::kDrawExhausted) == kReasonMax);
static_assert(WireOf(ErrorCode::kAlreadyOnline) == kErrorCodeMax);

static_assert(kFrameTypeNames.size() == static_cast<size_t>(kFrameTypeMax + 1));
static_assert(kActionNames.size() == static_cast<size_t>(kActionMax + 1));
static_assert(kWindowNames.size() == static_cast<size_t>(kWindowMax + 1));
static_assert(kModeNames.size() == static_cast<size_t>(kModeMax + 1));
static_assert(kLengthNames.size() == static_cast<size_t>(kLengthMax + 1));
static_assert(kMeldTypeNames.size() == static_cast<size_t>(kMeldTypeMax + 1));
static_assert(kReasonNames.size() == static_cast<size_t>(kReasonMax + 1));
static_assert(kErrorCodeNames.size() == static_cast<size_t>(kErrorCodeMax + 1));
static_assert(kClientFrameTypes.size() == static_cast<size_t>(kClientFrameTypeMax));

}  // namespace mahjong::protocol
