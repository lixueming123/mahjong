// Monotonic / wall-clock helpers.
//
// PROTOCOL.md §1.5: deadline_ms is epoch milliseconds (UTC) for the wire, while all
// internal timeout decisions must use a monotonic clock.

#pragma once

#include <chrono>
#include <cstdint>

namespace mahjong::clock {

inline int64_t MonotonicMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline int64_t EpochMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace mahjong::clock
