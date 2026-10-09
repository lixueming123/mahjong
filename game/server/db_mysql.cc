// The MySQL-backed AccountStore (ACCOUNT_DESIGN.md §2).
//
// Shape: copied from WindowTimer (timer.cc) on purpose. The actor world must never block, so
// one worker thread owns the MySQL connection and runs the blocking calls; the caller's
// callback executes on that worker and does nothing but post a message back into its actor.
// Nothing in this file touches actor state, and nothing here is called from a handler while
// the handler holds state it cannot afford to lose.
//
// Password storage (§2.2, demo-grade by explicit decision): a per-account 16-byte random salt
// plus a single SHA-256, computed by MySQL's SHA2() so the server needs no crypto library.
// Swapping both SQL expressions for libcrypto's PBKDF2 is the documented upgrade path.

#include <mysql.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "db.h"

namespace mahjong {
namespace {

// MySQL error numbers we react to by name instead of by chance.
constexpr unsigned int kErrDupEntry = 1062;
constexpr unsigned int kErrServerGone = 2006;
constexpr unsigned int kErrServerLost = 2013;

// A connect must not outlast the server's own startup path (§2.3). 3s is far longer than a
// local socket needs and short enough that a dead host does not stall the process.
constexpr unsigned int kConnectTimeoutSeconds = 3;

// A backstop against unbounded growth if MySQL is far slower than the clients (§2.3). Each
// connection has at most one account operation in flight, so reaching this takes a lot of
// connections; past it the operation is refused and answered as unavailable rather than queued.
constexpr size_t kMaxPendingJobs = 256;

// 16 random bytes as 32 hex characters (§2.2).
std::string RandomSalt() {
  std::random_device device;
  std::uniform_int_distribution<unsigned int> byte(0, 255);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string salt;
  salt.reserve(32);
  for (int i = 0; i < 16; ++i) {
    unsigned int value = byte(device);
    salt.push_back(kHex[value >> 4]);
    salt.push_back(kHex[value & 0x0F]);
  }
  return salt;
}

// The database name is interpolated into DDL (no placeholders exist there), so it is
// restricted to a plain identifier.
bool IsValidDatabaseName(const std::string& name) {
  if (name.empty() || name.size() > 64) {
    return false;
  }
  for (char c : name) {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!alnum && c != '_') {
      return false;
    }
  }
  return true;
}

uint64_t ParseUint(const char* text) {
  if (text == nullptr) {
    return 0;
  }
  return std::strtoull(text, nullptr, 10);
}

int ParseInt(const char* text) {
  if (text == nullptr) {
    return 0;
  }
  return static_cast<int>(std::strtol(text, nullptr, 10));
}

class MysqlAccountStore : public AccountStore {
 public:
  explicit MysqlAccountStore(DbConfig config);
  ~MysqlAccountStore() override;

  bool available() const override { return available_.load(std::memory_order_relaxed); }

  void LoginAsync(std::string username, std::string password, std::function<void(AuthOutcome)> done) override;
  void RegisterAsync(std::string username, std::string password, std::string nickname,
                     std::function<void(AuthOutcome)> done) override;
  void FetchStatsAsync(uint64_t account_id, std::function<void(StatsOutcome)> done) override;
  void RecordMatchAsync(MatchRecord record) override;
  void Stop() override;

 private:
  using Job = std::function<void()>;

  // Returns false when the job was refused (shutting down, or the queue is full); the caller
  // must then answer its own callback instead of waiting for a result that will never come.
  bool Enqueue(Job job);
  void Run();
  // All of the following run on the worker thread only.
  void Initialize();
  bool EnsureConnected();
  bool EnsureSchema();
  void Disconnect();
  void LoginJob(const std::string& username, const std::string& password, const std::function<void(AuthOutcome)>& done);
  void RegisterJob(const std::string& username, const std::string& password, const std::string& nickname,
                   const std::function<void(AuthOutcome)>& done);
  void StatsJob(uint64_t account_id, const std::function<void(StatsOutcome)>& done);
  void RecordJob(const MatchRecord& record);

  // Escapes a value for the single-quoted literal it will sit in (§2.2). The inputs are
  // already charset-validated, so this is the second line of defence, not the first.
  std::string Escape(const std::string& value);

