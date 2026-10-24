// The MySQL-backed DbActor (ACCOUNT_DESIGN.md §2).
//
// Shape: the actor owns one MySQL connection and every query runs inside a handler, so it
// blocks db_pool's only thread for that query's duration and nothing else. That is the whole
// point of main() pinning this actor to a dedicated single-thread pool: the blocking calls
// never reach a thread the game pools schedule on, and because the pool has exactly one
// thread the mailbox is drained in order — the FIFO property §4.1 depends on (a match record
// is always ahead of the stats request that must include it) comes for free.
//
// Password storage (§2.2, demo-grade by explicit decision): a per-account 16-byte random salt
// plus a single SHA-256, computed by MySQL's SHA2() so the server needs no crypto library.
// Swapping both SQL expressions for libcrypto's PBKDF2 is the documented upgrade path.

#include <mysql.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "actor_scope.h"
#include "player_actor.h"
#include "db.h"
#include "seat_endpoint.h"

namespace mahjong {
namespace {

// mysql_library_init/end are process-wide and not thread safe, so they are done exactly once by
// whoever gets here first (main() at startup, or the first DbActor::Initialize as a backstop).
std::once_flag g_library_once;
bool g_library_ready = false;

}  // namespace

bool DbLibraryInit() {
  std::call_once(g_library_once, [] { g_library_ready = (mysql_library_init(0, nullptr, nullptr) == 0); });
  return g_library_ready;
}

void DbLibraryShutdown() {
  if (g_library_ready) {
    mysql_library_end();
    g_library_ready = false;
  }
}

namespace {

// MySQL error numbers we react to by name instead of by chance.
constexpr unsigned int kErrDupEntry = 1062;
constexpr unsigned int kErrServerGone = 2006;
constexpr unsigned int kErrServerLost = 2013;

// A connect must not outlast the server's own startup path (§2.3). 3s is far longer than a
// local socket needs and short enough that a dead host does not stall the process.
constexpr unsigned int kConnectTimeoutSeconds = 3;

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

// Posts an account outcome into the requesting PlayerActor's mailbox. The result must
// arrive as a message, not as a return value: PlayerActor's handlers never suspend
// (player_actor.h), so there is nothing there to await.
void ReplyAuth(const ex_actor::ActorRef<PlayerActor>& reply, AuthOutcome outcome) {
  if (reply.IsEmpty()) {
    return;  // the connection went away while the query ran; nobody is left to answer
  }
  ActorSendScope().spawn(
      reply.SendLocal<&PlayerActor::Dispatch>(PlayerEnvelope::OnLoginResult(std::move(outcome))));
}

void ReplyRegister(const ex_actor::ActorRef<PlayerActor>& reply, AuthOutcome outcome) {
  if (reply.IsEmpty()) {
    return;
  }
  ActorSendScope().spawn(
      reply.SendLocal<&PlayerActor::Dispatch>(PlayerEnvelope::OnRegisterResult(std::move(outcome))));
}

void ReplyStats(const ex_actor::ActorRef<PlayerActor>& reply, StatsOutcome outcome) {
  if (reply.IsEmpty()) {
    return;
  }
  ActorSendScope().spawn(
      reply.SendLocal<&PlayerActor::Dispatch>(PlayerEnvelope::OnStatsResult(std::move(outcome))));
}

}  // namespace

DbActor::DbActor(DbConfig config) : config_(std::move(config)) {
  // Deliberately empty: the connection belongs to db_pool's thread, and this constructor runs
  // on the thread that called Spawn. Initialize() does the opening, on the right thread.
}

stdexec::task<bool> DbActor::Initialize() {
  // The library is initialised once for the whole process (DbLibraryInit); this call only
  // registers the current thread with it, which is what makes the client usable here at all.
  // Every db_pool thread runs this for itself.
  if (!DbLibraryInit()) {
    spdlog::error("db: mysql_library_init failed; account features are disabled");
    co_return false;
  }
  mysql_thread_init();

  bool ok = EnsureConnected() && EnsureSchema();
  available_ = ok;
  if (ok) {
    spdlog::info("db: account storage ready (database '{}' on {}:{})", config_.db, config_.host, config_.port);
  } else {
    // §2.3: the server keeps running; account frames answer INTERNAL from here on.
    spdlog::error("db: account storage unavailable ({}:{}); accounts are disabled, the game itself is unaffected",
                  config_.host, config_.port);
  }
  co_return ok;
}

bool DbActor::EnsureConnected() {
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
    available_ = false;
    return false;
  }
  mysql_ = handle;
  return true;
}

