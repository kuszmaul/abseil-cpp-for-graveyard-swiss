#include "absl/container/internal/bit_mask.h"

#include "absl/base/config.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace container_internal {

using ::testing::ElementsAre;

TEST(BitMask, Smoke) {
  EXPECT_FALSE((BitMask<uint8_t, 8>(0)));
  EXPECT_TRUE((BitMask<uint8_t, 8>(5)));

  EXPECT_THAT((BitMask<uint8_t, 8>(0)), ElementsAre());
  EXPECT_THAT((BitMask<uint8_t, 8>(0x1)), ElementsAre(0));
  EXPECT_THAT((BitMask<uint8_t, 8>(0x2)), ElementsAre(1));
  EXPECT_THAT((BitMask<uint8_t, 8>(0x3)), ElementsAre(0, 1));
  EXPECT_THAT((BitMask<uint8_t, 8>(0x4)), ElementsAre(2));
  EXPECT_THAT((BitMask<uint8_t, 8>(0x5)), ElementsAre(0, 2));
  EXPECT_THAT((BitMask<uint8_t, 8>(0x55)), ElementsAre(0, 2, 4, 6));
  EXPECT_THAT((BitMask<uint8_t, 8>(0xAA)), ElementsAre(1, 3, 5, 7));
}

TEST(BitMask, WithShift) {
  // See the non-SSE version of Group for details on what this math is for.
  uint64_t ctrl = 0x1716151413121110;
  uint64_t hash = 0x12;
  constexpr uint64_t msbs = 0x8080808080808080ULL;
  constexpr uint64_t lsbs = 0x0101010101010101ULL;
  auto x = ctrl ^ (lsbs * hash);
  uint64_t mask = (x - lsbs) & ~x & msbs;
  EXPECT_EQ(0x0000000080800000, mask);

  BitMask<uint64_t, 8, 3> b(mask);
  EXPECT_EQ(*b, 2);
}

TEST(BitMask, LeadingTrailing) {
  EXPECT_EQ((BitMask<uint32_t, 16>(0x00001a40).LeadingZeros()), 3);
  EXPECT_EQ((BitMask<uint32_t, 16>(0x00001a40).TrailingZeros()), 6);

  EXPECT_EQ((BitMask<uint32_t, 16>(0x00000001).LeadingZeros()), 15);
  EXPECT_EQ((BitMask<uint32_t, 16>(0x00000001).TrailingZeros()), 0);

  EXPECT_EQ((BitMask<uint32_t, 16>(0x00008000).LeadingZeros()), 0);
  EXPECT_EQ((BitMask<uint32_t, 16>(0x00008000).TrailingZeros()), 15);

  EXPECT_EQ((BitMask<uint64_t, 8, 3>(0x0000008080808000).LeadingZeros()), 3);
  EXPECT_EQ((BitMask<uint64_t, 8, 3>(0x0000008080808000).TrailingZeros()), 1);

  EXPECT_EQ((BitMask<uint64_t, 8, 3>(0x0000000000000080).LeadingZeros()), 7);
  EXPECT_EQ((BitMask<uint64_t, 8, 3>(0x0000000000000080).TrailingZeros()), 0);

  EXPECT_EQ((BitMask<uint64_t, 8, 3>(0x8000000000000000).LeadingZeros()), 0);
  EXPECT_EQ((BitMask<uint64_t, 8, 3>(0x8000000000000000).TrailingZeros()), 7);
}

}  // namespace container_internal
ABSL_NAMESPACE_END
}  // namespace absl