  DbConfig config_;
  std::mutex mutex_;
  std::condition_variable wakeup_;
  std::condition_variable init_done_cv_;
  std::deque<Job> queue_;
  bool stopping_ = false;
  bool init_done_ = false;
  std::thread worker_;
  MYSQL* mysql_ = nullptr;  // owned by the worker thread
  std::atomic<bool> available_ {false};
};

MysqlAccountStore::MysqlAccountStore(DbConfig config) : config_(std::move(config)) {
  worker_ = std::thread([this] { Run(); });

  // Probe synchronously: the caller wants to know at startup whether accounts work, and the
  // worker has to run the connect anyway (the connection belongs to its thread). The probe is
  // bounded by MYSQL_OPT_CONNECT_TIMEOUT, so this cannot hang the process. The job is pushed
  // directly — Enqueue would take the mutex this scope already holds.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back([this] { Initialize(); });
  }
  wakeup_.notify_one();
  std::unique_lock<std::mutex> lock(mutex_);
  init_done_cv_.wait(lock, [this] { return init_done_; });
}

MysqlAccountStore::~MysqlAccountStore() { Stop(); }

bool MysqlAccountStore::Enqueue(Job job) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // The queue is a plain deque drained front-to-back, so jobs run in submission order. That
    // FIFO property is load-bearing: a match record is submitted before the client can ask for
    // the stats that must include it (the game_end frame reaches the client only after
    // NotifyFinished has run).
    if (stopping_) {
      spdlog::warn("db: dropping an account operation submitted during shutdown");
      return false;
    }
    if (queue_.size() >= kMaxPendingJobs) {
      spdlog::error("db: {} account operations are already queued; refusing another one", queue_.size());
      return false;
    }
    queue_.push_back(std::move(job));
  }
  wakeup_.notify_one();
  return true;
}

void MysqlAccountStore::Run() {
  // The client library is initialized and torn down on this one thread, so no other thread
  // ever touches a half-initialized library (mysql_library_init is not thread safe).
  if (mysql_library_init(0, nullptr, nullptr) != 0) {
    spdlog::error("db: mysql_library_init failed; account features are disabled");
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;  // Enqueue must not accumulate jobs nobody will ever run
      init_done_ = true;
    }
    init_done_cv_.notify_all();
    return;
  }
  mysql_thread_init();
  while (true) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wakeup_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        break;  // stopping_ and drained
      }
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    // Outside the lock: a job posts into an actor mailbox and must not hold it (timer.cc
    // makes the same promise about its actions). A throwing job must never kill the worker.
    try {
      job();
    } catch (const std::exception& e) {
      spdlog::error("db: account operation failed: {}", e.what());
    }
  }
  Disconnect();  // the connection is this thread's, so it is closed on this thread
  mysql_thread_end();
  mysql_library_end();
}

void MysqlAccountStore::Initialize() {
  bool ok = EnsureConnected() && EnsureSchema();
  available_.store(ok, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    init_done_ = true;
  }
  init_done_cv_.notify_all();
  if (ok) {
    spdlog::info("db: account storage ready (database '{}' on {}:{})", config_.db, config_.host, config_.port);
  } else {
    // §2.3: the server keeps running; account frames answer INTERNAL from here on.
    spdlog::error("db: account storage unavailable ({}:{}); accounts are disabled, the game itself is unaffected",
                  config_.host, config_.port);
  }
}

