// gzip, for replay storage and for the wire (PROTOCOL v2.3 §5.18, encoding since v2.6).
//
// Why the store compresses rather than the room: the room's step handler is on a game thread, and
// a 1.5 MB serialize+deflate is ~25 ms — five frames of one table's timeline. ReplayActor owns a
// dedicated single-thread pool for exactly this kind of work (replay_store.h), so the compression
// lands there and the room keeps paying only for the JSON it already had to build.
//
// Why gzip and not zlib/deflate: the browser has to undo it, and `DecompressionStream('gzip')` is
// the one decoder every target supports without a library (Chrome 80+, Firefox 113+, Safari 16.4+).
// Measured on the sample replays (10.5 MB of JSON): gzip -6 gives 24x, gzip -9 26x, brotli -11
// 37x. Level 9 is not worth 3x the CPU for 7% on a file written once per round, and brotli would
// buy 22% more at the cost of a WASM decoder in the client.

#pragma once

#include <string>
#include <string_view>

namespace mahjong {

// 1f 8b: the two bytes every gzip stream starts with (RFC 1952 §2.3.1). The read path uses this to
// recognise a recording written before the store compressed, instead of guessing from content.
inline bool LooksGzip(std::string_view bytes) {
  return bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0x1F &&
         static_cast<unsigned char>(bytes[1]) == 0x8B;
}

// gzip-wrapped deflate. Level 6 is zlib's default: on a replay it lands within 25% of -9 for a
// third of the time (12 ms vs 27 ms on 1.5 MB), and this runs once per settled round.
// An empty return means zlib failed; the caller treats it as "could not record".
//
// There is deliberately no decompressor here: the server only ever writes (the client inflates,
// via `DecompressionStream('gzip')`), and the legacy read path serves an old plain file as-is
// instead of inflating it. replay_e2e_test.py is what proves the round trip is a real gzip.
std::string GzipCompress(std::string_view bytes, int level = 6);

}  // namespace mahjong
