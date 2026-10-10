#include "json.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace mahjong::json {
namespace {

constexpr int kMaxDepth = 64;

// Starter capacity for Dump()'s output string. DumpTo appends without reserving (it recurses
// per node, so a hint there would re-reserve at every level), and the protocol frames span
// roughly 60 bytes (error/draw) to 1 KB (game_end/state_sync), which from an empty string is
// several growths. Capacity only: the emitted bytes are unchanged.
constexpr size_t kDumpReserveHint = 256;

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool IsWhitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

void AppendUtf8(uint32_t code_point, std::string* out) {
  if (code_point <= 0x7F) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

void AppendEscapedString(const std::string& value, std::string* out) {
  out->push_back('"');
  for (char raw : value) {
    auto c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\b': out->append("\\b"); break;
      case '\f': out->append("\\f"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out->append(buf);
        } else {
          out->push_back(raw);
        }
    }
  }
  out->push_back('"');
}

// Recursive descent parser over a single document.
class Parser {
 public:
  Parser(std::string_view text, std::string* error)
      : begin_(text.data()), p_(text.data()), end_(text.data() + text.size()), error_(error) {}

  std::optional<Value> ParseDocument() {
    SkipWhitespace();
    std::optional<Value> value = ParseValue(0);
    if (!value.has_value()) {
      return std::nullopt;
    }
    SkipWhitespace();
    if (p_ != end_) {
      return Fail("trailing characters after the JSON value");
    }
    return value;
  }

 private:
  std::nullopt_t Fail(const std::string& message) {
    if (error_ != nullptr && error_->empty()) {
      *error_ = message + " (offset " + std::to_string(p_ - begin_) + ")";
    }
    return std::nullopt;
  }

  void SkipWhitespace() {
    while (p_ != end_ && IsWhitespace(*p_)) {
      ++p_;
    }
  }

  bool ConsumeLiteral(std::string_view literal) {
    if (static_cast<size_t>(end_ - p_) < literal.size() || std::memcmp(p_, literal.data(), literal.size()) != 0) {
      return false;
    }
    p_ += literal.size();
    return true;
  }

  std::optional<Value> ParseValue(int depth) {
    if (depth > kMaxDepth) {
      return Fail("JSON nesting is too deep");
    }
    if (p_ == end_) {
      return Fail("unexpected end of input");
    }
    switch (*p_) {
      case '{': return ParseObject(depth);
      case '[': return ParseArray(depth);
      case '"': {
        std::string text;
        if (!ParseString(&text)) {
          return std::nullopt;
        }
        return Value::Str(std::move(text));
      }
      case 't':
        if (!ConsumeLiteral("true")) return Fail("invalid literal, expected 'true'");
        return Value::Bool(true);
      case 'f':
        if (!ConsumeLiteral("false")) return Fail("invalid literal, expected 'false'");
        return Value::Bool(false);
      case 'n':
        if (!ConsumeLiteral("null")) return Fail("invalid literal, expected 'null'");
        return Value::Null();
      default:
        if (*p_ == '-' || IsDigit(*p_)) {
          return ParseNumber();
        }
        return Fail(std::string("unexpected character '") + *p_ + "'");
    }
  }

  std::optional<Value> ParseObject(int depth) {
    ++p_;  // '{'
    Value object = Value::Obj();
    SkipWhitespace();
    if (p_ != end_ && *p_ == '}') {
      ++p_;
      return object;
    }
    while (true) {
      SkipWhitespace();
      if (p_ == end_ || *p_ != '"') {
        return Fail("expected a string key in object");
      }
      std::string key;
      if (!ParseString(&key)) {
        return std::nullopt;
      }
      SkipWhitespace();
      if (p_ == end_ || *p_ != ':') {
        return Fail("expected ':' after the object key");
      }
      ++p_;
      SkipWhitespace();
      std::optional<Value> member = ParseValue(depth + 1);
      if (!member.has_value()) {
        return std::nullopt;
      }
      object.Set(std::move(key), std::move(*member));
      SkipWhitespace();
      if (p_ == end_) {
        return Fail("unterminated object");
      }
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ == '}') {
        ++p_;
        return object;
      }
      return Fail("expected ',' or '}' in object");
    }
  }