bool MysqlAccountStore::EnsureConnected() {
  if (mysql_ != nullptr) {
    return true;
  }
  MYSQL* handle = mysql_init(nullptr);
  if (handle == nullptr) {
    spdlog::error("db: mysql_init failed");
    return false;
  }
  unsigned int connect_timeout = kConnectTimeoutSeconds;
  mysql_options(handle, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
  mysql_options(handle, MYSQL_OPT_READ_TIMEOUT, &connect_timeout);
  mysql_options(handle, MYSQL_OPT_WRITE_TIMEOUT, &connect_timeout);
  mysql_options(handle, MYSQL_SET_CHARSET_NAME, "utf8mb4");

  // MySQL 8's default auth plugin is caching_sha2_password, whose full-authentication path
  // refuses to send the password over an unencrypted TCP connection unless the client is
  // allowed to fetch the server's RSA public key. Without this, every connection fails with
  // "Authentication requires secure connection" and accounts silently degrade to unavailable —
  // a nasty thing to debug, so the option is opt-in here rather than left to the default.
  bool get_server_public_key = true;
  mysql_options(handle, MYSQL_OPT_GET_SERVER_PUBLIC_KEY, &get_server_public_key);

  // Connect without a database first: it may not exist yet, and CREATE DATABASE needs a
  // connection (see EnsureSchema).
  if (mysql_real_connect(handle, config_.host.c_str(), config_.user.c_str(), config_.password.c_str(), nullptr,
                         static_cast<unsigned int>(config_.port), nullptr, 0) == nullptr) {
    spdlog::error("db: cannot connect to MySQL at {}:{}: {}", config_.host, config_.port, mysql_error(handle));
    mysql_close(handle);
    available_.store(false, std::memory_order_relaxed);
    return false;
  }
  mysql_ = handle;
  return true;
}

bool MysqlAccountStore::EnsureSchema() {
  if (!IsValidDatabaseName(config_.db)) {
    spdlog::error("db: '{}' is not a usable database name", config_.db);
    return false;
  }
  const std::string create_database = "CREATE DATABASE IF NOT EXISTS `" + config_.db + "` CHARACTER SET utf8mb4";
  if (mysql_query(mysql_, create_database.c_str()) != 0) {
    spdlog::error("db: CREATE DATABASE failed: {}", mysql_error(mysql_));
    return false;
  }
  if (mysql_select_db(mysql_, config_.db.c_str()) != 0) {
    spdlog::error("db: cannot select database '{}': {}", config_.db, mysql_error(mysql_));
    return false;
  }

  // One statement per mysql_query call (the client library does not enable multi-statements).
  const char* const kSchema[] = {
      "CREATE TABLE IF NOT EXISTS accounts ("
      "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
      "  username VARCHAR(32) NOT NULL,"
      "  password_salt CHAR(32) NOT NULL,"
      "  password_hash CHAR(64) NOT NULL,"
      "  nickname VARCHAR(64) NOT NULL,"
      "  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "  last_login_at TIMESTAMP NULL,"
      "  PRIMARY KEY (id),"
      "  UNIQUE KEY uk_accounts_username (username)"
      ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
      "CREATE TABLE IF NOT EXISTS matches ("
      "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
      "  account_id BIGINT UNSIGNED NOT NULL,"
      "  room_id VARCHAR(16) NOT NULL,"
      "  reason VARCHAR(16) NOT NULL,"
      "  delta_score INT NOT NULL,"
      "  win TINYINT NOT NULL,"
      "  ended_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "  PRIMARY KEY (id),"
      "  KEY idx_matches_account (account_id, id)"
      ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
  };
  for (const char* statement : kSchema) {
    if (mysql_query(mysql_, statement) != 0) {
      spdlog::error("db: schema creation failed: {}", mysql_error(mysql_));
      return false;
    }
  }
  return true;
}

void MysqlAccountStore::Disconnect() {
  if (mysql_ != nullptr) {
    mysql_close(mysql_);
    mysql_ = nullptr;
  }
  available_.store(false, std::memory_order_relaxed);
}

std::string MysqlAccountStore::Escape(const std::string& value) {
  if (mysql_ == nullptr) {
    return {};
  }
  std::vector<char> buffer(value.size() * 2 + 1);
  unsigned long length =
      mysql_real_escape_string(mysql_, buffer.data(), value.c_str(), static_cast<unsigned long>(value.size()));
  return std::string(buffer.data(), length);
}

void MysqlAccountStore::LoginAsync(std::string username, std::string password, std::function<void(AuthOutcome)> done) {
  if (!Enqueue([this, username = std::move(username), password = std::move(password), done = std::move(done)] {
        LoginJob(username, password, done);
      })) {
    done(AuthOutcome {});  // kUnavailable: the caller answers INTERNAL
  }
}

void MysqlAccountStore::RegisterAsync(std::string username, std::string password, std::string nickname,
                                      std::function<void(AuthOutcome)> done) {
  if (!Enqueue([this, username = std::move(username), password = std::move(password), nickname = std::move(nickname),
                done = std::move(done)] { RegisterJob(username, password, nickname, done); })) {
    done(AuthOutcome {});
  }
}

void MysqlAccountStore::FetchStatsAsync(uint64_t account_id, std::function<void(StatsOutcome)> done) {
  if (!Enqueue([this, account_id, done = std::move(done)] { StatsJob(account_id, done); })) {
    done(StatsOutcome {});
  }
}

void MysqlAccountStore::RecordMatchAsync(MatchRecord record) {
  // Fire and forget: a refused record is a logged loss (§2.3), never a change in game behaviour.
  Enqueue([this, record = std::move(record)] { RecordJob(record); });
}

void MysqlAccountStore::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  wakeup_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

// ---- job bodies (worker thread) ---------------------------------------------------------

void MysqlAccountStore::LoginJob(const std::string& username, const std::string& password,
                                 const std::function<void(AuthOutcome)>& done) {
  AuthOutcome outcome;
  if (!EnsureConnected()) {
    done(outcome);
    return;
  }
  // §3.1: an unknown account and a wrong password must be indistinguishable, so one query
  // answers both and the row's absence is simply a failure.
  const std::string sql = "SELECT id, nickname, (password_hash = SHA2(CONCAT(password_salt, '" + Escape(password) +
                          "'), 256)) AS ok FROM accounts WHERE username = '" + Escape(username) + "'";
  if (mysql_query(mysql_, sql.c_str()) != 0) {
    spdlog::error("db: login query failed: {}", mysql_error(mysql_));
    Disconnect();
    done(outcome);
    return;
  }
  MYSQL_RES* result = mysql_store_result(mysql_);
  if (result == nullptr) {
    spdlog::error("db: login query returned no result: {}", mysql_error(mysql_));
    done(outcome);
    return;
  }
  MYSQL_ROW row = mysql_fetch_row(result);
  if (row != nullptr && row[0] != nullptr && row[1] != nullptr && row[2] != nullptr && std::strcmp(row[2], "1") == 0) {
    outcome.status = AuthStatus::kOk;
    outcome.account.id = ParseUint(row[0]);
    outcome.account.username = username;
    outcome.account.nickname = row[1];
  } else {
    outcome.status = AuthStatus::kAuthFailed;
  }
  mysql_free_result(result);

  if (outcome.status == AuthStatus::kOk) {
    // Best effort: a failed timestamp update must not fail a successful login.
    const std::string touch =
        "UPDATE accounts SET last_login_at = NOW() WHERE id = " + std::to_string(outcome.account.id);
    if (mysql_query(mysql_, touch.c_str()) != 0) {
      spdlog::warn("db: could not update last_login_at: {}", mysql_error(mysql_));
    }
  }
  done(outcome);
}

void MysqlAccountStore::RegisterJob(const std::string& username, const std::string& password,
                                    const std::string& nickname, const std::function<void(AuthOutcome)>& done) {
  AuthOutcome outcome;
  if (!EnsureConnected()) {
    done(outcome);
    return;
  }
  const std::string salt = RandomSalt();
  const std::string stored_nickname = nickname.empty() ? username : nickname;
  const std::string sql =
      "INSERT INTO accounts (username, password_salt, password_hash, nickname, last_login_at) VALUES ('" +
      Escape(username) + "', '" + salt + "', SHA2(CONCAT('" + salt + "', '" + Escape(password) + "'), 256), '" +
      Escape(stored_nickname) + "', NOW())";
  if (mysql_query(mysql_, sql.c_str()) != 0) {
    const unsigned int error = mysql_errno(mysql_);
    if (error == kErrDupEntry) {
      outcome.status = AuthStatus::kUsernameTaken;  // the UNIQUE key is the arbiter of races
    } else {
      spdlog::error("db: register failed: {}", mysql_error(mysql_));
      if (error == kErrServerGone || error == kErrServerLost) {
        Disconnect();
      }
    }
    done(outcome);
    return;
  }
  outcome.status = AuthStatus::kOk;
  outcome.account.id = static_cast<uint64_t>(mysql_insert_id(mysql_));
  outcome.account.username = username;
  outcome.account.nickname = stored_nickname;
  done(outcome);
}

void MysqlAccountStore::StatsJob(uint64_t account_id, const std::function<void(StatsOutcome)>& done) {
  StatsOutcome outcome;
  if (!EnsureConnected()) {
    done(outcome);
    return;
  }
  const std::string id = std::to_string(account_id);
  // COALESCE: an account with no finished game has SUM(x) = NULL, not 0.
  const std::string totals =
      "SELECT COUNT(*), COALESCE(SUM(delta_score), 0), COALESCE(SUM(win), 0),"
      " COALESCE(SUM(reason = 'draw_exhausted'), 0) FROM matches WHERE account_id = " + id;
  if (mysql_query(mysql_, totals.c_str()) != 0) {
    spdlog::error("db: stats query failed: {}", mysql_error(mysql_));
    Disconnect();
    done(outcome);
    return;
  }
  MYSQL_RES* result = mysql_store_result(mysql_);
  if (result == nullptr) {
    spdlog::error("db: stats query returned no result: {}", mysql_error(mysql_));
    done(outcome);
    return;
  }
  MYSQL_ROW row = mysql_fetch_row(result);
  if (row != nullptr) {
    outcome.summary.games = ParseInt(row[0]);
    outcome.summary.net_score = ParseInt(row[1]);
    outcome.summary.wins = ParseInt(row[2]);
    outcome.summary.draws = ParseInt(row[3]);
  }
  mysql_free_result(result);

  // §3.4: the recent list is newest first and capped at kMaxRecentMatches.
  const std::string recent = "SELECT room_id, reason, delta_score, win, UNIX_TIMESTAMP(ended_at) FROM matches"
                             " WHERE account_id = " +
                             id + " ORDER BY id DESC LIMIT " + std::to_string(protocol::kMaxRecentMatches);
  if (mysql_query(mysql_, recent.c_str()) != 0) {
    spdlog::error("db: recent matches query failed: {}", mysql_error(mysql_));
    Disconnect();
    done(outcome);
    return;
  }
  result = mysql_store_result(mysql_);
  if (result == nullptr) {
    spdlog::error("db: recent matches query returned no result: {}", mysql_error(mysql_));
    done(outcome);
    return;
  }
  while (MYSQL_ROW entry = mysql_fetch_row(result)) {
    if (entry[0] == nullptr || entry[1] == nullptr) {
      continue;
    }
    protocol::StatsEntry item;
    item.room_id = entry[0];
    item.reason = entry[1];
    item.delta_score = ParseInt(entry[2]);
    item.win = entry[3] != nullptr && std::strcmp(entry[3], "1") == 0;
    item.ended_at = static_cast<int64_t>(ParseUint(entry[4]));
    outcome.summary.recent.push_back(std::move(item));
  }
  mysql_free_result(result);
  outcome.ok = true;
  done(outcome);
}

void MysqlAccountStore::RecordJob(const MatchRecord& record) {
  if (!EnsureConnected()) {
    spdlog::warn("db: dropping the match record of room {} (account storage unavailable)", record.room_id);
    return;
  }
  const std::string sql = "INSERT INTO matches (account_id, room_id, reason, delta_score, win) VALUES (" +
                          std::to_string(record.account_id) + ", '" + Escape(record.room_id) + "', '" +
                          Escape(record.reason) + "', " + std::to_string(record.delta_score) + ", " +
                          (record.win ? "1" : "0") + ")";
  if (mysql_query(mysql_, sql.c_str()) != 0) {
    spdlog::error("db: recording the match of room {} failed: {}", record.room_id, mysql_error(mysql_));
    const unsigned int error = mysql_errno(mysql_);
    if (error == kErrServerGone || error == kErrServerLost) {
      Disconnect();
    }
  }
}

}  // namespace

std::shared_ptr<AccountStore> AccountStore::Create(DbConfig config) {
  return std::make_shared<MysqlAccountStore>(std::move(config));
}

}  // namespace mahjong
