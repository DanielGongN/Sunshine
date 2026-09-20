/**
 * @file tests/unit/test_gateway.cpp
 * @brief Test src/gateway.*.
 */

#include "src/gateway.h"

#include <array>
#include <cstdint>

namespace gateway {
  std::uint16_t read_u16_be(char high, char low);
}

#include "../tests_common.h"

TEST(GatewayLengthParsingTests, ParsesSignedCharBytesAsUnsigned) {
  const std::array<char, 2> len_173 {static_cast<char>(0x00), static_cast<char>(0xAD)};
  const std::array<char, 2> len_230 {static_cast<char>(0x00), static_cast<char>(0xE6)};
  const std::array<char, 2> len_392 {static_cast<char>(0x01), static_cast<char>(0x88)};
  const std::array<char, 2> len_752 {static_cast<char>(0x02), static_cast<char>(0xF0)};

  EXPECT_EQ(173, gateway::read_u16_be(len_173[0], len_173[1]));
  EXPECT_EQ(230, gateway::read_u16_be(len_230[0], len_230[1]));
  EXPECT_EQ(392, gateway::read_u16_be(len_392[0], len_392[1]));
  EXPECT_EQ(752, gateway::read_u16_be(len_752[0], len_752[1]));
}

TEST(GatewayControlAdmissionTests, AllowsUpToHardLimit) {
  gateway::detail::control_send_admission_t admission {};

  for (std::size_t i = 0; i < gateway::detail::CONTROL_MAX_IN_FLIGHT; ++i) {
    ASSERT_TRUE(admission.try_acquire());
  }
  EXPECT_FALSE(admission.try_acquire());
  EXPECT_EQ(admission.in_flight, gateway::detail::CONTROL_MAX_IN_FLIGHT);
}

TEST(GatewayControlAdmissionTests, ReleaseMakesCapacityAvailable) {
  gateway::detail::control_send_admission_t admission {gateway::detail::CONTROL_MAX_IN_FLIGHT};

  EXPECT_FALSE(admission.try_acquire());
  admission.release();
  EXPECT_TRUE(admission.try_acquire());
  EXPECT_EQ(admission.in_flight, gateway::detail::CONTROL_MAX_IN_FLIGHT);
}
