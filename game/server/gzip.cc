#include "gzip.h"

#include <zlib.h>

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

}  // namespace mahjong
