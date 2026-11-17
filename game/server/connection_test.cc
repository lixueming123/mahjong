// Unit tests for Connection's outbound buffer bound (connection.h: kMaxOutboxBytes).
//
// The case these pin down is the one the gateway cannot defend by itself: a peer that stops
// reading. `outbox_` is fed by any actor thread and only ever drained by the gateway's `send()`,
// and whether that `send()` succeeds is decided by the peer's receive window. A peer that never
// reads turns every flush into EAGAIN; the gateway retries those (they are not errors) and pushes
// the unsent tail back into the outbox, so the buffer grows for as long as the peer sends
// anything at all — one `ping` per few seconds is enough to keep the idle timeout from firing.
//
// No socket is needed to exercise that: an invalid fd (the same trick lobby_test.cc uses) means
// nothing is ever drained, which is exactly the state a non-reading peer produces.
//
// Deliberately a target of its own rather than a case inside mahjong_lobby_test: this is a
// property of Connection, and it must stay runnable without the actor world, the lobby or a timer.

#include "connection.h"

#include <atomic>
#include <string>

#include <gtest/gtest.h>

namespace mahjong {
namespace {

// A connection whose frames can never be flushed: fd -1 and no wake pipe.
std::shared_ptr<Connection> MakeStuckConnection() {
  return std::make_shared<Connection>(/*fd=*/-1, /*id=*/1);
}

TEST(ConnectionOutboxTest, AcceptsFramesBelowTheLimit) {
  auto conn = MakeStuckConnection();
  conn->SendFrame(R"({"type":3})");
  EXPECT_FALSE(conn->close_requested()) << "one small frame must not trip the bound";
  EXPECT_EQ(conn->outbox_bytes(), std::string(R"({"type":3})").size() + 1);  // + terminator
}

// The bound itself: pushing past kMaxOutboxBytes must close the connection, not grow the buffer.
TEST(ConnectionOutboxTest, ClosesWhenTheOutboxWouldExceedTheLimit) {
  auto conn = MakeStuckConnection();

  // Fill to just under the limit with frames well under the cap, so the crossing frame is the one
  // that trips it rather than a single oversized one.
  const std::string chunk(64 * 1024, 'x');
  size_t queued = 0;
  while (queued + chunk.size() + 1 < kMaxOutboxBytes) {
    conn->SendFrame(chunk);
    queued += chunk.size() + 1;
  }
  ASSERT_FALSE(conn->close_requested()) << "filled to " << queued << " bytes, still under the cap";
  ASSERT_LE(conn->outbox_bytes(), kMaxOutboxBytes);

  // One more frame cannot fit: it is dropped and the connection is asked to close.
  conn->SendFrame(chunk);
  EXPECT_TRUE(conn->close_requested()) << "crossing kMaxOutboxBytes must close the connection";
  EXPECT_EQ(conn->close_reason(), "outbound buffer full");

  // The dropped frame must not have been appended: the outbox stays under the bound.
  EXPECT_LE(conn->outbox_bytes(), kMaxOutboxBytes);
}

// After the bound trips, further frames are refused too (the connection is on its way out) and the
// buffer must not keep growing.
TEST(ConnectionOutboxTest, StopsGrowingAfterTheLimitIsHit) {
  auto conn = MakeStuckConnection();
  const std::string chunk(64 * 1024, 'x');
  // Bounded loop on purpose: kMaxOutboxBytes / chunk is ~64 iterations, so 256 cannot be reached by
  // a working cap. A loop keyed on `!close_requested()` would hang forever if the cap ever went
  // away, and a hung test is worse than a failing one.
  for (int i = 0; i < 256 && !conn->close_requested(); ++i) {
    conn->SendFrame(chunk);
  }
  ASSERT_TRUE(conn->close_requested()) << "the outbox grew past kMaxOutboxBytes without closing";

  const size_t after_trip = conn->outbox_bytes();
  for (int i = 0; i < 100; ++i) {
    conn->SendFrame(chunk);
  }
  EXPECT_EQ(conn->outbox_bytes(), after_trip) << "a closed connection must not buffer more frames";
}

// A single frame larger than the whole budget is the same fault and must not be appended either.
TEST(ConnectionOutboxTest, RejectsOneOversizedFrame) {
  auto conn = MakeStuckConnection();
  conn->SendFrame(std::string(kMaxOutboxBytes + 1, 'x'));
  EXPECT_TRUE(conn->close_requested());
  EXPECT_EQ(conn->outbox_bytes(), 0u) << "the oversized frame must not be queued at all";
}

// The bound must not fire on a legitimate burst: a client that stops reading for a moment while a
// whole match is broadcast must still get its frames. Sized from the measured worst case (~2.3 MB
// for a classic match fanned out to four seats, which is why the cap is 4 MB).
TEST(ConnectionOutboxTest, HoldsAWholeMatchWorthOfFrames) {
  auto conn = MakeStuckConnection();
  const std::string state_sync(4096, 'x');
  for (int i = 0; i < 512; ++i) {  // ~2 MB
    conn->SendFrame(state_sync);
  }
  EXPECT_FALSE(conn->close_requested()) << "a ~2MB burst is a slow reader, not a hostile one";
  EXPECT_GT(conn->outbox_bytes(), 2u * 1024 * 1024);
}

// SendFrameThenClose's own reason must win: it is answered to the client, and the bound is only a
// fallback. Ordering matters — the frame is appended before the close is requested.
TEST(ConnectionOutboxTest, SendFrameThenCloseKeepsItsOwnReason) {
  auto conn = MakeStuckConnection();
  conn->SendFrameThenClose(R"({"type":99})", "peer hung up");
  EXPECT_TRUE(conn->close_requested());
  EXPECT_EQ(conn->close_reason(), "peer hung up");
}

// The path that lets the buffer escape the bound if draining is not accounted for. DrainOutbox
// hands the whole outbox to the writer, but that memory is still live: if only the queue were
// counted, a peer that never reads would empty the queue on every pass and let the producers refill
// it to the cap in between, so the writer's copy plus the queue would add up to twice the bound and
// grow that way round after round.
TEST(ConnectionOutboxTest, InFlightBatchCountsAgainstTheBound) {
  auto conn = MakeStuckConnection();

  // Fill to just under the cap, then drain it — this is one flush cycle with a peer that never
  // reads, so the batch stays in flight (FinishWrite is the writer's "I am done with this").
  const std::string chunk(64 * 1024, 'x');
  while (conn->outbox_bytes() + chunk.size() + 1 <= kMaxOutboxBytes) {
    conn->SendFrame(chunk);
  }
  ASSERT_FALSE(conn->close_requested());

  std::string drained;
  conn->DrainOutbox(&drained);
  EXPECT_EQ(conn->outbox_bytes(), 0u) << "DrainOutbox must hand the whole buffer over";
  EXPECT_EQ(conn->in_flight_bytes(), drained.size()) << "the drained batch is still live memory";

  // Refill while the batch is in flight: the producers must hit the cap after the remaining budget
  // rather than after another full kMaxOutboxBytes.
  while (!conn->close_requested()) {
    conn->SendFrame(chunk);
  }
  EXPECT_EQ(conn->close_reason(), "outbound buffer full");
  EXPECT_LE(conn->in_flight_bytes() + conn->outbox_bytes(), kMaxOutboxBytes)
      << "queued plus in-flight must stay within kMaxOutboxBytes";

  // Once the writer finishes, that budget is available again.
  conn->FinishWrite();
  EXPECT_EQ(conn->in_flight_bytes(), 0u);
}

// Finishing a write frees the in-flight budget, so a merely slow reader is not closed: the batch
// it just wrote no longer counts, and the next append fits.
TEST(ConnectionOutboxTest, FinishWriteReleasesTheBudget) {
  auto conn = MakeStuckConnection();
  conn->SendFrame("first");
  std::string drained;
  conn->DrainOutbox(&drained);
  ASSERT_EQ(drained, "first\n");
  EXPECT_EQ(conn->in_flight_bytes(), drained.size());

  conn->FinishWrite();
  EXPECT_FALSE(conn->close_requested());
  EXPECT_EQ(conn->outbox_bytes(), 0u);

  // The ordering guarantee the outbox owes: a frame queued after the batch is only ever written
  // after it, because the writer drains again once the previous batch is done.
  conn->SendFrame("second");
  std::string all;
  conn->DrainOutbox(&all);
  EXPECT_EQ(all, "second\n");
}

}  // namespace
}  // namespace mahjong
