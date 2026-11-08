// Unit tests for the pure frame decoding layer (frame_parser.h).
//
// No actor, no socket, no database: that is the whole point of the extraction. Every case below
// is a field rule of PROTOCOL.md §4 or ACCOUNT_DESIGN.md §3.1, and the asserted messages are the
// wire contract the Python e2e tests also check — if one of these fails after an edit, the
// protocol changed, not just the implementation.

#include "frame_parser.h"

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "json.h"
#include "protocol.h"

namespace mahjong {
namespace {

json::Value Frame(const char* text) {
  std::string error;
  std::optional<json::Value> parsed = json::Value::Parse(text, &error);
  if (!parsed.has_value()) {
    ADD_FAILURE() << "test fixture is not JSON: " << error;
    return json::Value::Null();
  }
  return *parsed;
}

// ---- envelope (§4) --------------------------------------------------------------------

TEST(FrameParserEnvelope, RejectsMalformedJsonAsBadRequest) {
  frame_parser::Error error;
  EXPECT_FALSE(frame_parser::ParseEnvelope("{\"type\":", &error).has_value());
  EXPECT_EQ(error.kind, frame_parser::ErrorKind::kBadRequest);
  EXPECT_EQ(error.code, protocol::ErrorCode::kBadRequest);
  EXPECT_EQ(error.message.rfind("malformed JSON: ", 0), 0u);
}

TEST(FrameParserEnvelope, RejectsANonObjectFrame) {
  frame_parser::Error error;
  EXPECT_FALSE(frame_parser::ParseEnvelope("[1,2]", &error).has_value());
  EXPECT_EQ(error.kind, frame_parser::ErrorKind::kBadRequest);
  EXPECT_EQ(error.message, "frame must be a JSON object");
}

// PROTOCOL v2.0 §1.3.1: "type" is an integer wire value; a missing one, a string and a float
// are all field-type errors, not unknown-frame-type rejections.
TEST(FrameParserEnvelope, RequiresAnIntegerType) {
  frame_parser::Error missing;
  EXPECT_FALSE(frame_parser::ParseEnvelope(R"({"name":"Alice"})", &missing).has_value());
  EXPECT_EQ(missing.kind, frame_parser::ErrorKind::kBadRequest);
  EXPECT_EQ(missing.message, "frame must carry an integer \"type\"");

  frame_parser::Error string_type;
  EXPECT_FALSE(frame_parser::ParseEnvelope(R"({"type":"login"})", &string_type).has_value());
  EXPECT_EQ(string_type.message, "frame must carry an integer \"type\"");

  frame_parser::Error float_type;
  EXPECT_FALSE(frame_parser::ParseEnvelope(R"({"type":3.0})", &float_type).has_value());
  EXPECT_EQ(float_type.message, "frame must carry an integer \"type\"");
}

// The frame travels on with the type: the handlers forward `discard`/`action` verbatim, so a
// move that lost the document would be invisible until the room rejected every game frame.
TEST(FrameParserEnvelope, KeepsTheParsedFrameAlongsideTheType) {
  std::optional<frame_parser::Envelope> envelope =
      frame_parser::ParseEnvelope(R"({"type":10,"tile":"1m","seq":3})", nullptr);
  ASSERT_TRUE(envelope.has_value());
  EXPECT_EQ(envelope->type, protocol::FrameType::kDiscard);
  const json::Value* tile = envelope->frame.Find("tile");
  ASSERT_NE(tile, nullptr);
  EXPECT_EQ(tile->AsString(), "1m");
}

// ---- login (§4.1 / ACCOUNT_DESIGN §3.1) -----------------------------------------------

TEST(FrameParserLogin, AcceptsTheGuestForm) {
  frame_parser::Error error;
  std::optional<frame_parser::Login> intent = frame_parser::ParseLogin(Frame(R"({"type":1,"name":"Alice"})"), &error);
  ASSERT_TRUE(intent.has_value());
  EXPECT_FALSE(intent->account_form);
  EXPECT_EQ(intent->name, "Alice");
}

TEST(FrameParserLogin, GuestFormRequiresAStringName) {
  frame_parser::Error error;
  EXPECT_FALSE(frame_parser::ParseLogin(Frame(R"({"type":1})"), &error).has_value());
  EXPECT_EQ(error.kind, frame_parser::ErrorKind::kBadRequest);
  EXPECT_EQ(error.message, "login requires a string \"name\"");
}

// §1.7 counts invalid nicknames apart from bad frames, and the 4th one drops the connection, so
// the kind must not be kBadRequest.
TEST(FrameParserLogin, InvalidNicknameIsItsOwnErrorKind) {
  frame_parser::Error empty_name;
  EXPECT_FALSE(frame_parser::ParseLogin(Frame(R"({"type":1,"name":""})"), &empty_name).has_value());
  EXPECT_EQ(empty_name.kind, frame_parser::ErrorKind::kInvalidName);
  EXPECT_EQ(empty_name.code, protocol::ErrorCode::kInvalidName);

  // 17 code points, one over the frozen limit of §4.1.
  frame_parser::Error too_long;
  EXPECT_FALSE(frame_parser::ParseLogin(Frame(R"({"type":1,"name":"0123456789abcdefg"})"), &too_long).has_value());
  EXPECT_EQ(too_long.kind, frame_parser::ErrorKind::kInvalidName);
}

TEST(FrameParserLogin, AcceptsTheAccountForm) {
  frame_parser::Error error;
  std::optional<frame_parser::Login> intent =
      frame_parser::ParseLogin(Frame(R"({"type":1,"username":"Alice_99","password":"hunter2"})"), &error);
  ASSERT_TRUE(intent.has_value());
  EXPECT_TRUE(intent->account_form);
  EXPECT_EQ(intent->username, "Alice_99");
  EXPECT_EQ(intent->password, "hunter2");
}

TEST(FrameParserLogin, AccountFormRequiresBothFields) {
  frame_parser::Error no_password;
  EXPECT_FALSE(frame_parser::ParseLogin(Frame(R"({"type":1,"username":"Alice"})"), &no_password).has_value());
  EXPECT_EQ(no_password.message, "account login requires a string \"username\" and a string \"password\"");

  frame_parser::Error numeric;
  EXPECT_FALSE(frame_parser::ParseLogin(Frame(R"({"type":1,"username":"Alice","password":5})"), &numeric).has_value());
  EXPECT_EQ(numeric.message, "account login requires a string \"username\" and a string \"password\"");
}

TEST(FrameParserLogin, AccountFieldRulesAreEnforced) {
  frame_parser::Error short_name;
  EXPECT_FALSE(
      frame_parser::ParseLogin(Frame(R"({"type":1,"username":"ab","password":"hunter2"})"), &short_name).has_value());
  EXPECT_EQ(short_name.kind, frame_parser::ErrorKind::kBadRequest);
  EXPECT_EQ(short_name.message.rfind("username must be 3..16", 0), 0u);

  frame_parser::Error short_password;
  EXPECT_FALSE(frame_parser::ParseLogin(Frame(R"({"type":1,"username":"Alice","password":"12345"})"), &short_password)
                   .has_value());
  EXPECT_EQ(short_password.message.rfind("username must be 3..16", 0), 0u);

  // A SQL injection attempt fails the character rule, not the database (§3.1).
  frame_parser::Error injection;
  EXPECT_FALSE(frame_parser::ParseLogin(
                   Frame(R"({"type":1,"username":"a'b; DROP TABLE accounts; --","password":"hunter2"})"), &injection)
                   .has_value());
  EXPECT_EQ(injection.kind, frame_parser::ErrorKind::kBadRequest);
}

// ---- register (§3.2) ------------------------------------------------------------------

TEST(FrameParserRegister, NicknameIsOptional) {
  frame_parser::Error error;
  std::optional<frame_parser::Register> intent =
      frame_parser::ParseRegister(Frame(R"({"type":2,"username":"Alice","password":"hunter2"})"), &error);
  ASSERT_TRUE(intent.has_value());
  EXPECT_EQ(intent->username, "Alice");
  EXPECT_TRUE(intent->nickname.empty()) << "an empty nickname means the DB defaults it to the username";
}

TEST(FrameParserRegister, AcceptsAnOptionalNickname) {
  frame_parser::Error error;
  std::optional<frame_parser::Register> intent =
      frame_parser::ParseRegister(Frame(R"({"type":2,"username":"Alice","password":"hunter2","name":"阿丽"})"), &error);
  ASSERT_TRUE(intent.has_value());
  EXPECT_EQ(intent->nickname, "阿丽");
}

TEST(FrameParserRegister, RejectsAMalformedOptionalNickname) {
  frame_parser::Error error;
  EXPECT_FALSE(
      frame_parser::ParseRegister(Frame(R"({"type":2,"username":"Alice","password":"hunter2","name":""})"), &error)
          .has_value());
  EXPECT_EQ(error.kind, frame_parser::ErrorKind::kBadRequest);
  EXPECT_EQ(error.message.rfind("register's optional \"name\"", 0), 0u);
}

TEST(FrameParserRegister, RequiresUsernameAndPassword) {
  frame_parser::Error error;
  EXPECT_FALSE(frame_parser::ParseRegister(Frame(R"({"type":2})"), &error).has_value());
  EXPECT_EQ(error.message, "register requires a string \"username\" and a string \"password\"");
}

// ---- join_room (§4.3) -----------------------------------------------------------------

TEST(FrameParserJoinRoom, RequiresAStringRoomId) {
  frame_parser::Error error;
  std::optional<frame_parser::JoinRoom> intent =
      frame_parser::ParseJoinRoom(Frame(R"({"type":7,"room_id":"r-0001"})"), &error);
  ASSERT_TRUE(intent.has_value());
  EXPECT_EQ(intent->room_id, "r-0001");

  frame_parser::Error missing;
  EXPECT_FALSE(frame_parser::ParseJoinRoom(Frame(R"({"type":7})"), &missing).has_value());
  EXPECT_EQ(missing.message, "join_room requires a string \"room_id\"");
}

// ---- discard (§4.5) -------------------------------------------------------------------

TEST(FrameParserDiscard, AcceptsAWellFormedDiscard) {
  frame_parser::Error error;
  EXPECT_TRUE(frame_parser::ValidateDiscard(Frame(R"({"type":10,"tile":"1m","seq":1})"), &error));
}

TEST(FrameParserDiscard, SeqMustBeAPositiveInteger) {
  frame_parser::Error missing;
  EXPECT_FALSE(frame_parser::ValidateDiscard(Frame(R"({"type":10,"tile":"1m"})"), &missing));
  EXPECT_EQ(missing.kind, frame_parser::ErrorKind::kBadRequest);
  EXPECT_EQ(missing.message, "discard requires a string \"tile\" and an integer \"seq\" >= 1");

  frame_parser::Error zero;
  EXPECT_FALSE(frame_parser::ValidateDiscard(Frame(R"({"type":10,"tile":"1m","seq":0})"), &zero));
  EXPECT_EQ(zero.kind, frame_parser::ErrorKind::kBadRequest);
}

// A real-looking field with an impossible tile is ILLEGAL_OPERATION, not BAD_REQUEST: the frame
// is well formed, the content is not (§7.2 charges them to different counters).
TEST(FrameParserDiscard, UnknownTileIsAnIllegalOperation) {
  frame_parser::Error error;
  EXPECT_FALSE(frame_parser::ValidateDiscard(Frame(R"({"type":10,"tile":"5x","seq":1})"), &error));
  EXPECT_EQ(error.kind, frame_parser::ErrorKind::kIllegalOp);
  EXPECT_EQ(error.code, protocol::ErrorCode::kUnknownTile);
  EXPECT_EQ(error.message, "invalid tile code: 5x");
}

TEST(FrameParserDiscard, NonStringTileIsABadRequest) {
  frame_parser::Error error;
  EXPECT_FALSE(frame_parser::ValidateDiscard(Frame(R"({"type":10,"tile":1,"seq":1})"), &error));
  EXPECT_EQ(error.kind, frame_parser::ErrorKind::kBadRequest);
}

// ---- action (§4.6) --------------------------------------------------------------------

TEST(FrameParserAction, AcceptsAKnownAction) {
  frame_parser::Error error;
  EXPECT_TRUE(frame_parser::ValidateAction(Frame(R"({"type":11,"action":1,"tiles":[],"seq":2})"), &error));
  EXPECT_TRUE(frame_parser::ValidateAction(Frame(R"({"type":11,"action":3,"tiles":["1m","2m"],"seq":2})"), &error));
}

// §4.6 freezes the eight action wire values; an unknown integer is a malformed frame, while an
// action that is merely illegal in this window stays ILLEGAL_ACTION inside the room.
TEST(FrameParserAction, UnknownActionValueIsABadRequest) {
  frame_parser::Error error;
  EXPECT_FALSE(frame_parser::ValidateAction(Frame(R"({"type":11,"action":99,"tiles":[],"seq":2})"), &error));
  EXPECT_EQ(error.kind, frame_parser::ErrorKind::kBadRequest);
  EXPECT_EQ(error.message, "unknown action: 99");
}

TEST(FrameParserAction, TilesMustBeAnArrayOfStrings) {
  frame_parser::Error not_array;
  EXPECT_FALSE(frame_parser::ValidateAction(Frame(R"({"type":11,"action":2,"tiles":"1m","seq":2})"), &not_array));
  EXPECT_EQ(not_array.message, "action requires an integer \"action\", an array \"tiles\" and an integer \"seq\" >= 1");

  frame_parser::Error not_strings;
  EXPECT_FALSE(frame_parser::ValidateAction(Frame(R"({"type":11,"action":2,"tiles":[1],"seq":2})"), &not_strings));
  EXPECT_EQ(not_strings.message, "\"tiles\" must only contain strings");
}

TEST(FrameParserAction, UnknownTileInsideTilesIsAnIllegalOperation) {
  frame_parser::Error error;
  EXPECT_FALSE(frame_parser::ValidateAction(Frame(R"({"type":11,"action":3,"tiles":["1m","9z"],"seq":2})"), &error));
  EXPECT_EQ(error.kind, frame_parser::ErrorKind::kIllegalOp);
  EXPECT_EQ(error.code, protocol::ErrorCode::kUnknownTile);
}

}  // namespace
}  // namespace mahjong
