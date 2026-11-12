// Connection-failure policy for the MySQL pool: error classification, the circuit breaker that
// keeps a dead MySQL from charging every request a 3s connect timeout, and the one-slot-one-thread
// bookkeeping the pool relies on.
//
// Deliberately dependency free — no <mysql.h>, no ex_actor, no game headers — so the unit test can
// link it on machines without libmysqlclient (the same reason account_test.cc exists). Everything
// here is pure state or a pure function, and the clock is always passed in as |now_ms| so a test
// can drive it without sleeping.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace mahjong {

// How a MySQL error should be treated. Only the first three arms feed the circuit breaker: they
// mean "this shard cannot serve right now". kDuplicate and kQueryError are per-statement facts —
// the connection survives and the next statement is free to succeed.
enum class DbFailureClass {
  kNone,
  // 2006 CR_SERVER_GONE_ERROR, 2013 CR_SERVER_LOST, 2014 CR_COMMANDS_OUT_OF_SYNC,
  // 1927 ER_CONNECTION_KILLED, 1053 ER_SERVER_SHUTDOWN. The socket is gone or the session state
  // is unknown, so the connection must be closed rather than reused.
  kConnectionLost,
  // 2002/2003/2005: mysql_real_connect never produced a socket (server down, DNS, firewall).
  kConnectFailed,
  // 1044/1045/1049: wrong user, password or database. Retrying cannot help; the operator has to
  // fix the configuration, so this is logged at ERROR and still counts as unavailable.
  kConfig,
  // 1062 ER_DUP_ENTRY: a normal answer for Register and for a replayed match result.
  kDuplicate,
  // Anything else (syntax, data too long, ...): the connection is still usable.
  kQueryError,
};

// The numbers are the MySQL client/server error codes (include/errmsg.h, include/mysqld_error.h).
inline DbFailureClass ClassifyMysqlError(unsigned int error_code) {
  switch (error_code) {
    case 1053:  // ER_SERVER_SHUTDOWN
    case 1927:  // ER_CONNECTION_KILLED
    case 2006:  // CR_SERVER_GONE_ERROR
    case 2013:  // CR_SERVER_LOST
    case 2014:  // CR_COMMANDS_OUT_OF_SYNC
      return DbFailureClass::kConnectionLost;
    case 2002:  // CR_CONNECTION_ERROR
    case 2003:  // CR_CONN_HOST_ERROR
    case 2005:  // CR_UNKNOWN_HOST
      return DbFailureClass::kConnectFailed;
    case 1044:  // ER_DBACCESS_DENIED_ERROR
    case 1045:  // ER_ACCESS_DENIED_ERROR
    case 1049:  // ER_BAD_DB_ERROR
      return DbFailureClass::kConfig;
    case 1062:  // ER_DUP_ENTRY
      return DbFailureClass::kDuplicate;
    default:
      return error_code == 0 ? DbFailureClass::kNone : DbFailureClass::kQueryError;
  }
}

// True when the lease's connection must be closed instead of recycled. A configuration error
// leaves a perfectly good socket behind, so it does not belong here.
inline bool BreaksConnection(DbFailureClass reason) { return reason == DbFailureClass::kConnectionLost; }

// True when the failure means "accounts are unavailable" (ACCOUNT_DESIGN.md §2.3), which is also
// the condition the circuit breaker counts.
inline bool CountsAsUnavailable(DbFailureClass reason) {
  return reason == DbFailureClass::kConnectionLost || reason == DbFailureClass::kConnectFailed ||
         reason == DbFailureClass::kConfig;
}

inline std::string_view ToString(DbFailureClass reason) {
  switch (reason) {
    case DbFailureClass::kNone:
      return "ok";
    case DbFailureClass::kConnectionLost:
      return "connection_lost";
    case DbFailureClass::kConnectFailed:
      return "connect_failed";
    case DbFailureClass::kConfig:
      return "config";
    case DbFailureClass::kDuplicate:
      return "duplicate";
    case DbFailureClass::kQueryError:
      return "query_error";
  }
  return "unknown";
}

struct FailureBudgetOptions {
  // Consecutive failures that open the circuit. Clamped to >= 1 by FailureBudget.
  uint32_t failure_threshold = 2;
  // cooldown = min(max(base * 2^(failures - threshold), floor), max). No jitter: the state machine
  // has to be deterministic to be unit-testable, and one process per shard does not need
  // thundering-herd protection.
  int64_t cooldown_base_ms = 500;
  int64_t cooldown_max_ms = 30000;
  // Lower bound on the first cooldown, and the value the doubling escalates from. Zero here,
  // because a pure state machine has no idea what one attempt costs; DbPool sets it to the connect
  // timeout (see its constructor for why).
  int64_t cooldown_floor_ms = 0;
};

// One instance per pool slot, touched only by that slot's thread, so it holds no atomics.
//
//   closed --(failures >= threshold)--> open --(cooldown expired)--> half-open (one probe)
//     ^                                  ^                              |
//     +------------ success -------------+---------- failure -----------+
class FailureBudget {
 public:
  enum class Verdict {
    kAllow,       // closed: go ahead
    kAllowProbe,  // half-open: this attempt is the only probe in flight
    kReject,      // open: fail fast, do not touch the socket
  };

