// UTF-8 helpers for the NDJSON gateway (PROTOCOL.md §1.2).

#pragma once

#include <cstddef>
#include <string_view>

namespace mahjong::utf8 {

// Strict UTF-8 validation: rejects overlong encodings, surrogate code points and
// code points above U+10FFFF, and requires the shortest possible encoding.
inline bool IsValid(std::string_view text) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
  size_t i = 0;
  const size_t size = text.size();
  while (i < size) {
    unsigned char c = bytes[i];
    size_t extra = 0;
    unsigned int code_point = 0;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      extra = 1;
      code_point = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
      code_point = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      code_point = c & 0x07u;
    } else {
      return false;
    }
    if (i + extra >= size) {
      return false;
    }
    for (size_t k = 1; k <= extra; ++k) {
      unsigned char continuation = bytes[i + k];
      if ((continuation & 0xC0) != 0x80) {
        return false;
      }
      code_point = (code_point << 6) | (continuation & 0x3Fu);
    }
    // Reject overlong forms, UTF-16 surrogates and out-of-range code points.
    if (extra == 1 && code_point < 0x80) {
      return false;
    }
    if (extra == 2 && code_point < 0x800) {
      return false;
    }
    if (extra == 3 && code_point < 0x10000) {
      return false;
    }
    if (code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
      return false;
    }
    i += extra + 1;
  }
  return true;
}

// Counts code points; callers must have validated the text first.
inline size_t CountCodePoints(std::string_view text) {
  size_t count = 0;
  for (char raw : text) {
    auto c = static_cast<unsigned char>(raw);
    if ((c & 0xC0) != 0x80) {
      ++count;
    }
  }
  return count;
}

// True when the text contains a C0 control character, DEL or a newline
// (PROTOCOL.md §4.1 forbids them in login names).
inline bool HasControlCharacter(std::string_view text) {
  for (char raw : text) {
    auto c = static_cast<unsigned char>(raw);
    if (c < 0x20 || c == 0x7F) {
      return true;
    }
  }
  return false;
}

}  // namespace mahjong::utf8
