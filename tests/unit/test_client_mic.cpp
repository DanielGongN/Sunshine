/**
 * @file tests/unit/test_client_mic.cpp
 * @brief Test src/client_mic.*.
 */
#include "../tests_common.h"

#include <src/client_mic.h>

TEST(ClientMicConfigTests, AcceptsAdvertisedDefaults) {
  client_mic::config_t config;

  EXPECT_TRUE(client_mic::validate_config(config));
  EXPECT_EQ(480, config.frame_size());
}

TEST(ClientMicConfigTests, RejectsUnsupportedFormatChanges) {
  client_mic::config_t config;

  config.sample_rate = 44100;
  EXPECT_FALSE(client_mic::validate_config(config));

  config = {};
  config.packet_duration = 5;
  EXPECT_FALSE(client_mic::validate_config(config));

  config = {};
  config.channels = client_mic::MAX_CHANNELS + 1;
  EXPECT_FALSE(client_mic::validate_config(config));
}

TEST(ClientMicConfigTests, AcceptsCompatPacketDuration) {
  client_mic::config_t config;
  config.packet_duration = client_mic::COMPAT_PACKET_DURATION;

  EXPECT_TRUE(client_mic::validate_config(config));
  EXPECT_EQ(960, config.frame_size());
}
