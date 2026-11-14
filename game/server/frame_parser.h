// Pure frame decoding for the NDJSON protocol: one JSON object in, one intent or one wire
// error out.
//
// Nothing here touches Connection, the actors or the database, and that is the point: every
// field rule of PROTOCOL.md §4 and ACCOUNT_DESIGN.md §3.1 becomes checkable on its own
// (frame_parser_test.cc) instead of sitting behind a live gateway, a socket and an actor
// system. PlayerActor keeps what only it can own — the §7.2 quota accounting, the session
// state machine and the routing — and this layer answers just one question: is the frame well
// formed, and what does it ask for.
//
// The error codes and messages are wire contract (§7.1, and the Python e2e tests assert them),
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

// Which §7.2 counter a rejected frame is charged to. The parser classifies; the actor does the
// accounting, because only it owns the Connection.
enum class ErrorKind {
  kBadRequest,   // malformed shape -> BAD_REQUEST, bad-frame quota
  kIllegalOp,    // well formed but illegal content (UNKNOWN_TILE), illegal-op quota
  kInvalidName,  // §1.7 nickname quota — deliberately NOT a BAD_REQUEST
};

struct Error {
  ErrorKind kind = ErrorKind::kBadRequest;
  protocol::ErrorCode code = protocol::ErrorCode::kBadRequest;
  std::string message;
};

// One parsed NDJSON line: the document plus the already-validated "type" (an integer wire
// value, PROTOCOL v2.0 §1.3.1). Handlers need the whole frame (they forward
// `discard`/`action` to the room verbatim), so it travels with the type instead of being
// re-parsed.
struct Envelope {
  json::Value frame;
  protocol::FrameType type = protocol::FrameType::kInvalid;
};

// Parses one line and checks the envelope every frame shares (§4): a JSON object carrying an
// integer "type" in 1..28. Does not decide whether the type is routable — that is dispatch,
// not parsing.
std::optional<Envelope> ParseEnvelope(std::string_view line, Error* error);

// §4.1 guest form (`name`) and ACCOUNT_DESIGN §3.1 account form (`username` + `password`).
// account_form tells the two apart: the presence of "username" selects the account form.
struct Login {
  bool account_form = false;
  std::string username;
  std::string password;
  std::string name;
  // PROTOCOL v2.2 §4.1: account logins default to resuming an in-progress room; `false` means
  // "log in normally but stay in the lobby" (the post-game exits use this to leave the room).
  bool resume = true;
};
std::optional<Login> ParseLogin(const json::Value& frame, Error* error);

// ACCOUNT_DESIGN §3.2. An empty nickname means "let the DB default it to the username".
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

// `discard` and `action` are forwarded to the room as-is (it re-reads the frame to run the
// game-level checks of §4.5/§6.3), so these two only validate and return no intent: fields
// nothing would ever read are just a second copy waiting to rot.
bool ValidateDiscard(const json::Value& frame, Error* error);
bool ValidateAction(const json::Value& frame, Error* error);
bool ValidateAutoplay(const json::Value& frame, Error* error);

}  // namespace mahjong::frame_parser
