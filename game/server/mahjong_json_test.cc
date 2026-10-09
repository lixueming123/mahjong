// Unit tests for the gateway's JSON parser/serializer (game/server/json.h).

#include <gtest/gtest.h>

#include <string>

#include "json.h"

namespace {

using mahjong::json::Value;

std::optional<Value> ParseOk(const std::string& text) {
  std::string error;
  std::optional<Value> value = Value::Parse(text, &error);
  EXPECT_TRUE(value.has_value()) << "expected success for " << text << " but got: " << error;
  return value;
}

void ExpectParseFailure(const std::string& text) {
  std::string error;
  std::optional<Value> value = Value::Parse(text, &error);
  EXPECT_FALSE(value.has_value()) << "expected failure for " << text;
  EXPECT_FALSE(error.empty());
}

TEST(JsonTest, ParsesScalars) {
  EXPECT_TRUE(ParseOk("null")->IsNull());
  EXPECT_TRUE(ParseOk("true")->AsBool());
  EXPECT_FALSE(ParseOk("false")->AsBool());
  EXPECT_EQ(ParseOk("42")->AsInt(), 42);
  EXPECT_TRUE(ParseOk("42")->IsInteger());
  EXPECT_EQ(ParseOk("-7")->AsInt(), -7);
  EXPECT_DOUBLE_EQ(ParseOk("1.5")->AsDouble(), 1.5);
  EXPECT_FALSE(ParseOk("1.5")->IsInteger());
  EXPECT_DOUBLE_EQ(ParseOk("1e3")->AsDouble(), 1000.0);
  EXPECT_EQ(ParseOk("\"text\"")->AsString(), "text");
}

TEST(JsonTest, ParsesObjectAndKeepsFieldOrder) {
  std::optional<Value> value = ParseOk(R"({"type":"login","name":"玩家_","seq":3})");
  ASSERT_TRUE(value->IsObject());
  ASSERT_EQ(value->AsObject().size(), 3u);
  EXPECT_EQ(value->AsObject()[0].first, "type");
  EXPECT_EQ(value->AsObject()[1].first, "name");
  EXPECT_EQ(value->AsObject()[2].first, "seq");
  EXPECT_EQ(value->Find("type")->AsString(), "login");
  EXPECT_EQ(value->Find("name")->AsString(), "玩家_");
  EXPECT_EQ(value->Find("seq")->AsInt(), 3);
  EXPECT_EQ(value->Find("missing"), nullptr);
}

TEST(JsonTest, ParsesNestedStructures) {
  std::optional<Value> value = ParseOk(R"({"a":[1,[2,3],{"b":null}],"c":{}})");
  ASSERT_TRUE(value->IsObject());
  const Value* a = value->Find("a");
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->IsArray());
  ASSERT_EQ(a->AsArray().size(), 3u);
  EXPECT_EQ(a->AsArray()[1].AsArray()[1].AsInt(), 3);
  EXPECT_TRUE(a->AsArray()[2].Find("b")->IsNull());
  EXPECT_TRUE(value->Find("c")->AsObject().empty());
}

TEST(JsonTest, ParsesEscapesAndSurrogatePairs) {
  EXPECT_EQ(ParseOk(R"("a\nb\t\"c\"\\d")")->AsString(), "a\nb\t\"c\"\\d");
  EXPECT_EQ(ParseOk(R"("\u00e9")")->AsString(), "\xc3\xa9");
  EXPECT_EQ(ParseOk(R"("\ud83d\ude00")")->AsString(), "\xf0\x9f\x98\x80");
  EXPECT_EQ(ParseOk(R"("\/")")->AsString(), "/");
}

TEST(JsonTest, RejectsMalformedDocuments) {
  ExpectParseFailure("");
  ExpectParseFailure("   ");
  ExpectParseFailure("{");
  ExpectParseFailure("{\"a\":1");
  ExpectParseFailure("{\"a\"1}");
  ExpectParseFailure("{\"a\":1,}");
  ExpectParseFailure("[1,2");
  ExpectParseFailure("[1,]");
  ExpectParseFailure("{'a':1}");
  ExpectParseFailure("{\"a\":1} trailing");
  ExpectParseFailure("{\"a\":1} {\"b\":2}");
  ExpectParseFailure("nul");
  ExpectParseFailure("True");
  ExpectParseFailure("01");
  ExpectParseFailure("+1");
  ExpectParseFailure(".5");
  ExpectParseFailure("1.");
  ExpectParseFailure("1e");
  ExpectParseFailure("NaN");
  ExpectParseFailure("\"unterminated");
  ExpectParseFailure("\"bad\\xescape\"");
  ExpectParseFailure("\"raw\tcontrol\"");
  ExpectParseFailure("\"\\ud83d\"");
  ExpectParseFailure("\"\\udc00\"");
  ExpectParseFailure("123456789012345678901234567890123456789012345678901234567890"
                     "123456789012345678901234567890123456789012345678901234567890"
                     "123456789012345678901234567890123456789012345678901234567890"
                     "123456789012345678901234567890123456789012345678901234567890"
                     "123456789012345678901234567890123456789012345678901234567890"
                     "123456789012345678901234567890123456789012345678901234567890"
                     "123456789012345678901234567890123456789012345678901234567890");
}

TEST(JsonTest, RejectsExcessiveNesting) {
  std::string deep;
  for (int i = 0; i < 200; ++i) {
    deep.push_back('[');
  }
  for (int i = 0; i < 200; ++i) {
    deep.push_back(']');
  }
  ExpectParseFailure(deep);
}

TEST(JsonTest, DumpsFrames) {
  Value frame = Value::Obj();
  frame.Set("type", Value::Str("error"));
  frame.Set("code", Value::Str("BAD_REQUEST"));
  Value detail = Value::Obj();
  detail.Set("tile", Value::Str("4p"));
  frame.Set("detail", std::move(detail));
  EXPECT_EQ(frame.Dump(), R"({"type":"error","code":"BAD_REQUEST","detail":{"tile":"4p"}})");

  Value array = Value::Arr();
  array.Append(Value::Int(1));
  array.Append(Value::Bool(false));
  array.Append(Value::Null());
  array.Append(Value::Str("a\"b\n"));
  EXPECT_EQ(array.Dump(), R"([1,false,null,"a\"b\n"])");
}

TEST(JsonTest, DumpEscapesControlCharactersAndKeepsUtf8) {
  Value value = Value::Str(std::string("x\x01y") + "\xe7\x8e\xa9");
  EXPECT_EQ(value.Dump(), "\"x\\u0001y\xe7\x8e\xa9\"");
}

TEST(JsonTest, RoundTripsGeneratedFrames) {
  Value frame = Value::Obj();
  frame.Set("type", Value::Str("action_event"));
  frame.Set("seat", Value::Int(1));
  frame.Set("action", Value::Str("pong"));
  Value tiles = Value::Arr();
  tiles.Append(Value::Str("9s"));
  frame.Set("tiles", std::move(tiles));
  Value options = Value::Arr();
  Value option = Value::Obj();
  option.Set("action", Value::Str("pass"));
  option.Set("tiles", Value::Arr());
  options.Append(std::move(option));
  frame.Set("options", std::move(options));

  std::optional<Value> reparsed = ParseOk(frame.Dump());
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(reparsed->Find("type")->AsString(), "action_event");
  EXPECT_EQ(reparsed->Find("tiles")->AsArray()[0].AsString(), "9s");
  EXPECT_EQ(reparsed->Find("options")->AsArray()[0].Find("action")->AsString(), "pass");
}

}  // namespace
