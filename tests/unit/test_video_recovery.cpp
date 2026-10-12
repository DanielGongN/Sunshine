/**
 * @file tests/unit/test_video_recovery.cpp
 * @brief Test bounded encoder recovery and nonblocking capture image draining.
 */

#include "../tests_common.h"
#include "src/thread_safe.h"
#include "src/video_recovery.h"

#include <chrono>
#include <memory>

using disconnect_type_e = middleware::disconnect_notification_type_e;

TEST(VideoRecoveryTests, RecoveredEncoderFailureDoesNotOverrideLaterClientStop) {
  stream::detail::disconnect_state_t disconnect;
  video::detail::encoder_recovery_t recovery {disconnect};

  EXPECT_TRUE(recovery.retry({disconnect_type_e::server_encoder_error, "NVENC frame 36199 timed out"}, true));
  EXPECT_EQ(recovery.restarts(), 1);

  auto reason = disconnect.claim(disconnect_type_e::client_requested_stop, "Client stopped the recovered stream");
  ASSERT_TRUE(reason);
  EXPECT_EQ(reason->type, disconnect_type_e::client_requested_stop);
  EXPECT_EQ(reason->message, "Client stopped the recovered stream");
}

TEST(VideoRecoveryTests, PersistentEncoderFailureStopsAfterThreeRebuildsWithFinalReason) {
  stream::detail::disconnect_state_t disconnect;
  video::detail::encoder_recovery_t recovery {disconnect};

  for (int attempt = 0; attempt < 3; ++attempt) {
    ASSERT_TRUE(recovery.retry({disconnect_type_e::server_encoder_error, "Transient encoding failure"}, true));
  }
  EXPECT_FALSE(recovery.retry({disconnect_type_e::server_encoder_error, "Replacement encoder initialization failed"}, true));
  EXPECT_EQ(recovery.restarts(), 3);

  auto reason = disconnect.claim(disconnect_type_e::server_internal_error, "Video worker ended");
  ASSERT_TRUE(reason);
  EXPECT_EQ(reason->type, disconnect_type_e::server_encoder_error);
  EXPECT_EQ(reason->message, "Replacement encoder initialization failed");
  EXPECT_FALSE(disconnect.claim(disconnect_type_e::other, "Cleanup"));
}

TEST(VideoRecoveryTests, CaptureFailureRemainsTerminalWithRecoverySupported) {
  stream::detail::disconnect_state_t disconnect;
  video::detail::encoder_recovery_t recovery {disconnect};

  EXPECT_FALSE(recovery.retry({disconnect_type_e::server_capture_error, "Initial capture image unavailable"}, true));
  EXPECT_EQ(recovery.restarts(), 0);
  auto reason = disconnect.claim(disconnect_type_e::server_internal_error, "Video worker ended");
  ASSERT_TRUE(reason);
  EXPECT_EQ(reason->type, disconnect_type_e::server_capture_error);
}

TEST(VideoRecoveryTests, EncoderWithoutAsyncTeardownFailsImmediately) {
  stream::detail::disconnect_state_t disconnect;
  video::detail::encoder_recovery_t recovery {disconnect};

  EXPECT_FALSE(recovery.retry({disconnect_type_e::server_encoder_error, "Software encoding failed"}, false));
  EXPECT_EQ(recovery.restarts(), 0);
  auto reason = disconnect.claim(disconnect_type_e::server_internal_error, "Video worker ended");
  ASSERT_TRUE(reason);
  EXPECT_EQ(reason->type, disconnect_type_e::server_encoder_error);
  EXPECT_EQ(reason->message, "Software encoding failed");
}

TEST(VideoRecoveryTests, ExhaustedRecoveryPreservesEarlierTerminalFailure) {
  stream::detail::disconnect_state_t disconnect;
  video::detail::encoder_recovery_t recovery {disconnect};

  ASSERT_TRUE(disconnect.record(disconnect_type_e::control_timeout, "Control channel expired"));
  EXPECT_FALSE(recovery.retry({disconnect_type_e::server_encoder_error, "Encoder ended during cleanup"}, false));
  auto reason = disconnect.claim(disconnect_type_e::other, "Cleanup");
  ASSERT_TRUE(reason);
  EXPECT_EQ(reason->type, disconnect_type_e::control_timeout);
  EXPECT_EQ(reason->message, "Control channel expired");
}

TEST(CaptureImageDrainTests, EmptyAndConsumedEventsDoNotWaitForAnotherImage) {
  safe::event_t<int> images;
  EXPECT_FALSE(images.try_pop());

  images.raise(7);
  ASSERT_TRUE(images.peek());
  // An encoder may consume the pending image after the capture worker's peek.
  ASSERT_EQ(*images.pop(), 7);
  EXPECT_FALSE(images.try_pop());
}

TEST(CaptureImageDrainTests, DrainingReleasesImageOwnership) {
  safe::event_t<std::shared_ptr<int>> images;
  auto image = std::make_shared<int>(7);
  std::weak_ptr<int> released = image;
  images.raise(std::move(image));

  auto drained = images.try_pop();
  ASSERT_TRUE(drained);
  EXPECT_EQ(*drained, 7);
  EXPECT_FALSE(images.try_pop());
  drained.reset();
  EXPECT_TRUE(released.expired());
}

TEST(CaptureImageDrainTests, StoppedEventsDoNotExposePendingImages) {
  safe::event_t<int> images;
  images.raise(7);
  images.stop();
  EXPECT_FALSE(images.try_pop());
}

TEST(CaptureImageDrainTests, TimedPopClearsBooleanAndRawPointerEvents) {
  safe::event_t<bool> ready;
  ready.raise(true);
  EXPECT_TRUE(ready.pop(std::chrono::milliseconds {0}));
  EXPECT_FALSE(ready.pop(std::chrono::milliseconds {0}));

  safe::event_t<int *> images;
  int image = 7;
  images.raise(&image);
  EXPECT_EQ(images.pop(std::chrono::milliseconds {0}), &image);
  EXPECT_EQ(images.pop(std::chrono::milliseconds {0}), nullptr);
}
