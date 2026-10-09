// The no-MySQL AccountStore (-DMAHJONG_WITH_MYSQL=OFF).
//
// It exists so the server can still be built on a machine without the MySQL client library,
// and so the degraded path of ACCOUNT_DESIGN.md §2.3 stays exercisable: every operation
// reports "unavailable", which is exactly what a real store does when MySQL is down.
//
// Unlike db_mysql.cc this one has no worker thread — it answers inline, on the caller's
// thread. That is safe because the only thing a callback does is enqueue a message into an
// actor, and it keeps the stub free of threads that CMake would otherwise have to join.

#include <memory>
#include <string>
#include <utility>

#include "db.h"

namespace mahjong {
namespace {

class UnavailableAccountStore : public AccountStore {
 public:
  explicit UnavailableAccountStore(DbConfig config) : config_(std::move(config)) {}

  bool available() const override { return false; }

  void LoginAsync(std::string /*username*/, std::string /*password*/, std::function<void(AuthOutcome)> done) override {
    done(AuthOutcome { });
  }

  void RegisterAsync(std::string /*username*/, std::string /*password*/, std::string /*nickname*/,
                     std::function<void(AuthOutcome)> done) override {
    done(AuthOutcome { });
  }

  void FetchStatsAsync(uint64_t /*account_id*/, std::function<void(StatsOutcome)> done) override {
    done(StatsOutcome { });
  }

  void RecordMatchAsync(MatchRecord /*record*/) override {}

  void Stop() override {}

 private:
  DbConfig config_;
};

}  // namespace

std::shared_ptr<AccountStore> AccountStore::Create(DbConfig config) {
  return std::make_shared<UnavailableAccountStore>(std::move(config));
}

}  // namespace mahjong
