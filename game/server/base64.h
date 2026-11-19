// Base64 (RFC 4648), for one purpose: a replay payload is gzip bytes and a replay frame's `data`
// is a JSON string, so the bytes have to become text before they can be sliced into frames.
//
// Base64 costs 33% over the compressed size and is still ~20x smaller than the plain JSON text it
// replaces (10.5 MB of sample replays: 12.9 MB of framed plain text -> 595 KB of framed base64).
// The alternative — a binary frame — would need a second transport shape for one message.
//
// Header-only and dependency-free on purpose: PlayerActor is what frames the answer, and adding
// zlib to every target that links the player for one table lookup is not worth it.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace mahjong {

inline std::string Base64Encode(std::string_view bytes) {
  constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  const auto byte_at = [&bytes](size_t index) { return static_cast<unsigned char>(bytes[index]); };

  size_t offset = 0;
  for (; offset + 3 <= bytes.size(); offset += 3) {
    const unsigned int group = (byte_at(offset) << 16) | (byte_at(offset + 1) << 8) | byte_at(offset + 2);
    out.push_back(kAlphabet[(group >> 18) & 0x3F]);
    out.push_back(kAlphabet[(group >> 12) & 0x3F]);
    out.push_back(kAlphabet[(group >> 6) & 0x3F]);
    out.push_back(kAlphabet[group & 0x3F]);
  }
  // Tail: one or two leftover bytes, padded so the length stays a multiple of four.
  const size_t remaining = bytes.size() - offset;
  if (remaining == 1) {
    const unsigned int group = byte_at(offset) << 16;
    out.push_back(kAlphabet[(group >> 18) & 0x3F]);
    out.push_back(kAlphabet[(group >> 12) & 0x3F]);
    out.append("==");
  } else if (remaining == 2) {
    const unsigned int group = (byte_at(offset) << 16) | (byte_at(offset + 1) << 8);
    out.push_back(kAlphabet[(group >> 18) & 0x3F]);
    out.push_back(kAlphabet[(group >> 12) & 0x3F]);
    out.push_back(kAlphabet[(group >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

}  // namespace mahjong