  FailureBudget() = default;
  explicit FailureBudget(FailureBudgetOptions options);

  // Called before any socket work. In the half-open state exactly one caller gets kAllowProbe.
  Verdict BeginAttempt(int64_t now_ms);
  // The slot can serve again: close the circuit and forget the failure streak.
  void OnSuccess(int64_t /*now_ms*/);
  // Count one failure; opens (or re-opens with a doubled cooldown) once the threshold is reached.
  void OnFailure(int64_t now_ms);

  bool circuit_open(int64_t now_ms) const { return cooldown_until_ms_ > now_ms; }
  int64_t cooldown_until_ms() const { return cooldown_until_ms_; }
  uint32_t consecutive_failures() const { return consecutive_failures_; }
  // How many times the circuit tripped (initial open plus every failed probe): a pool metric.
  uint32_t open_count() const { return open_count_; }

 private:
  int64_t CooldownFor(uint32_t failures) const;

  FailureBudgetOptions options_ {};
  uint32_t consecutive_failures_ = 0;
  uint32_t open_count_ = 0;
  // 0 means "closed"; a non-zero value is the monotonic deadline of the current cooldown.
  int64_t cooldown_until_ms_ = 0;
  // Set while a half-open probe is outstanding, so a second request cannot race it.
  bool probe_in_flight_ = false;
};

inline FailureBudget::FailureBudget(FailureBudgetOptions options) : options_(options) {
  options_.failure_threshold = std::max<uint32_t>(1, options_.failure_threshold);
  options_.cooldown_base_ms = std::max<int64_t>(1, options_.cooldown_base_ms);
  options_.cooldown_max_ms = std::max(options_.cooldown_base_ms, options_.cooldown_max_ms);
}

inline FailureBudget::Verdict FailureBudget::BeginAttempt(int64_t now_ms) {
  if (cooldown_until_ms_ > now_ms) {
    return Verdict::kReject;
  }
  if (cooldown_until_ms_ != 0) {
    // The cooldown just ran out: let exactly one request probe the server.
    if (probe_in_flight_) {
      return Verdict::kReject;
    }
    probe_in_flight_ = true;
    return Verdict::kAllowProbe;
  }
  return Verdict::kAllow;
}

inline void FailureBudget::OnSuccess(int64_t /*now_ms*/) {
  consecutive_failures_ = 0;
  cooldown_until_ms_ = 0;
  probe_in_flight_ = false;
}

inline void FailureBudget::OnFailure(int64_t now_ms) {
  if (consecutive_failures_ < UINT32_MAX) {
    ++consecutive_failures_;
  }
  probe_in_flight_ = false;
  if (consecutive_failures_ < options_.failure_threshold) {
    return;
  }
  cooldown_until_ms_ = now_ms + CooldownFor(consecutive_failures_);
  ++open_count_;
}

inline int64_t FailureBudget::CooldownFor(uint32_t failures) const {
  // The first open already waits one interval; every further failure doubles it. The escalation
  // starts at the floor, not at the base: otherwise a floor of 3s would sit unchanged through the
  // first three trips (500, 1000, 2000 all clamp up to 3000) and only then start to grow.
  const uint32_t steps = failures > options_.failure_threshold ? failures - options_.failure_threshold : 0;
  // Cap the shift so a long outage cannot overflow the multiplication.
  const uint32_t shift = std::min<uint32_t>(steps, 20);
  const int64_t start = std::max(options_.cooldown_base_ms, options_.cooldown_floor_ms);
  return std::min(start << shift, options_.cooldown_max_ms);
}

// The "one slot, one thread" invariant, as pure bookkeeping. The pool feeds it a token derived
// from std::this_thread::get_id(); a test can feed it arbitrary numbers.
class SlotOwnership {
 public:
  explicit SlotOwnership(size_t slot_count) : owners_(slot_count) {}

  // True when |slot_index| now belongs to |thread_token|. Re-binding the same pair is a no-op,
  // a different token on a bound slot is refused.
  bool TryBind(size_t slot_index, uint64_t thread_token) {
    if (slot_index >= owners_.size()) {
      return false;
    }
    std::optional<uint64_t>& owner = owners_[slot_index];
    if (owner.has_value()) {
      return owner.value() == thread_token;
    }
    owner = thread_token;
    ++bound_count_;
    return true;
  }

  // True when |thread_token| was the owner and the slot is free again.
  bool TryUnbind(size_t slot_index, uint64_t thread_token) {
    if (slot_index >= owners_.size()) {
      return false;
    }
    std::optional<uint64_t>& owner = owners_[slot_index];
    if (!owner.has_value() || owner.value() != thread_token) {
      return false;
    }
    owner.reset();
    --bound_count_;
    return true;
  }

  bool bound(size_t slot_index) const { return slot_index < owners_.size() && owners_[slot_index].has_value(); }
  size_t bound_count() const { return bound_count_; }
  size_t slot_count() const { return owners_.size(); }

 private:
  std::vector<std::optional<uint64_t>> owners_;
  size_t bound_count_ = 0;
};

}  // namespace mahjong
