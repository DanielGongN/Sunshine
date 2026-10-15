/**
 * @file tests/unit/test_stream_disconnect.cpp
 * @brief Test first-cause ownership and concurrent stream stop notifications.
 */

#include "../tests_common.h"
#include "src/stream_disconnect.h"

#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

using disconnect_type_e = middleware::disconnect_notification_type_e;

TEST(StreamDisconnectTests, KnownClientCodesPreserveDistinctReasons) {
  const auto requested = stream::detail::client_disconnect_reason(0x8004F001U);
  EXPECT_EQ(requested.type, disconnect_type_e::client_requested_stop);
  EXPECT_NE(requested.message.find("0x8004F001"), std::string::npos);

  const auto displaced = stream::detail::client_disconnect_reason(0x8004F002U);
  EXPECT_EQ(displaced.type, disconnect_type_e::client_displaced);
  EXPECT_NE(displaced.message.find("0x8004F002"), std::string::npos);
}

TEST(StreamDisconnectTests, UnknownClientCodesNeverImplyActiveExit) {
  for (const std::uint32_t code : {0U, 0x8004F003U, 0x8004E001U, UINT32_MAX}) {
    SCOPED_TRACE(code);
    EXPECT_EQ(stream::detail::client_disconnect_reason(code).type, disconnect_type_e::peer_disconnected_unknown);
  }
}

TEST(StreamDisconnectTests, ClientCodesCannotReplaceAnEarlierServerFailure) {
  for (const auto code : {0x8004F001U, 0x8004F002U}) {
    SCOPED_TRACE(code);
    stream::detail::disconnect_state_t state;
    ASSERT_TRUE(state.record(disconnect_type_e::server_encoder_error, "Earlier encoder failure"));
    const auto client_reason = stream::detail::client_disconnect_reason(code);
    const auto reason = state.claim(client_reason.type, client_reason.message);
    ASSERT_TRUE(reason);
    EXPECT_EQ(reason->type, disconnect_type_e::server_encoder_error);
    EXPECT_EQ(reason->message, "Earlier encoder failure");
  }
}

TEST(StreamDisconnectTests, EncoderFailureSurvivesTimeoutAndCleanup) {
  stream::detail::disconnect_state_t state;
  std::string message = "NVENC failed on frame 123";
  ASSERT_TRUE(state.record(disconnect_type_e::server_encoder_error, message));
  message.clear();

  auto reason = state.claim(disconnect_type_e::control_timeout, "Control timed out");
  ASSERT_TRUE(reason);
  EXPECT_EQ(reason->type, disconnect_type_e::server_encoder_error);
  EXPECT_EQ(reason->message, "NVENC failed on frame 123");
  EXPECT_FALSE(state.claim(disconnect_type_e::other, "Cleanup"));
  EXPECT_FALSE(state.record(disconnect_type_e::server_shutdown, "Shutdown"));
}

TEST(StreamDisconnectTests, ExplicitStopCannotBeReclassifiedByLateWorkerFailure) {
  stream::detail::disconnect_state_t state;
  auto reason = state.claim(disconnect_type_e::upstream_requested_stop, "Upstream requested stop");
  ASSERT_TRUE(reason);
  EXPECT_EQ(reason->type, disconnect_type_e::upstream_requested_stop);
  EXPECT_EQ(reason->message, "Upstream requested stop");
  EXPECT_FALSE(state.record(disconnect_type_e::server_capture_error, "Capture ended"));
  EXPECT_FALSE(state.claim(disconnect_type_e::server_capture_error, "Capture ended"));
}

TEST(StreamDisconnectTests, ConcurrentStopsReportFirstFailureExactlyOnce) {
  stream::detail::disconnect_state_t state;
  ASSERT_TRUE(state.record(disconnect_type_e::server_capture_error, "Capture device failed"));
  constexpr int thread_count = 8;
  std::barrier ready {thread_count};
  std::atomic<int> notifications {};
  std::atomic<int> incorrect_reasons {};
  std::vector<std::thread> threads;

  for (int i = 0; i < thread_count; ++i) {
    threads.emplace_back([&]() {
      ready.arrive_and_wait();
      auto reason = state.claim(disconnect_type_e::control_timeout, "Later timeout");
      if (reason) {
        ++notifications;
        if (reason->type != disconnect_type_e::server_capture_error || reason->message != "Capture device failed") {
          ++incorrect_reasons;
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(notifications.load(), 1);
  EXPECT_EQ(incorrect_reasons.load(), 0);
}

TEST(StreamDisconnectTests, CompetingFailuresPreserveOneMatchingTypeAndMessage) {
  stream::detail::disconnect_state_t state;
  std::barrier ready {2};
  bool capture_won {};
  bool encoder_won {};
  std::thread capture {[&]() {
    ready.arrive_and_wait();
    capture_won = state.record(disconnect_type_e::server_capture_error, "Capture failed");
  }};
  std::thread encoder {[&]() {
    ready.arrive_and_wait();
    encoder_won = state.record(disconnect_type_e::server_encoder_error, "Encoding failed");
  }};
  capture.join();
  encoder.join();

  ASSERT_NE(capture_won, encoder_won);
  auto reason = state.claim(disconnect_type_e::other, "Cleanup");
  ASSERT_TRUE(reason);
  EXPECT_EQ(reason->type, capture_won ? disconnect_type_e::server_capture_error : disconnect_type_e::server_encoder_error);
  EXPECT_EQ(reason->message, capture_won ? "Capture failed" : "Encoding failed");
}