  std::optional<Value> ParseArray(int depth) {
    ++p_;  // '['
    Value array = Value::Arr();
    SkipWhitespace();
    if (p_ != end_ && *p_ == ']') {
      ++p_;
      return array;
    }
    while (true) {
      SkipWhitespace();
      std::optional<Value> element = ParseValue(depth + 1);
      if (!element.has_value()) {
        return std::nullopt;
      }
      array.Append(std::move(*element));
      SkipWhitespace();
      if (p_ == end_) {
        return Fail("unterminated array");
      }
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ == ']') {
        ++p_;
        return array;
      }
      return Fail("expected ',' or ']' in array");
    }
  }

  bool ParseHex4(uint32_t* out) {
    if (end_ - p_ < 4) {
      return false;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      char c = p_[i];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return false;
      }
    }
    p_ += 4;
    *out = value;
    return true;
  }

  bool ParseString(std::string* out) {
    if (p_ == end_ || *p_ != '"') {
      Fail("expected a string");
      return false;
    }
    ++p_;
    out->clear();
    while (true) {
      if (p_ == end_) {
        Fail("unterminated string");
        return false;
      }
      auto c = static_cast<unsigned char>(*p_);
      if (c == '"') {
        ++p_;
        return true;
      }
      if (c == '\\') {
        ++p_;
        if (p_ == end_) {
          Fail("unterminated escape sequence");
          return false;
        }
        char escape = *p_++;
        switch (escape) {
          case '"': out->push_back('"'); break;
          case '\\': out->push_back('\\'); break;
          case '/': out->push_back('/'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'n': out->push_back('\n'); break;
          case 'r': out->push_back('\r'); break;
          case 't': out->push_back('\t'); break;
          case 'u': {
            uint32_t code_point = 0;
            if (!ParseHex4(&code_point)) {
              Fail("invalid \\u escape");
              return false;
            }
            if (code_point >= 0xD800 && code_point <= 0xDBFF) {
              if (end_ - p_ < 2 || p_[0] != '\\' || p_[1] != 'u') {
                Fail("unpaired high surrogate");
                return false;
              }
              p_ += 2;
              uint32_t low = 0;
              if (!ParseHex4(&low)) {
                Fail("invalid \\u escape in surrogate pair");
                return false;
              }
              if (low < 0xDC00 || low > 0xDFFF) {
                Fail("invalid low surrogate");
                return false;
              }
              code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
            } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
              Fail("unpaired low surrogate");
              return false;
            }
            AppendUtf8(code_point, out);
            break;
          }
          default:
            Fail("invalid escape sequence");
            return false;
        }
        continue;
      }
      if (c < 0x20) {
        Fail("unescaped control character in string");
        return false;
      }
      out->push_back(static_cast<char>(c));
      ++p_;
    }
  }

  std::optional<Value> ParseNumber() {
    const char* start = p_;
    if (p_ != end_ && *p_ == '-') {
      ++p_;
    }
    if (p_ == end_ || !IsDigit(*p_)) {
      return Fail("invalid number, expected a digit");
    }
    if (*p_ == '0') {
      ++p_;
    } else {
      while (p_ != end_ && IsDigit(*p_)) {
        ++p_;
      }
    }
    bool is_real = false;
    if (p_ != end_ && *p_ == '.') {
      is_real = true;
      ++p_;
      if (p_ == end_ || !IsDigit(*p_)) {
        return Fail("invalid number, expected a digit after '.'");
      }
      while (p_ != end_ && IsDigit(*p_)) {
        ++p_;
      }
    }
    if (p_ != end_ && (*p_ == 'e' || *p_ == 'E')) {
      is_real = true;
      ++p_;
      if (p_ != end_ && (*p_ == '+' || *p_ == '-')) {
        ++p_;
      }
      if (p_ == end_ || !IsDigit(*p_)) {
        return Fail("invalid number, expected exponent digits");
      }
      while (p_ != end_ && IsDigit(*p_)) {
        ++p_;
      }
    }

    std::string token(start, static_cast<size_t>(p_ - start));
    if (!is_real) {
      errno = 0;
      char* parse_end = nullptr;
      long long integer = std::strtoll(token.c_str(), &parse_end, 10);
      if (errno == 0 && parse_end == token.c_str() + token.size()) {
        return Value::Int(static_cast<int64_t>(integer));
      }
    }
    errno = 0;
    char* parse_end = nullptr;
    double real = std::strtod(token.c_str(), &parse_end);
    if (parse_end != token.c_str() + token.size() || errno == ERANGE) {
      return Fail("number is out of range");
    }
    return Value::Real(real);
  }

  const char* begin_;
  const char* p_;
  const char* end_;
  std::string* error_;
};

const std::string& EmptyString() {
  static const std::string kEmpty;
  return kEmpty;
}

const Array& EmptyArray() {
  static const Array kEmpty;
  return kEmpty;
}

