// Replay payload plumbing: base64 (base64.h), gzip (gzip.cc) and the §5.18 framing that carries
// the result. All three are pure functions, so this needs no actor, no disk and no sockets.
//
// Why these matter more than they look: the payload path is the only place in the server where a
// byte-level mistake is invisible to the compiler and silent at run time — base64 that drops the
// tail, gzip that omits the wrapper, a splitter that cuts inside a character. Every one of those
// produces a replay the browser refuses, and the failure would show up as "the replay is broken"
// long after the change that caused it.

#include <gtest/gtest.h>
#include <zlib.h>

#include <string>
#include <string_view>
#include <vector>

#include "base64.h"
#include "gzip.h"
#include "protocol.h"

namespace mahjong {
namespace {

// Inflates with zlib directly rather than through GzipDecompress: this is the test that proves the
// stream is a real gzip stream (wrapper included) and not bare deflate, so it must not borrow the
// helper whose contract it is checking. GzipDecompress itself is exercised by the part-merge tests
// in replay_test.cc.
std::string InflateWithZlib(std::string_view compressed) {
  z_stream stream {};
  EXPECT_EQ(inflateInit2(&stream, 15 + 16), Z_OK);  // 16 = "expect the gzip wrapper"
  std::string out(compressed.size() * 4 + 1024, '\0');
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = reinterpret_cast<Bytef*>(out.data());
  stream.avail_out = static_cast<uInt>(out.size());
  const int result = inflate(&stream, Z_FINISH);
  const size_t produced = stream.total_out;
  inflateEnd(&stream);
  EXPECT_EQ(result, Z_STREAM_END);
  out.resize(produced);
  return out;
}

TEST(Base64Test, EncodesEveryTailLength) {
  // The three cases a base64 encoder can get wrong: no leftover, one byte, two bytes.
  EXPECT_EQ(Base64Encode(""), "");
  EXPECT_EQ(Base64Encode("f"), "Zg==");
  EXPECT_EQ(Base64Encode("fo"), "Zm8=");
  EXPECT_EQ(Base64Encode("foo"), "Zm9v");
  EXPECT_EQ(Base64Encode("foob"), "Zm9vYg==");
  EXPECT_EQ(Base64Encode("fooba"), "Zm9vYmE=");
  EXPECT_EQ(Base64Encode("foobar"), "Zm9vYmFy");
}

TEST(Base64Test, EncodesBinaryWithoutLosingHighBytes) {
  // The alphabet's index values must come from the raw byte, not from a char that may be signed.
  std::string bytes;
  for (int value = 0; value < 256; ++value) {
    bytes.push_back(static_cast<char>(value));
  }
  const std::string encoded = Base64Encode(bytes);
  EXPECT_EQ(encoded.size(), (bytes.size() + 2) / 3 * 4);
  // 0xFF 0xFF 0xFF -> "////", the only 3-byte group that reaches the end of the alphabet.
  bytes.assign(3, static_cast<char>(0xFF));
  EXPECT_EQ(Base64Encode(bytes), "////");
}

TEST(GzipTest, RoundTripsThroughTheStdlibDecoder) {
  // The client inflates with `DecompressionStream('gzip')`, so the stream has to be the gzip
  // wrapper, not bare deflate: `LooksGzip` checks the two magic bytes that decoder looks at first.
  const std::string text = R"({"format":"mahjong-replay","steps":[]})";
  const std::string compressed = GzipCompress(text);
  ASSERT_GE(compressed.size(), 2u);
  EXPECT_TRUE(LooksGzip(compressed));
  // NOTE: no size assertion here. gzip's header and trailer are ~18 bytes, so a 38-byte input
  // legitimately grows. "Smaller than the input" is a property of real replays, not of gzip.

  // And it really is a valid stream: zlib's own inflate has to accept it.
  const std::string inflated = InflateWithZlib(compressed);
  EXPECT_EQ(inflated, text);
}

TEST(GzipTest, CompressesAReplayLikeText) {
  // A real recording is mostly repeated key names, so the ratio is far better than the text above.
  // Building one here is cheap and pins the order of magnitude the wire size depends on.
  std::string text;
  for (int index = 0; index < 500; ++index) {
    text += R"({"t":1,"f":[[[0,1,2,3],{"type":20,"current_seat":0,"hand":["1m","3m"]}]]})";
  }
  const std::string compressed = GzipCompress(text);
  ASSERT_FALSE(compressed.empty());
  EXPECT_GT(static_cast<double>(text.size()) / static_cast<double>(compressed.size()), 10.0);
}

TEST(GzipTest, DecompressInvertsCompress) {
  // The read path inflates every hand part before merging them (replay_store.cc), so this is the
  // decoder half of the one round trip the server itself relies on. Multi-byte text and a payload
  // larger than the decoder's internal chunk are both included on purpose.
  std::string text = R"({"format":"mahjong-replay","room_id":"r-2o5endvwb8zcx","seats":[{"name":"小明"}]})";
  // 8 000 * 12 bytes ("小明和牌" is four characters, three bytes each) clears the decoder's
  // 64 KB chunk, so the loop in GzipDecompress really runs more than once.
  for (int index = 0; index < 8000; ++index) {
    text += "小明和牌";
  }
  ASSERT_GT(text.size(), 64u * 1024u);
  const std::string inflated = GzipDecompress(GzipCompress(text));
  EXPECT_EQ(inflated, text);
}

TEST(GzipTest, DecompressRefusesWhatIsNotAGzipStream) {
  // Every caller treats an empty answer as "no such recording": half a part spliced into a
  // timeline would be a broken file, which is worse than an honest not-found.
  EXPECT_EQ(GzipDecompress(""), "");
  EXPECT_EQ(GzipDecompress("not a gzip stream"), "");
  // A truncated stream: the header is valid, the trailer is gone.
  const std::string compressed = GzipCompress(R"({"steps":[]})");
  ASSERT_GT(compressed.size(), 20u);
  EXPECT_EQ(GzipDecompress(compressed.substr(0, compressed.size() - 8)), "");
}

TEST(ReplayFramesTest, EncodingIsOmittedForAPlainPayload) {
  // An old recording (or an old server) sends the JSON text itself: the frame must keep the exact
  // v2.3 shape, or a client that predates `encoding` would see an unknown key on every frame.
  const std::string frame = protocol::ReplayFrame("r-2o5endvwb8zcx", /*chunk=*/0, /*chunks=*/1,
                                                  /*encoding=*/"", /*data=*/"{}");
  EXPECT_EQ(frame.find("encoding"), std::string::npos);
  EXPECT_NE(frame.find("\"chunk\":0"), std::string::npos);
  EXPECT_NE(frame.find("\"chunks\":1"), std::string::npos);
}

TEST(ReplayFramesTest, EncodingIsCarriedWhenThePayloadIsCompressed) {
  const std::string frame =
      protocol::ReplayFrame("r-2o5endvwb8zcx", /*chunk=*/0, /*chunks=*/1, protocol::kReplayEncodingGzip, "H4sI");
  EXPECT_NE(frame.find("\"encoding\":\"gzip\""), std::string::npos);
}

TEST(ReplayFramesTest, SplitsBase64WithoutBreakingItAndNeverExceedsTheChunkLimit) {
  // 100 KB of base64 is ~7 frames at the 16 KB limit. Base64 is ASCII, so no cut can land inside a
  // character, but the splitter still has to keep every slice within the budget.
  std::string payload;
  payload.reserve(100 * 1024);
  while (payload.size() < 100u * 1024u) {
    payload.append("QUJDREVGR0hJSktMTU5PUA==");
  }
  const std::vector<std::string> frames =
      protocol::SplitReplayFrames("r-2o5endvwb8zcx", protocol::kReplayEncodingGzip, payload);
  ASSERT_GT(frames.size(), 1u);
  for (const std::string& frame : frames) {
    // The whole NDJSON line has to fit the client's 64 KB cap (§1.2), not just the payload.
    EXPECT_LT(frame.size(), protocol::kMaxFrameBytes);
    EXPECT_NE(frame.find("\"encoding\":\"gzip\""), std::string::npos);
  }
}

TEST(ReplayFramesTest, OneChunkPerSliceAndNoEmptyLeadingFrame) {
  // The splitter used to start its cut list with a 0 sentinel and index it as `cuts[i - 1]`, which
  // emitted an empty `chunk = 0` frame on every answer and inflated `chunks` by one. Both are
  // client-visible: the empty slice wastes a frame, and a wrong `chunks` makes the client wait for
  // a piece that never comes.
  const std::string payload = "0123456789";  // 10 bytes, 4-byte budget -> 3 slices
  const std::vector<std::string> frames = protocol::SplitReplayFrames("r-2o5endvwb8zcx", "", payload, 4);
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_NE(frames[0].find("\"chunk\":0,\"chunks\":3,\"data\":\"0123\""), std::string::npos) << frames[0];
  EXPECT_NE(frames[1].find("\"chunk\":1,\"chunks\":3,\"data\":\"4567\""), std::string::npos) << frames[1];
  EXPECT_NE(frames[2].find("\"chunk\":2,\"chunks\":3,\"data\":\"89\""), std::string::npos) << frames[2];
}

TEST(ReplayFramesTest, EmptyPayloadStillAnswersOneFrame) {
  // §5.18 requires chunks >= 1, so an empty payload must not produce zero frames.
  const std::vector<std::string> frames = protocol::SplitReplayFrames("r-2o5endvwb8zcx", "", "");
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_NE(frames[0].find("\"chunks\":1"), std::string::npos);
  EXPECT_NE(frames[0].find("\"data\":\"\""), std::string::npos);
}

TEST(ReplayFramesTest, NeverCutsAMultiByteCharacter) {
  // A legacy plain-text recording carries player names, so the splitter has to land on a UTF-8
  // lead byte. 3 bytes per character with a 4-byte budget means every cut would land mid-character
  // if the walk-back were missing.
  std::string text;
  for (int index = 0; index < 20; ++index) {
    text.append("小明");  // 6 bytes
  }
  const std::vector<std::string> frames =
      protocol::SplitReplayFrames("r-2o5endvwb8zcx", /*encoding=*/"", text, /*max_bytes=*/4);
  std::string joined;
  for (const std::string& frame : frames) {
    const std::string needle = "\"data\":\"";
    const size_t begin = frame.find(needle);
    ASSERT_NE(begin, std::string::npos);
    const size_t start = begin + needle.size();
    const size_t end = frame.rfind('"');
    ASSERT_LT(start, end);
    const std::string slice = frame.substr(start, end - start);
    // Every slice must be whole characters: a lead byte followed by continuation bytes.
    size_t offset = 0;
    while (offset < slice.size()) {
      const auto lead = static_cast<unsigned char>(slice[offset]);
      ASSERT_NE(lead & 0xC0, 0x80) << "a slice starts inside a character";
      const size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
      offset += length;
    }
    EXPECT_EQ(offset, slice.size());
    joined.append(slice);
  }
  EXPECT_EQ(joined, text);
}

}  // namespace
}  // namespace mahjong
