#include "frame_parser.h"

#include <string>
#include <utility>

#include "protocol.h"

namespace mahjong::frame_parser {
namespace {

// ACCOUNT_DESIGN.md §3.1: the field rules shared by `login`'s account form and `register`.
constexpr char kAccountFieldRules[] =
    "username must be 3..16 characters [A-Za-z0-9_] and password 6..64 printable ASCII characters";

// PROTOCOL.md §4.1: the frozen nickname rule message (the rule itself is
// protocol::IsValidNickname, shared with register's optional name).
constexpr char kNicknameRules[] = "name must be 1..16 UTF-8 characters without control characters";

// The three setters return false so the call sites read `return Reject...(...)` — one line per
// rejection, and no way to report an error but keep going.
bool RejectBadRequest(Error* error, std::string message) {
  error->kind = ErrorKind::kBadRequest;
  error->code = protocol::kCodeBadRequest;
  error->message = std::move(message);
  return false;
}

bool RejectUnknownTile(Error* error, const std::string& tile) {
  error->kind = ErrorKind::kIllegalOp;
  error->code = protocol::kCodeUnknownTile;
  error->message = "invalid tile code: " + tile;
  return false;
}

bool RejectInvalidName(Error* error) {
  error->kind = ErrorKind::kInvalidName;
  error->code = protocol::kCodeInvalidName;
  error->message = kNicknameRules;
  return false;
}

// §4.5: a `seq` is the client's monotonic counter, so 0 and negatives are malformed, not stale.
bool IsValidSeq(const json::Value* seq) { return seq != nullptr && seq->IsInteger() && seq->AsInt() >= 1; }

}  // namespace

std::optional<Envelope> ParseEnvelope(std::string_view line, Error* error) {
  std::string parse_error;
  std::optional<json::Value> parsed = json::Value::Parse(line, &parse_error);
  if (!parsed.has_value()) {
    RejectBadRequest(error, "malformed JSON: " + parse_error);
    return std::nullopt;
  }
  if (!parsed->IsObject()) {
    RejectBadRequest(error, "frame must be a JSON object");
    return std::nullopt;
  }
  const json::Value* type = parsed->Find("type");
  if (type == nullptr || !type->IsString()) {
    RejectBadRequest(error, "frame must carry a string \"type\"");
    return std::nullopt;
  }
  // Copied out first: |type| points into |*parsed|, which the return below moves away.
  std::string type_name = type->AsString();
  return Envelope {.frame = std::move(*parsed), .type = std::move(type_name)};
}

std::optional<Login> ParseLogin(const json::Value& frame, Error* error) {
  // ACCOUNT_DESIGN.md §3.1: the presence of `username` selects the account form; without it
  // the frame is the frozen name-only guest login.
  const json::Value* account_name = frame.Find("username");
  if (account_name != nullptr) {
    const json::Value* password = frame.Find("password");
    if (!account_name->IsString() || password == nullptr || !password->IsString()) {
      RejectBadRequest(error, "account login requires a string \"username\" and a string \"password\"");
      return std::nullopt;
    }
    if (!protocol::IsValidAccountName(account_name->AsString()) || !protocol::IsValidPassword(password->AsString())) {
      RejectBadRequest(error, kAccountFieldRules);
      return std::nullopt;
    }
    Login intent;
    intent.account_form = true;
    intent.username = account_name->AsString();
    intent.password = password->AsString();
    return intent;
  }

  const json::Value* name = frame.Find("name");
  if (name == nullptr || !name->IsString()) {
    RejectBadRequest(error, "login requires a string \"name\"");
    return std::nullopt;
  }
  if (!protocol::IsValidNickname(name->AsString())) {
    RejectInvalidName(error);  // §1.7 counts these apart from bad frames
    return std::nullopt;
  }
  Login intent;
  intent.name = name->AsString();
  return intent;
}

std::optional<Register> ParseRegister(const json::Value& frame, Error* error) {
  const json::Value* username = frame.Find("username");
  const json::Value* password = frame.Find("password");
  if (username == nullptr || !username->IsString() || password == nullptr || !password->IsString()) {
    RejectBadRequest(error, "register requires a string \"username\" and a string \"password\"");
    return std::nullopt;
  }
  if (!protocol::IsValidAccountName(username->AsString()) || !protocol::IsValidPassword(password->AsString())) {
    RejectBadRequest(error, kAccountFieldRules);
    return std::nullopt;
  }

  // §3.2: the nickname is optional and defaults to the username (the DB layer fills it in).
  Register intent;
  const json::Value* name = frame.Find("name");
  if (name != nullptr) {
    if (!name->IsString() || !protocol::IsValidNickname(name->AsString())) {
      RejectBadRequest(error, "register's optional \"name\" must be 1..16 UTF-8 characters without control characters");
      return std::nullopt;
    }
    intent.nickname = name->AsString();
  }
  intent.username = username->AsString();
  intent.password = password->AsString();
  return intent;
}

std::optional<JoinRoom> ParseJoinRoom(const json::Value& frame, Error* error) {
  const json::Value* room_id = frame.Find("room_id");
  if (room_id == nullptr || !room_id->IsString()) {
    RejectBadRequest(error, "join_room requires a string \"room_id\"");
    return std::nullopt;
  }
  return JoinRoom {room_id->AsString()};
}

bool ValidateDiscard(const json::Value& frame, Error* error) {
  const json::Value* tile = frame.Find("tile");
  const json::Value* seq = frame.Find("seq");
  if (tile == nullptr || !tile->IsString() || !IsValidSeq(seq)) {
    return RejectBadRequest(error, "discard requires a string \"tile\" and an integer \"seq\" >= 1");
  }
  if (!protocol::IsValidTileCode(tile->AsString())) {
    return RejectUnknownTile(error, tile->AsString());
  }
  return true;
}

bool ValidateAction(const json::Value& frame, Error* error) {
  const json::Value* action = frame.Find("action");
  const json::Value* tiles = frame.Find("tiles");
  const json::Value* seq = frame.Find("seq");
  if (action == nullptr || !action->IsString() || tiles == nullptr || !tiles->IsArray() || !IsValidSeq(seq)) {
    return RejectBadRequest(error, "action requires a string \"action\", an array \"tiles\" and an integer \"seq\" >= 1");
  }
  if (!protocol::IsKnownActionName(action->AsString())) {
    // §4.6 freezes the five action names; an unknown one is a malformed frame. Whether the
    // action is legal *in the current window* is ILLEGAL_ACTION and belongs to the room.
    return RejectBadRequest(error, "unknown action: " + action->AsString());
  }
  for (const json::Value& tile : tiles->AsArray()) {
    if (!tile.IsString()) {
      return RejectBadRequest(error, "\"tiles\" must only contain strings");
    }
    if (!protocol::IsValidTileCode(tile.AsString())) {
      return RejectUnknownTile(error, tile.AsString());
    }
  }
  return true;
}

}  // namespace mahjong::frame_parser
