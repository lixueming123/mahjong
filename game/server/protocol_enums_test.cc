// Behaviour test for the frozen PROTOCOL v2.1 wire enums. The numeric values themselves
// are guarded against game/protocol/enums.json (and the Python/JS/doc tables) by
// tests/protocol_enums_consistency_test.py; this test covers parsing, names and boundaries.

#include "protocol_enums.h"

#include <cstdint>
#include <optional>
#include <string_view>

#include <gtest/gtest.h>

namespace mahjong::protocol {
namespace {

template <typename E>
void CheckDomain(std::optional<E> (*parse)(int64_t)) {
  const int max_value = WireTable<E>::kMax;
  EXPECT_FALSE(ParseWire<E>(0).has_value());
  EXPECT_FALSE(ParseWire<E>(-1).has_value());
  EXPECT_FALSE(ParseWire<E>(max_value + 1).has_value());
  EXPECT_EQ(NameOf(E::kInvalid), "INVALID");
  EXPECT_FALSE(FromName<E>("INVALID").has_value());
  for (int value = 1; value <= max_value; ++value) {
    const std::optional<E> parsed = parse(value);
    ASSERT_TRUE(parsed.has_value()) << "value " << value;
    EXPECT_EQ(WireOf(*parsed), value);
    const std::string_view name = NameOf(*parsed);
    EXPECT_NE(name, "INVALID") << "value " << value;
    const std::optional<E> reversed = FromName<E>(name);
    ASSERT_TRUE(reversed.has_value()) << "name " << name;
    EXPECT_EQ(*reversed, *parsed);
  }
}

TEST(ProtocolEnumsTest, EveryDomainRoundTrips) {
  CheckDomain<FrameType>(ParseFrameType);
  CheckDomain<Action>(ParseAction);
  CheckDomain<Window>(ParseWindow);
  CheckDomain<WireMode>(ParseWireMode);
  CheckDomain<WireLength>(ParseWireLength);
  CheckDomain<WireMeldType>(ParseWireMeldType);
  CheckDomain<RoundEndReason>(ParseReason);
  CheckDomain<ErrorCode>(ParseErrorCode);
  CheckDomain<YakuId>(ParseYakuId);
  CheckDomain<WireLimit>(ParseWireLimit);
}

TEST(ProtocolEnumsTest, YakuDomainCoversWinMethodsAndYaku) {
  EXPECT_EQ(WireOf(YakuId::kMenzen), 1);
  EXPECT_EQ(WireOf(YakuId::kDaisuushii), 45);
  EXPECT_EQ(WireOf(YakuId::kWinRon), 46);
  EXPECT_EQ(WireOf(YakuId::kWinTsumo), 47);
  EXPECT_EQ(NameOf(YakuId::kWinTsumo), "win_tsumo");
  EXPECT_EQ(NameOf(YakuId::kTsumo), "tsumo");
  EXPECT_EQ(FromName<YakuId>("win_ron"), std::optional<YakuId>(YakuId::kWinRon));
  EXPECT_EQ(FromName<YakuId>("tsumo"), std::optional<YakuId>(YakuId::kTsumo));
}

TEST(ProtocolEnumsTest, LimitNoneIsARealValue) {
  EXPECT_EQ(WireOf(WireLimit::kNone), 1);
  EXPECT_EQ(WireOf(WireLimit::kYakuman), 6);
  EXPECT_EQ(NameOf(WireLimit::kNone), "none");
  EXPECT_FALSE(ParseWireLimit(0).has_value());
  EXPECT_EQ(ParseWireLimit(1), std::optional<WireLimit>(WireLimit::kNone));
}

TEST(ProtocolEnumsTest, ClientFrameTypesCoverOneToEleven) {
  // 1..11 stay contiguous. autoplay is appended at 28 so the S→C block is not renumbered.
  EXPECT_EQ(kClientFrameTypes.size(), static_cast<size_t>(kClientFrameTypeMax));
  ASSERT_GE(kClientFrameTypes.size(), static_cast<size_t>(11));
  for (size_t index = 0; index < 11; ++index) {
    EXPECT_EQ(WireOf(kClientFrameTypes[index]), static_cast<int>(index) + 1);
  }
  EXPECT_EQ(kClientFrameTypes.back(), FrameType::kAutoplay);
  EXPECT_EQ(WireOf(FrameType::kAutoplay), 28);
  for (const FrameType type : kClientFrameTypes) {
    EXPECT_TRUE(IsClientFrameType(type));
    EXPECT_FALSE(IsServerFrameType(type));
  }
  EXPECT_TRUE(IsServerFrameType(FrameType::kLoginOk));
  EXPECT_TRUE(IsServerFrameType(FrameType::kRoomUpdate));
  EXPECT_FALSE(IsClientFrameType(FrameType::kGameStart));
  EXPECT_FALSE(IsServerFrameType(FrameType::kInvalid));
}

TEST(ProtocolEnumsTest, SameWordHasIndependentValuesPerDomain) {
  // `pong` is a server frame, an action and a meld type — three different numbers.
  EXPECT_EQ(WireOf(FrameType::kPong), 22);
  EXPECT_EQ(WireOf(Action::kPong), 2);
  EXPECT_EQ(WireOf(WireMeldType::kPong), 1);
  // `discard` is a client frame and a window.
  EXPECT_EQ(WireOf(FrameType::kDiscard), 10);
  EXPECT_EQ(WireOf(Window::kDiscard), 1);
}

}  // namespace
}  // namespace mahjong::protocol
