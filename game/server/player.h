// Player identity shared by the lobby, rooms and connections.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_set>

namespace mahjong {

struct PlayerInfo {
  std::string player_id;
  std::string name;
  // ACCOUNT_DESIGN.md §1.2: the durable account behind this session, 0 for a guest or a bot.
  // Only match recording cares; it never goes on the wire.
  uint64_t account_id = 0;
};

// ACCOUNT_DESIGN.md §1.2: an account's player_id is derived from its durable account id, so
// it survives a server restart, unlike the per-process guest ids ("u-000042" vs "p-3f2a1c").
inline std::string AccountPlayerId(uint64_t account_id) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "u-%06llu", static_cast<unsigned long long>(account_id));
  return std::string(buffer);
}

// Allocates the opaque per-process player ids of PROTOCOL.md §5.1 ("p-3f2a1c").
// Multiplying the counter by an odd constant is a bijection modulo 2^24, so the ids are
// unique for the first 16.7M connections while looking opaque to clients.
class PlayerIdAllocator {
 public:
  std::string Next() {
    uint64_t counter = counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    auto hashed = static_cast<uint32_t>(counter * 2654435761u) & 0xFFFFFFu;
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "p-%06x", hashed);
    return std::string(buffer);
  }

 private:
  std::atomic<uint64_t> counter_{0};
};

// ACCOUNT_DESIGN.md §1.3: one account, one live connection. The login is asynchronous, so the
// claim and the release happen on whichever actor thread handles the result message — hence
// the mutex, and hence a test-and-set rather than a check followed by an insert.
class SessionRegistry {
 public:
  // Returns false when the account is already online elsewhere.
  bool TryAcquire(uint64_t account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return online_.insert(account_id).second;
  }

  void Release(uint64_t account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    online_.erase(account_id);
  }

  bool Contains(uint64_t account_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return online_.contains(account_id);
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_set<uint64_t> online_;
};

}  // namespace mahjong
