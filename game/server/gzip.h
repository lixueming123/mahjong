// gzip, for replay storage and for the wire.
//
// Compression lives in the store, not the room: ReplayActor has its own single-thread pool for
// exactly this kind of CPU work, so a game thread only pays for the JSON it already built.
//
// gzip rather than zlib/deflate because the browser undoes it with `DecompressionStream('gzip')`,
// no library needed. Level 6 is zlib's default: within 25% of -9 for a third of the time, and this
// runs once per settled round on one hand's JSON.

#pragma once

#include <string>
#include <string_view>

namespace mahjong {

// 1f 8b: the two bytes every gzip stream starts with. The read path uses this to recognise a
// recording written before the store compressed.
inline bool LooksGzip(std::string_view bytes) {
  return bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0x1F &&
         static_cast<unsigned char>(bytes[1]) == 0x8B;
}

// gzip-wrapped deflate. An empty return means zlib failed; the caller treats it as "could not
// record".
std::string GzipCompress(std::string_view bytes, int level = 6);

// The inverse, needed because a match is stored as one part per hand: each part is inflated before
// the read path glues them back into one document.
//
// An empty return means |bytes| was not a gzip stream (or zlib failed), which the caller answers as
// "no such recording".
std::string GzipDecompress(std::string_view bytes);

}  // namespace mahjong
