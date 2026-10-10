/**
 * @file tests/unit/test_stream_ping.cpp
 * @brief Test required media deadlines, late optional activation, and shutdown.
 */

#include "../tests_common.h"
#include "src/stream_ping.h"
#include "src/thread_safe.h"

#include <atomic>
#include <chrono>
#include <future>

using namespace std::chrono_literals;
using ping_result_e = stream::detail::initial_ping_result_e;

TEST(StreamPingTests, RequiredVideoTimesOutWithoutPing) {
  std::chrono::steady_clock::time_point now {};
  const auto start = now;
  auto result = stream::detail::wait_for_initial_ping(
    10s,
    []() {
      return false;
    },
    [&](auto delay) {
      EXPECT_LE(delay, 100ms);
      now += delay;
      return false;
    },
    [&]() {
      return now;
    }
  );

  EXPECT_EQ(result, ping_result_e::timeout);
  EXPECT_EQ(now - start, 10s);
}

TEST(StreamPingTests, RequiredVideoAcceptsMatchingPingBeforeDeadline) {
  auto result = stream::detail::wait_for_initial_ping(
    10s,
    []() {
      return false;
    },
    [](auto) {
      return true;
    }
  );

  EXPECT_EQ(result, ping_result_e::received);
}

TEST(StreamPingTests, OptionalMediaAcceptsPingAfterFormerTenSecondDeadline) {
  std::chrono::steady_clock::time_point now {};
  const auto start = now;
  auto result = stream::detail::wait_for_initial_ping(
    std::nullopt,
    [&]() {
      return now - start >= 20s;
    },
    [&](auto delay) {
      EXPECT_LE(delay, 100ms);
      now += delay;
      return now - start >= 12s;
    },
    [&]() {
      return now;
    }
  );

  EXPECT_EQ(result, ping_result_e::received);
  EXPECT_EQ(now - start, 12s);
}

TEST(StreamPingTests, OptionalMediaWaitsUntilSessionStopsWhenPingNeverArrives) {
  std::chrono::steady_clock::time_point now {};
  const auto start = now;
  auto result = stream::detail::wait_for_initial_ping(
    std::nullopt,
    [&]() {
      return now - start >= 30s;
    },
    [&](auto delay) {
      now += delay;
      return false;
    },
    [&]() {
      return now;
    }
  );

  EXPECT_EQ(result, ping_result_e::cancelled);
  EXPECT_EQ(now - start, 30s);
}

TEST(StreamPingTests, InvalidPingsDoNotPreventLateOptionalActivation) {
  int received_packets = 0;
  auto result = stream::detail::wait_for_initial_ping(
    std::nullopt,
    []() {
      return false;
    },
    [&](auto) {
      return ++received_packets == 5;
    }
  );

  EXPECT_EQ(result, ping_result_e::received);
  EXPECT_EQ(received_packets, 5);
}

TEST(StreamPingTests, RequiredDeadlineStillAppliesDuringInvalidTraffic) {
  std::chrono::steady_clock::time_point now {};
  const auto start = now;
  auto result = stream::detail::wait_for_initial_ping(
    10s,
    []() {
      return false;
    },
    [&](auto) {
      now += 1ms;
      return false;
    },
    [&]() {
      return now;
    }
  );

  EXPECT_EQ(result, ping_result_e::timeout);
  EXPECT_EQ(now - start, 10s);
}

TEST(StreamPingTests, ContinuousInvalidTrafficDoesNotPreventCancellation) {
  int received_packets = 0;
  auto result = stream::detail::wait_for_initial_ping(
    std::nullopt,
    [&]() {
      return received_packets >= 10;
    },
    [&](auto) {
      ++received_packets;
      return false;
    }
  );

  EXPECT_EQ(result, ping_result_e::cancelled);
  EXPECT_EQ(received_packets, 10);
}

TEST(StreamPingTests, AlreadyStoppedSessionDoesNotWaitForPing) {
  auto result = stream::detail::wait_for_initial_ping(
    std::nullopt,
    []() {
      return true;
    },
    [](auto) {
      ADD_FAILURE() << "Stopped sessions must not wait for a PING";
      return false;
    }
  );

  EXPECT_EQ(result, ping_result_e::cancelled);
}

TEST(StreamPingTests, SessionStopTakesPrecedenceOverConcurrentMatchingPing) {
  bool stopped = false;
  auto result = stream::detail::wait_for_initial_ping(
    std::nullopt,
    [&]() {
      return stopped;
    },
    [&](auto) {
      stopped = true;
      return true;
    }
  );

  EXPECT_EQ(result, ping_result_e::cancelled);
}

TEST(StreamPingTests, PendingOptionalQueueWaitExitsPromptlyOnSessionStop) {
  safe::queue_t<int> messages;
  std::atomic<bool> stopped {false};
  std::promise<void> waiting;
  auto waiting_future = waiting.get_future();
  bool announced_wait = false;
  auto result_future = std::async(std::launch::async, [&]() {
    return stream::detail::wait_for_initial_ping(
      std::nullopt,
      [&]() {
        return stopped.load();
      },
      [&](auto delay) {
        EXPECT_LE(delay, 100ms);
        if (!announced_wait) {
          announced_wait = true;
          waiting.set_value();
        }
        return static_cast<bool>(messages.pop(delay));
      }
    );
  });

  const auto waiting_status = waiting_future.wait_for(2s);
  stopped.store(true);
  const auto stopped_status = result_future.wait_for(2s);
  // Ensure a failed cancellation assertion cannot leave the test blocked in pop().
  messages.stop();

  EXPECT_EQ(waiting_status, std::future_status::ready);
  EXPECT_EQ(stopped_status, std::future_status::ready);
  EXPECT_EQ(result_future.get(), ping_result_e::cancelled);
}
