#include "gzip.h"

#include <zlib.h>

#include <array>
#include <cstddef>

namespace mahjong {

// windowBits = 15 + 16 asks zlib for the gzip wrapper rather than the bare zlib one: the browser
// decoder is `DecompressionStream('gzip')`, and it rejects a raw deflate stream. Level 6 is the
// library default (see gzip.h for why not 9).
std::string GzipCompress(std::string_view bytes, int level) {
  z_stream stream {};
  if (deflateInit2(&stream, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    return {};
  }
  // deflate's worst case is slightly over 1.0x the input, and an upper bound for the whole stream.
  // Sizing the buffer to it means the common path is one Z_FINISH call with no loop and no
  // partially-flushed state to reason about.
  std::string out;
  out.resize(deflateBound(&stream, static_cast<uLong>(bytes.size())));

  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()));
  stream.avail_in = static_cast<uInt>(bytes.size());
  stream.next_out = reinterpret_cast<Bytef*>(out.data());
  stream.avail_out = static_cast<uInt>(out.size());

  const int result = deflate(&stream, Z_FINISH);
  const size_t produced = stream.total_out;
  deflateEnd(&stream);
  if (result != Z_STREAM_END) {
    return {};  // the caller answers INTERNAL; an empty payload is never a valid replay
  }
  out.resize(produced);
  return out;
}

// windowBits 15 + 16 again: the stream has the gzip wrapper. Inflate is a loop rather than one
// call because the output size is not known up front; the chunk is deliberately larger than a
// part (a hand is ~5-30 KB of JSON, see replay.h) so a part is one or two rounds of it.
//
// A truncated or non-gzip stream is refused as a whole: half a part would splice a broken frame
// into the middle of a document, which the player would report as a bad recording rather than as
// the missing file it is.
std::string GzipDecompress(std::string_view bytes) {
  z_stream stream {};
  if (inflateInit2(&stream, 15 + 16) != Z_OK) {
    return {};
  }
  std::string out;
  std::array<char, 64 * 1024> chunk {};
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()));
  stream.avail_in = static_cast<uInt>(bytes.size());
  int result = Z_OK;
  while (result == Z_OK) {
    stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
    stream.avail_out = static_cast<uInt>(chunk.size());
    result = inflate(&stream, Z_NO_FLUSH);
    if (result != Z_OK && result != Z_STREAM_END) {
      inflateEnd(&stream);
      return {};
    }
    out.append(chunk.data(), chunk.size() - stream.avail_out);
  }
  inflateEnd(&stream);
  if (out.empty()) {
    return {};
  }
  return out;
}

}  // namespace mahjong
