// gzip, for replay storage and for the wire (PROTOCOL v2.3 §5.18, encoding since v2.6).
//
// Why the store compresses rather than the room: the room's step handler is on a game thread, and a
// deflate is the one part of a write that is pure CPU on a buffer somebody else could own.
// ReplayActor has a dedicated single-thread pool for exactly this kind of work (replay_store.h),
// so the compression lands there and the room keeps paying only for the JSON it already had to
// build. Since v2.7 that JSON is one hand (~40-160 KB → 4-5 KB), not the whole match, so the cost
// per write is a small fraction of what this comment originally measured.
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
// third of the time, and this runs once per settled round on one hand's JSON.
// An empty return means zlib failed; the caller treats it as "could not record".
std::string GzipCompress(std::string_view bytes, int level = 6);

// The inverse, for one reason: a match is recorded as one part per hand (replay.h
// PartFileNameFor), so the read path has to inflate each part before it can glue them back into
// the single document §5.18 defines. Nothing in the write path decompresses anything — the client
// still does its own inflating, via `DecompressionStream('gzip')`.
//
// An empty return means |bytes| was not a gzip stream (or zlib failed); the caller treats it as
// "no such recording". An empty document and a failure are the same answer on purpose: every part
// this server writes carries at least the file header.
std::string GzipDecompress(std::string_view bytes);

}  // namespace mahjong