bool DbActor::EnsureSchema() {
  if (!IsValidDatabaseName(config_.db)) {
    spdlog::error("db: '{}' is not a usable database name", config_.db);
    return false;
  }
  MYSQL* mysql = static_cast<MYSQL*>(mysql_);
  const std::string create_database = "CREATE DATABASE IF NOT EXISTS `" + config_.db + "` CHARACTER SET utf8mb4";
  if (mysql_query(mysql, create_database.c_str()) != 0) {
    spdlog::error("db: CREATE DATABASE failed: {}", mysql_error(mysql));
    return false;
  }
  if (mysql_select_db(mysql, config_.db.c_str()) != 0) {
    spdlog::error("db: cannot select database '{}': {}", config_.db, mysql_error(mysql));
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
      // v1.3 新增：场次汇总（一场每账号一行），与 ACCOUNT_DESIGN.md §2.1 逐字段一致。
      "CREATE TABLE IF NOT EXISTS match_results ("
      "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
      "  account_id BIGINT UNSIGNED NOT NULL,"
      "  room_id VARCHAR(16) NOT NULL,"
      "  hands_played INT NOT NULL,"
      "  dealer_hands INT NOT NULL,"
      "  final_score INT NOT NULL,"
      "  final_rank TINYINT NOT NULL,"
      "  ended_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "  PRIMARY KEY (id),"
      "  KEY idx_match_results_account (account_id, id),"
      "  UNIQUE KEY uk_match_results_room_account (room_id, account_id)"
      ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
  };
  for (const char* statement : kSchema) {
    if (mysql_query(mysql, statement) != 0) {
      spdlog::error("db: schema creation failed: {}", mysql_error(mysql));
      return false;
    }
  }
  return true;
}

void DbActor::Disconnect() {
  if (mysql_ != nullptr) {
    mysql_close(static_cast<MYSQL*>(mysql_));
    mysql_ = nullptr;
  }
  available_ = false;
}

std::string DbActor::Escape(const std::string& value) {
  MYSQL* mysql = static_cast<MYSQL*>(mysql_);
  if (mysql == nullptr) {
    return {};
  }
  std::vector<char> buffer(value.size() * 2 + 1);
  unsigned long length =
      mysql_real_escape_string(mysql, buffer.data(), value.c_str(), static_cast<unsigned long>(value.size()));
  return std::string(buffer.data(), length);
}

// ---- handlers (db_pool thread) -----------------------------------------------------------

stdexec::task<void> DbActor::Login(std::string username, std::string password, ex_actor::ActorRef<PlayerActor> reply) {
  ReplyAuth(reply, LoginJob(username, password));
  co_return;
}

stdexec::task<void> DbActor::Register(std::string username, std::string password, std::string nickname,
                                   ex_actor::ActorRef<PlayerActor> reply) {
  ReplyRegister(reply, RegisterJob(username, password, nickname));
  co_return;
}

stdexec::task<void> DbActor::FetchStats(uint64_t account_id, ex_actor::ActorRef<PlayerActor> reply) {
  ReplyStats(reply, StatsJob(account_id));
  co_return;
}

stdexec::task<void> DbActor::RecordMatch(MatchRecord record) {
  // Fire and forget: a failed write is logged and never propagates to the game (§2.3, §4.6).
  RecordJob(record);
  co_return;
}

stdexec::task<void> DbActor::RecordMatchResult(MatchResultRecord record) {
  // Fire and forget (§4 item 7)：场终一次性写入；失败只记日志，绝不影响 match_end 的广播。
  RecordMatchResultJob(record);
  co_return;
}

stdexec::task<void> DbActor::Stop() {
  Disconnect();
  // Only this thread unregisters itself; the library itself is torn down once by main() after
  // every db thread has stopped (DbLibraryShutdown).
  mysql_thread_end();
  co_return;
}

// ---- query bodies (db_pool thread) -------------------------------------------------------

AuthOutcome DbActor::LoginJob(const std::string& username, const std::string& password) {
  AuthOutcome outcome;
  if (!EnsureConnected()) {
    return outcome;
  }
  MYSQL* mysql = static_cast<MYSQL*>(mysql_);
  // §3.1: an unknown account and a wrong password must be indistinguishable, so one query
  // answers both and the row's absence is simply a failure.
  const std::string sql = "SELECT id, nickname, (password_hash = SHA2(CONCAT(password_salt, '" + Escape(password) +
                          "'), 256)) AS ok FROM accounts WHERE username = '" + Escape(username) + "'";
  if (mysql_query(mysql, sql.c_str()) != 0) {
    spdlog::error("db: login query failed: {}", mysql_error(mysql));
    Disconnect();
    return outcome;
  }
  MYSQL_RES* result = mysql_store_result(mysql);
  if (result == nullptr) {
    spdlog::error("db: login query returned no result: {}", mysql_error(mysql));
    return outcome;
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
    if (mysql_query(mysql, touch.c_str()) != 0) {
      spdlog::warn("db: could not update last_login_at: {}", mysql_error(mysql));
    }
  }
  return outcome;
}

