// Minimal, dependency-free JSON value used by the mahjong TCP gateway.
//
// The milestone protocol (game/docs/PROTOCOL.md §1.2) transports NDJSON: every frame is a
// single JSON object terminated by '\n'. The ex_actor checkout does not enable a JSON
// library (reflect-cpp is configured with REFLECTCPP_JSON=OFF, and no nlohmann/json is
// vendored), so the gateway ships its own strict RFC 8259 subset:
//
//   * one value parses the whole document, trailing garbage is rejected;
//   * objects keep insertion order so generated frames are stable and testable;
//   * depth is bounded, strings reject unescaped control characters, \u escapes are
//     decoded (including surrogate pairs) into UTF-8.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mahjong::json {

enum class Type { kNull, kBool, kNumber, kString, kArray, kObject, kRawJson };

class Value;

using Array = std::vector<Value>;
// Insertion-ordered object; the protocol frames are small enough that a vector beats a map.
using Object = std::vector<std::pair<std::string, Value>>;

class Value {
 public:
  Value() = default;

  static Value Null();
  static Value Bool(bool value);
  static Value Int(int64_t value);
  static Value Real(double value);
  static Value Str(std::string value);
  // The reserve overloads pre-size the container for a known member count; they are pure
  // hints (see Reserve) and dump exactly like their argument-less forms.
  static Value Arr();
  static Value Arr(size_t reserve);
  static Value Obj();
  static Value Obj(size_t reserve);

  // Wraps text that is ALREADY valid JSON — in practice the Dump()/DumpTo() output of a subtree
  // that several frames share, so the subtree is rendered once and spliced into each frame
  // instead of being rebuilt per frame (RoomActor::StateSyncFrames). DumpTo writes it verbatim,
  // which is where the byte-identity of the spliced frames comes from.
  //
  // Deliberately neither a string nor an object: Is*()/As*() report false/empty, Find() returns
  // nullptr, Set()/Append() are no-ops, and Parse() never produces one. A caller that passes
  // anything other than rendered JSON corrupts the frame it lands in — keep it to the frame
  // builders that own both sides of the splice.
  static Value RawJson(std::string rendered);

  // Parses exactly one JSON document. On failure returns std::nullopt and writes a
  // human readable reason into |*error| when |error| is not null.
  static std::optional<Value> Parse(std::string_view text, std::string* error);

  Type GetType() const { return type_; }
  bool IsNull() const { return type_ == Type::kNull; }
  bool IsBool() const { return type_ == Type::kBool; }
  bool IsNumber() const { return type_ == Type::kNumber; }
  bool IsString() const { return type_ == Type::kString; }
  bool IsArray() const { return type_ == Type::kArray; }
  bool IsObject() const { return type_ == Type::kObject; }
  // True when the value is a number representable as int64 without loss.
  bool IsInteger() const { return type_ == Type::kNumber && int_valued_; }

  bool AsBool() const { return type_ == Type::kBool && bool_; }
  int64_t AsInt() const;
  double AsDouble() const;
  const std::string& AsString() const;
  const Array& AsArray() const;
  const Object& AsObject() const;

  // Object member lookup. Returns nullptr when this is not an object or the key is absent.
  const Value* Find(std::string_view key) const;

  // Builders; no-ops when the current type does not match (never throws). The key is taken as a
  // view and copied only when a new member is inserted — frame builders pass literals.
  void Append(Value value);
  void Set(std::string_view key, Value value);

  // Reserves capacity for |count| elements, std::vector::reserve semantics: it changes neither
  // the contents nor the dump, is a no-op once |count| is not more than the current capacity,
  // and is ignored on any type that is not an array or object. Worth using wherever the member
  // count is known: every element is a full Value (96 bytes on libc++), so the geometric growth
  // of object_/array_ otherwise moves that payload a few times per frame.
  void Reserve(size_t count);

  std::string Dump() const;
  void DumpTo(std::string* out) const;

 private:
  Type type_ = Type::kNull;
  bool bool_ = false;
  bool int_valued_ = false;
  int64_t int_ = 0;
  double real_ = 0;
  std::string string_;
  Array array_;
  Object object_;
};

}  // namespace mahjong::json
