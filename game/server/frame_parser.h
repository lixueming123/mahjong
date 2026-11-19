// Pure frame decoding for the NDJSON protocol: one JSON object in, one intent or one wire
// error out.
//
// Nothing here touches Connection, the actors or the database, and that is the point: every
// field rule of the wire protocol and of the account frames becomes checkable on its own
// (frame_parser_test.cc) instead of sitting behind a live gateway, a socket and an actor
// system. PlayerActor keeps what only it can own — the quota accounting, the session
// state machine and the routing — and this layer answers just one question: is the frame well
// formed, and what does it ask for.
//
// The error codes and messages are wire contract (the Python e2e tests assert them),
// so they are copied verbatim from the handlers these functions were extracted out of. Changing
// one here changes the protocol.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "json.h"
#include "protocol_enums.h"

namespace mahjong::frame_parser {

// Which counter a rejected frame is charged to. The parser classifies; the actor does the
// accounting, because only it owns the Connection.
enum class ErrorKind {
  kBadRequest,   // malformed shape -> BAD_REQUEST, bad-frame quota
  kIllegalOp,    // well formed but illegal content (UNKNOWN_TILE), illegal-op quota
  kInvalidName,  // nickname quota — deliberately NOT a BAD_REQUEST
};

struct Error {
  ErrorKind kind = ErrorKind::kBadRequest;
  protocol::ErrorCode code = protocol::ErrorCode::kBadRequest;
  std::string message;
};

// One parsed NDJSON line: the document plus the already-validated "type" (an integer wire
// value). Handlers need the whole frame (they forward
// `discard`/`action` to the room verbatim), so it travels with the type instead of being
// re-parsed.
struct Envelope {
  json::Value frame;
  protocol::FrameType type = protocol::FrameType::kInvalid;
};

// Parses one line and checks the envelope every frame shares: a JSON object carrying an
// integer "type" in 1..28. Does not decide whether the type is routable — that is dispatch,
// not parsing.
std::optional<Envelope> ParseEnvelope(std::string_view line, Error* error);

// The guest form (`name`) and the account form (`username` + `password`).
// account_form tells the two apart: the presence of "username" selects the account form.
struct Login {
  bool account_form = false;
  std::string username;
  std::string password;
  std::string name;
  // Account logins default to resuming an in-progress room; `false` means
  // "log in normally but stay in the lobby" (the post-game exits use this to leave the room).
  bool resume = true;
};
std::optional<Login> ParseLogin(const json::Value& frame, Error* error);

// An empty nickname means "let the DB default it to the username".
struct Register {
  std::string username;
  std::string password;
  std::string nickname;
};
std::optional<Register> ParseRegister(const json::Value& frame, Error* error);

struct JoinRoom {
  std::string room_id;
};
std::optional<JoinRoom> ParseJoinRoom(const json::Value& frame, Error* error);

// Which recording to fetch. Authorization is not a parse concern — the store decides it
// from the file's seat table.
struct ReplayRequest {
  std::string room_id;
};

// `discard` and `action` are forwarded to the room as-is (it re-reads the frame to run the
// game-level checks), so these two only validate and return no intent: fields
// nothing would ever read are just a second copy waiting to rot.
bool ValidateDiscard(const json::Value& frame, Error* error);
bool ValidateAction(const json::Value& frame, Error* error);
bool ValidateAutoplay(const json::Value& frame, Error* error);

// `room_id` must have the shape lobby.cc mints (`r-` + 13 lowercase base36). Checked here
// so a malformed id never reaches the filesystem, and the check is shared with read paths.
bool IsRoomIdShaped(std::string_view room_id);
std::optional<ReplayRequest> ParseReplayRequest(const json::Value& frame, Error* error);

}  // namespace mahjong::frame_parser
