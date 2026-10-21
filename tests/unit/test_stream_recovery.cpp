/**
 * @file tests/unit/test_stream_recovery.cpp
 * @brief Control gaps release input without suspending outgoing media.
 */
#include "../tests_common.h"
#include "src/stream_recovery.h"

using namespace std::chrono_literals;
using transition = stream::detail::recovery_transition_e;

TEST(StreamRecoveryTests, FiftyShortOutagesEachGetAFreshDeadline) {
  stream::detail::control_recovery_t recovery;
  std::chrono::steady_clock::time_point now {};
  recovery.start(now);
  for (int attempt = 0; attempt < 50; ++attempt) {
    now += 1s;
    EXPECT_EQ(recovery.poll(now, 10s), transition::waiting);
    EXPECT_FALSE(recovery.control_available());
    EXPECT_TRUE(recovery.media_available());
    EXPECT_EQ(recovery.poll(now + 1s, 10s), transition::none);
    now += 4s;
    EXPECT_EQ(recovery.receive(now, 10s), transition::recovered);
    EXPECT_TRUE(recovery.control_available());
    EXPECT_TRUE(recovery.media_available());
    EXPECT_EQ(recovery.idle_ms(now), 0);
  }
}

TEST(StreamRecoveryTests, LatePacketsCannotReviveAnExpiredOrStoppedSession) {
  stream::detail::control_recovery_t recovery;
  const std::chrono::steady_clock::time_point start {};
  recovery.start(start);
  EXPECT_EQ(recovery.receive(start + 10s, 10s), transition::expired);
  EXPECT_FALSE(recovery.control_available());
  EXPECT_FALSE(recovery.media_available());
  EXPECT_EQ(recovery.receive(start + 11s, 10s), transition::expired);
  recovery.start(start);
  recovery.stop();
  EXPECT_EQ(recovery.receive(start + 100ms, 10s), transition::expired);
  EXPECT_FALSE(recovery.control_available());
  EXPECT_FALSE(recovery.media_available());
}

TEST(StreamRecoveryTests, MissingControlExpiresAtTheConfiguredDeadline) {
  stream::detail::control_recovery_t recovery;
  const std::chrono::steady_clock::time_point start {};
  recovery.start(start);
  EXPECT_EQ(recovery.poll(start + 999ms, 10s), transition::none);
  EXPECT_TRUE(recovery.control_available());
  EXPECT_TRUE(recovery.media_available());
  EXPECT_EQ(recovery.poll(start + 1s, 10s), transition::waiting);
  EXPECT_FALSE(recovery.control_available());
  EXPECT_TRUE(recovery.media_available());
  EXPECT_EQ(recovery.poll(start + 9999ms, 10s), transition::none);
  EXPECT_TRUE(recovery.media_available());
  EXPECT_EQ(recovery.poll(start + 10s, 10s), transition::expired);
  EXPECT_FALSE(recovery.control_available());
  EXPECT_FALSE(recovery.media_available());
}

TEST(StreamRecoveryTests, CustomDeadlineStillStopsMediaBeforeTheDefaultTimeout) {
  stream::detail::control_recovery_t recovery;
  const std::chrono::steady_clock::time_point start {};
  recovery.start(start);
  EXPECT_EQ(recovery.poll(start + 1s, 2s), transition::waiting);
  EXPECT_FALSE(recovery.control_available());
  EXPECT_TRUE(recovery.media_available());
  EXPECT_EQ(recovery.poll(start + 1999ms, 2s), transition::none);
  EXPECT_TRUE(recovery.media_available());
  EXPECT_EQ(recovery.poll(start + 2s, 2s), transition::expired);
  EXPECT_FALSE(recovery.media_available());
}

TEST(StreamRecoveryTests, TransportDeadlineIncludesMarginAndHandlesLargeConfiguration) {
  EXPECT_EQ(stream::detail::control_transport_timeout(10s), 12000);
  EXPECT_EQ(stream::detail::control_transport_timeout(0ms), 2000);
  EXPECT_EQ(stream::detail::control_transport_timeout(std::chrono::milliseconds {INT32_MAX}), 2147485647U);
}

TEST(StreamRecoveryTests, OneFiveAndEightSecondOutagesRecoverButTwelveSecondsExpires) {
  const std::chrono::steady_clock::time_point start {};
  for (const auto gap : {1s, 5s, 8s, 12s}) {
    stream::detail::control_recovery_t recovery;
    recovery.start(start);
    EXPECT_EQ(recovery.poll(start + 1s, 10s), transition::waiting);
    EXPECT_EQ(recovery.receive(start + gap, 10s), gap < 10s ? transition::recovered : transition::expired);
    EXPECT_EQ(recovery.control_available(), gap < 10s);
    EXPECT_EQ(recovery.media_available(), gap < 10s);
  }
}

TEST(StreamRecoveryTests, MediaRemainsAvailableThroughoutRepeatedControlGaps) {
  stream::detail::control_recovery_t recovery;
  std::chrono::steady_clock::time_point now {};
  recovery.start(now);
  for (int attempt = 0; attempt < 50; ++attempt) {
    for (int second = 1; second < 10; ++second) {
      now += 1s;
      EXPECT_EQ(recovery.poll(now, 10s), second == 1 ? transition::waiting : transition::none);
      EXPECT_FALSE(recovery.control_available());
      EXPECT_TRUE(recovery.media_available());
    }
    ASSERT_EQ(recovery.receive(now, 10s), transition::recovered);
    EXPECT_TRUE(recovery.control_available());
    EXPECT_TRUE(recovery.media_available());
  }
}

TEST(StreamRecoveryTests, StopWhileWaitingDisablesMediaAndRejectsRecovery) {
  stream::detail::control_recovery_t recovery;
  const std::chrono::steady_clock::time_point start {};
  recovery.start(start);
  EXPECT_EQ(recovery.poll(start + 1s, 10s), transition::waiting);
  EXPECT_TRUE(recovery.media_available());
  recovery.stop();
  EXPECT_FALSE(recovery.control_available());
  EXPECT_FALSE(recovery.media_available());
  EXPECT_EQ(recovery.receive(start + 2s, 10s), transition::expired);
  EXPECT_FALSE(recovery.media_available());
}

TEST(StreamRecoveryTests, OnlyStartingANewSessionRestoresExpiredMedia) {
  stream::detail::control_recovery_t recovery;
  const std::chrono::steady_clock::time_point start {};
  EXPECT_FALSE(recovery.control_available());
  EXPECT_FALSE(recovery.media_available());
  recovery.start(start);
  EXPECT_EQ(recovery.poll(start + 10s, 10s), transition::expired);
  EXPECT_EQ(recovery.receive(start + 11s, 10s), transition::expired);
  EXPECT_FALSE(recovery.media_available());
  recovery.start(start + 20s);
  EXPECT_TRUE(recovery.control_available());
  EXPECT_TRUE(recovery.media_available());
  EXPECT_EQ(recovery.receive(start + 21s, 10s), transition::none);
}
