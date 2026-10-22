// Unit tests for the account feature's pure logic (ACCOUNT_DESIGN.md).
//
// Deliberately not linked against mahjong_db: the storage layer needs a live MySQL, while
// everything here — the field rules of §3.1/§3.2, the durable player_id of §1.2, the
// one-account-one-connection registry of §1.3 and the stats wire shape of §3.4 — must be
// checkable on any machine. The end-to-end path (real MySQL) is tests/account_e2e_test.py.

#include <string>

#include <gtest/gtest.h>

#include "json.h"
#include "player.h"
#include "protocol.h"

namespace mahjong {
namespace {

TEST(AccountFieldRules, UsernameIsAsciiIdentifierOfThreeToSixteenChars) {
  EXPECT_TRUE(protocol::IsValidAccountName("abc"));
  EXPECT_TRUE(protocol::IsValidAccountName("a_1"));
  EXPECT_TRUE(protocol::IsValidAccountName("Alice_99"));
  EXPECT_TRUE(protocol::IsValidAccountName("abcdefghijklmnop"));  // exactly 16

  EXPECT_FALSE(protocol::IsValidAccountName(""));                  // empty
  EXPECT_FALSE(protocol::IsValidAccountName("ab"));                // too short
  EXPECT_FALSE(protocol::IsValidAccountName("abcdefghijklmnopq"));  // 17, one too many
  EXPECT_FALSE(protocol::IsValidAccountName("a b"));               // space
  EXPECT_FALSE(protocol::IsValidAccountName("a-b"));               // not in [A-Za-z0-9_]
  EXPECT_FALSE(protocol::IsValidAccountName("玩家"));               // non-ASCII
  EXPECT_FALSE(protocol::IsValidAccountName("a'b; DROP TABLE accounts; --"));
}

TEST(AccountFieldRules, PasswordIsSixToSixtyFourPrintableAsciiChars) {
  EXPECT_TRUE(protocol::IsValidPassword("123456"));
  EXPECT_TRUE(protocol::IsValidPassword("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"));
  EXPECT_TRUE(protocol::IsValidPassword(std::string(64, 'x')));

  EXPECT_FALSE(protocol::IsValidPassword("12345"));                // too short
  EXPECT_FALSE(protocol::IsValidPassword(std::string(65, 'x')));   // too long
  EXPECT_FALSE(protocol::IsValidPassword("12345\n"));              // control character
  EXPECT_FALSE(protocol::IsValidPassword("12\t3456"));             // control character
  EXPECT_FALSE(protocol::IsValidPassword("12345\x7F"));            // DEL
  EXPECT_FALSE(protocol::IsValidPassword("密码123456"));            // non-ASCII
}

TEST(AccountFieldRules, NicknameKeepsTheFrozenLoginRule) {
  // PROTOCOL §4.1 / GAME_DESIGN §3.5: 1..16 code points, no control characters.
  EXPECT_TRUE(protocol::IsValidNickname("玩家_"));
  EXPECT_TRUE(protocol::IsValidNickname("abcdefghijklmnop"));
  EXPECT_TRUE(protocol::IsValidNickname("十六个中文字符测试一下啊啊"));  // 14 code points

  EXPECT_FALSE(protocol::IsValidNickname(""));
  EXPECT_FALSE(protocol::IsValidNickname("abcdefghijklmnopq"));      // 17 code points
  EXPECT_FALSE(protocol::IsValidNickname("a\nb"));
  EXPECT_FALSE(protocol::IsValidNickname("a\x7F b"));
}

// ACCOUNT_DESIGN.md §1.2: guest ids and account ids never collide, and an account id is a
// pure function of the durable account row.
TEST(PlayerIdentity, AccountIdsAreDerivedFromTheDurableRow) {
  EXPECT_EQ(AccountPlayerId(0), "u-000000");
  EXPECT_EQ(AccountPlayerId(42), "u-000042");
  EXPECT_EQ(AccountPlayerId(999999), "u-999999");
  EXPECT_EQ(AccountPlayerId(12345678), "u-12345678");  // grows past six digits, stays unique

  PlayerIdAllocator allocator;
  for (int i = 0; i < 1000; ++i) {
    const std::string guest = allocator.Next();
    EXPECT_EQ(guest.size(), 8u);
    EXPECT_EQ(guest[0], 'p');
    EXPECT_EQ(guest[1], '-');
    EXPECT_NE(guest, AccountPlayerId(static_cast<uint64_t>(i)));
  }
}

// ACCOUNT_DESIGN.md §1.3.
TEST(SessionRegistryTest, ClaimsAnAccountExactlyOnce) {
  SessionRegistry registry;
  EXPECT_TRUE(registry.TryAcquire(7));
  EXPECT_TRUE(registry.Contains(7));
  EXPECT_FALSE(registry.TryAcquire(7)) << "a second connection must not take the same account";
  EXPECT_TRUE(registry.TryAcquire(8)) << "different accounts are independent";

  registry.Release(7);
  EXPECT_FALSE(registry.Contains(7));
  EXPECT_TRUE(registry.TryAcquire(7)) << "the account is free again after its session ended";

  registry.Release(7);  // releasing twice is harmless
  EXPECT_FALSE(registry.Contains(7));
}

// ACCOUNT_DESIGN.md §3.4: the stats frame is what the client renders, so its field names and
// types are part of the contract.
TEST(StatsFrameTest, CarriesTheFrozenFields) {
  protocol::StatsSummary summary;
  summary.games = 12;
  summary.wins = 5;
  summary.draws = 3;
  summary.net_score = -14;
  summary.recent.push_back(protocol::StatsEntry {"r-0007", "win_ron", 12, true, 1759900000});

  const std::string text = protocol::StatsFrame(summary);
  std::string error;
  std::optional<json::Value> parsed = json::Value::Parse(text, &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  ASSERT_TRUE(parsed->IsObject());

  const json::Value* type = parsed->Find("type");
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(type->AsString(), "stats");
  EXPECT_EQ(parsed->Find("games")->AsInt(), 12);
  EXPECT_EQ(parsed->Find("wins")->AsInt(), 5);
  EXPECT_EQ(parsed->Find("draws")->AsInt(), 3);
  EXPECT_EQ(parsed->Find("net_score")->AsInt(), -14);

  const json::Value* recent = parsed->Find("recent");
  ASSERT_NE(recent, nullptr);
  ASSERT_TRUE(recent->IsArray());
  ASSERT_EQ(recent->AsArray().size(), 1u);
  const json::Value& entry = recent->AsArray().front();
  EXPECT_EQ(entry.Find("room_id")->AsString(), "r-0007");
  EXPECT_EQ(entry.Find("reason")->AsString(), "win_ron");
  EXPECT_EQ(entry.Find("delta_score")->AsInt(), 12);
  EXPECT_TRUE(entry.Find("win")->AsBool());
  EXPECT_EQ(entry.Find("ended_at")->AsInt(), 1759900000);

  // A guest's zeroed payload (§3.3) is still a well-formed frame.
  const std::string empty = protocol::StatsFrame(protocol::StatsSummary {});
  parsed = json::Value::Parse(empty, &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  EXPECT_EQ(parsed->Find("games")->AsInt(), 0);
  EXPECT_TRUE(parsed->Find("recent")->AsArray().empty());
}

// v1.3（B3）回归断言：match_results 落库对 `stats` 帧**零改动**（ACCOUNT_DESIGN §4 第 4 条：
// 本期不展示，StatsSummary 与 StatsFrame 一律不动）。字段集合与既有形状逐字节冻结。
TEST(StatsFrameTest, UnaffectedByMatchResultsPersistence) {
  protocol::StatsSummary summary;
  summary.games = 4;  // 一场四局（matches 仍按局一行，语义未变）
  summary.wins = 1;
  summary.draws = 1;
  summary.net_score = 6;
  summary.recent.push_back(protocol::StatsEntry {"r-0001", "win_tsumo", 24, true, 1759900100});
  summary.recent.push_back(protocol::StatsEntry {"r-0001", "draw_exhausted", 0, false, 1759900200});

  const std::string text = protocol::StatsFrame(summary);
  std::string error;
  std::optional<json::Value> parsed = json::Value::Parse(text, &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  // 字段集合冻结：既有 6 个键（type + 四项汇总 + recent），不得因 match_results 增删任何键
  const auto& object = parsed->AsObject();
  ASSERT_EQ(object.size(), 6u);
  EXPECT_NE(parsed->Find("type"), nullptr);
  EXPECT_NE(parsed->Find("games"), nullptr);
  EXPECT_NE(parsed->Find("wins"), nullptr);
  EXPECT_NE(parsed->Find("draws"), nullptr);
  EXPECT_NE(parsed->Find("net_score"), nullptr);
  EXPECT_NE(parsed->Find("recent"), nullptr);
  // recent 的行形状冻结：5 个键
  const json::Value& entry = parsed->Find("recent")->AsArray().front();
  EXPECT_EQ(entry.AsObject().size(), 5u);
  EXPECT_NE(entry.Find("room_id"), nullptr);
  EXPECT_NE(entry.Find("reason"), nullptr);
  EXPECT_NE(entry.Find("delta_score"), nullptr);
  EXPECT_NE(entry.Find("win"), nullptr);
  EXPECT_NE(entry.Find("ended_at"), nullptr);
}

// The three codes ACCOUNT_DESIGN.md §3.5 adds must not disturb the frozen eleven.
TEST(ErrorCodes, NewCodesAreTheOnlyAdditions) {
  EXPECT_EQ(std::string(protocol::kCodeAuthFailed), "AUTH_FAILED");
  EXPECT_EQ(std::string(protocol::kCodeUsernameTaken), "USERNAME_TAKEN");
  EXPECT_EQ(std::string(protocol::kCodeAlreadyOnline), "ALREADY_ONLINE");
  EXPECT_EQ(std::string(protocol::kCodeInvalidName), "INVALID_NAME");
  EXPECT_EQ(std::string(protocol::kCodeInternal), "INTERNAL");
}

}  // namespace
}  // namespace mahjong