const Object& EmptyObject() {
  static const Object kEmpty;
  return kEmpty;
}

}  // namespace

Value Value::Null() { return Value(); }

Value Value::Bool(bool value) {
  Value out;
  out.type_ = Type::kBool;
  out.bool_ = value;
  return out;
}

Value Value::Int(int64_t value) {
  Value out;
  out.type_ = Type::kNumber;
  out.int_valued_ = true;
  out.int_ = value;
  out.real_ = static_cast<double>(value);
  return out;
}

Value Value::Real(double value) {
  Value out;
  out.type_ = Type::kNumber;
  out.real_ = value;
  return out;
}

Value Value::Str(std::string value) {
  Value out;
  out.type_ = Type::kString;
  out.string_ = std::move(value);
  return out;
}

Value Value::Arr() { return Arr(0); }

Value Value::Arr(size_t reserve) {
  Value out;
  out.type_ = Type::kArray;
  out.array_.reserve(reserve);
  return out;
}

Value Value::Obj() { return Obj(0); }

Value Value::Obj(size_t reserve) {
  Value out;
  out.type_ = Type::kObject;
  out.object_.reserve(reserve);
  return out;
}

Value Value::RawJson(std::string rendered) {
  Value out;
  out.type_ = Type::kRawJson;
  out.string_ = std::move(rendered);  // reuses the string member, so sizeof(Value) is unchanged
  return out;
}

std::optional<Value> Value::Parse(std::string_view text, std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  Parser parser(text, error);
  return parser.ParseDocument();
}

int64_t Value::AsInt() const {
  if (type_ != Type::kNumber) {
    return 0;
  }
  return int_valued_ ? int_ : static_cast<int64_t>(real_);
}

double Value::AsDouble() const {
  if (type_ != Type::kNumber) {
    return 0;
  }
  return int_valued_ ? static_cast<double>(int_) : real_;
}

const std::string& Value::AsString() const { return type_ == Type::kString ? string_ : EmptyString(); }

const Array& Value::AsArray() const { return type_ == Type::kArray ? array_ : EmptyArray(); }

const Object& Value::AsObject() const { return type_ == Type::kObject ? object_ : EmptyObject(); }

const Value* Value::Find(std::string_view key) const {
  if (type_ != Type::kObject) {
    return nullptr;
  }
  for (const auto& [member_key, member_value] : object_) {
    if (member_key == key) {
      return &member_value;
    }
  }
  return nullptr;
}

void Value::Append(Value value) {
  if (type_ == Type::kArray) {
    array_.push_back(std::move(value));
  }
}

void Value::Reserve(size_t count) {
  if (type_ == Type::kArray) {
    array_.reserve(count);
  } else if (type_ == Type::kObject) {
    object_.reserve(count);
  }
}

void Value::Set(std::string_view key, Value value) {
  if (type_ != Type::kObject) {
    return;
  }
  for (auto& [member_key, member_value] : object_) {
    if (member_key == key) {
      member_value = std::move(value);
      return;
    }
  }
  object_.emplace_back(std::string(key), std::move(value));
}

std::string Value::Dump() const {
  std::string out;
  out.reserve(kDumpReserveHint);
  DumpTo(&out);
  return out;
}

void Value::DumpTo(std::string* out) const {
  switch (type_) {
    case Type::kNull: out->append("null"); break;
    case Type::kBool: out->append(bool_ ? "true" : "false"); break;
    case Type::kNumber:
      if (int_valued_) {
        out->append(std::to_string(int_));
      } else {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", real_);
        out->append(buf);
      }
      break;
    case Type::kString: AppendEscapedString(string_, out); break;
    // Verbatim by design (see RawJson). This case has to exist: the switch has no default and
    // the build turns on no -Wswitch, so leaving it out would silently emit an empty string and
    // corrupt every frame that carries a spliced block.
    case Type::kRawJson: out->append(string_); break;
    case Type::kArray: {
      out->push_back('[');
      for (size_t i = 0; i < array_.size(); ++i) {
        if (i != 0) {
          out->push_back(',');
        }
        array_[i].DumpTo(out);
      }
      out->push_back(']');
      break;
    }
    case Type::kObject: {
      out->push_back('{');
      for (size_t i = 0; i < object_.size(); ++i) {
        if (i != 0) {
          out->push_back(',');
        }
        AppendEscapedString(object_[i].first, out);
        out->push_back(':');
        object_[i].second.DumpTo(out);
      }
      out->push_back('}');
      break;
    }
  }
}

}  // namespace mahjong::json
