/**
 * @file src/stream_ping.h
 * @brief Cancellable initial PING waits for required and optional media channels.
 */
#pragma once

// standard includes
#include <chrono>
#include <functional>
#include <optional>

namespace stream::detail {
  enum class initial_ping_result_e {
    received,
    timeout,
    cancelled,
  };

  /**
   * @brief Wait for a matching PING, with no deadline for optional channels.
   * @param timeout Deadline duration for a required channel, or no deadline for an optional channel.
   * @param cancelled Return true when the session is stopping.
   * @param receive_ping Wait for at most the supplied duration and return true only for a matching PING.
   * @param now Monotonic clock, replaceable for deterministic deadline tests.
   */
  initial_ping_result_e wait_for_initial_ping(
    std::optional<std::chrono::milliseconds> timeout,
    const std::function<bool()> &cancelled,
    const std::function<bool(std::chrono::milliseconds)> &receive_ping,
    const std::function<std::chrono::steady_clock::time_point()> &now = std::chrono::steady_clock::now
  );
}  // namespace stream::detail