AuthOutcome DbActor::RegisterJob(const std::string& username, const std::string& password,
                                 const std::string& nickname) {
  AuthOutcome outcome;
  if (!EnsureConnected()) {
    return outcome;
  }
  MYSQL* mysql = static_cast<MYSQL*>(mysql_);
  const std::string salt = RandomSalt();
  const std::string stored_nickname = nickname.empty() ? username : nickname;
  const std::string sql =
      "INSERT INTO accounts (username, password_salt, password_hash, nickname, last_login_at) VALUES ('" +
      Escape(username) + "', '" + salt + "', SHA2(CONCAT('" + salt + "', '" + Escape(password) + "'), 256), '" +
      Escape(stored_nickname) + "', NOW())";
  if (mysql_query(mysql, sql.c_str()) != 0) {
    const unsigned int error = mysql_errno(mysql);
    if (error == kErrDupEntry) {
      outcome.status = AuthStatus::kUsernameTaken;  // the UNIQUE key is the arbiter of races
    } else {
      spdlog::error("db: register failed: {}", mysql_error(mysql));
      if (error == kErrServerGone || error == kErrServerLost) {
        Disconnect();
      }
    }
    return outcome;
  }
  outcome.status = AuthStatus::kOk;
  outcome.account.id = static_cast<uint64_t>(mysql_insert_id(mysql));
  outcome.account.username = username;
  outcome.account.nickname = stored_nickname;
  return outcome;
}

StatsOutcome DbActor::StatsJob(uint64_t account_id) {
  StatsOutcome outcome;
  if (!EnsureConnected()) {
    return outcome;
  }
  MYSQL* mysql = static_cast<MYSQL*>(mysql_);
  const std::string id = std::to_string(account_id);
  // COALESCE: an account with no finished game has SUM(x) = NULL, not 0.
  const std::string totals =
      "SELECT COUNT(*), COALESCE(SUM(delta_score), 0), COALESCE(SUM(win), 0),"
      " COALESCE(SUM(reason = 'draw_exhausted'), 0) FROM matches WHERE account_id = " +
      id;
  if (mysql_query(mysql, totals.c_str()) != 0) {
    spdlog::error("db: stats query failed: {}", mysql_error(mysql));
    Disconnect();
    return outcome;
  }
  MYSQL_RES* result = mysql_store_result(mysql);
  if (result == nullptr) {
    spdlog::error("db: stats query returned no result: {}", mysql_error(mysql));
    return outcome;
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
  const std::string recent =
      "SELECT room_id, reason, delta_score, win, UNIX_TIMESTAMP(ended_at) FROM matches"
      " WHERE account_id = " +
      id + " ORDER BY id DESC LIMIT " + std::to_string(protocol::kMaxRecentMatches);
  if (mysql_query(mysql, recent.c_str()) != 0) {
    spdlog::error("db: recent matches query failed: {}", mysql_error(mysql));
    Disconnect();
    return outcome;
  }
  result = mysql_store_result(mysql);
  if (result == nullptr) {
    spdlog::error("db: recent matches query returned no result: {}", mysql_error(mysql));
    return outcome;
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
  return outcome;
}

void DbActor::RecordJob(const MatchRecord& record) {
  if (!EnsureConnected()) {
    spdlog::warn("db: dropping the match record of room {} (account storage unavailable)", record.room_id);
    return;
  }
  MYSQL* mysql = static_cast<MYSQL*>(mysql_);
  const std::string sql = "INSERT INTO matches (account_id, room_id, reason, delta_score, win) VALUES (" +
                          std::to_string(record.account_id) + ", '" + Escape(record.room_id) + "', '" +
                          Escape(record.reason) + "', " + std::to_string(record.delta_score) + ", " +
                          (record.win ? "1" : "0") + ")";
  if (mysql_query(mysql, sql.c_str()) != 0) {
    spdlog::error("db: recording the match of room {} failed: {}", record.room_id, mysql_error(mysql));
    const unsigned int error = mysql_errno(mysql);
    if (error == kErrServerGone || error == kErrServerLost) {
      Disconnect();
    }
  }
}

void DbActor::RecordMatchResultJob(const MatchResultRecord& record) {
  if (!EnsureConnected()) {
    spdlog::warn("db: dropping the match result of room {} (account storage unavailable)", record.room_id);
    return;
  }
  MYSQL* mysql = static_cast<MYSQL*>(mysql_);
  const std::string sql =
      "INSERT INTO match_results (account_id, room_id, hands_played, dealer_hands, final_score, final_rank) VALUES (" +
      std::to_string(record.account_id) + ", '" + Escape(record.room_id) + "', " +
      std::to_string(record.hands_played) + ", " + std::to_string(record.dealer_hands) + ", " +
      std::to_string(record.final_score) + ", " + std::to_string(record.final_rank) + ")";
  if (mysql_query(mysql, sql.c_str()) != 0) {
    const unsigned int error = mysql_errno(mysql);
    // v1.3 §4 第 1 条：重复写入（同 room_id + account_id 命中 UNIQUE 键）记 WARN 并忽略，绝不重试。
    if (error == 1062 /* ER_DUP_ENTRY */) {
      spdlog::warn("db: match result of room {} for account {} already recorded; ignoring the duplicate",
                   record.room_id, record.account_id);
      return;
    }
    spdlog::error("db: recording the match result of room {} failed: {}", record.room_id, mysql_error(mysql));
    if (error == kErrServerGone || error == kErrServerLost) {
      Disconnect();
    }
  }
}

}  // namespace mahjong
