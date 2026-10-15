/**
 * @file src/stream_disconnect.h
 * @brief Thread-safe first-cause tracking for a streaming session.
 */
#pragma once

// standard includes
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// local includes
#include "middleware.h"

namespace stream::detail {
  inline constexpr std::uint32_t CONTROL_TERMINATION_GRACEFUL = 0x80030023;
  // Private client termination codes selected by upstream commands and local timeouts.
  inline constexpr std::uint32_t CONTROL_TERMINATION_PLATFORM_DEFAULT_EXIT = 0x8004E000;
  inline constexpr std::uint32_t CONTROL_TERMINATION_FORCE_TIMEOUT = 0x8004E001;
  inline constexpr std::uint32_t CONTROL_TERMINATION_STANDBY_TIMEOUT = 0x8004E002;
  inline constexpr std::uint32_t CONTROL_TERMINATION_GAME_PROCESS_EXIT = 0x8004E003;
  inline constexpr std::uint32_t CONTROL_TERMINATION_NO_AVAILABLE_TIME = 0x8004E004;

  // Client-to-server ENet disconnect data, separate from server termination messages.
  inline constexpr std::uint32_t CLIENT_DISCONNECT_REQUESTED_STOP = 0x8004F001;
  inline constexpr std::uint32_t CLIENT_DISCONNECT_DISPLACED = 0x8004F002;

  struct upstream_disconnect_t {
    std::string message;
    std::uint32_t termination_code {CONTROL_TERMINATION_GRACEFUL};
  };

  struct disconnect_reason_t {
    middleware::disconnect_notification_type_e type;
    std::string message;
  };

  inline disconnect_reason_t client_disconnect_reason(std::uint32_t code) {
    auto type = middleware::disconnect_notification_type_e::peer_disconnected_unknown;
    std::string_view message = "Control peer disconnected; client intent is unknown";
    switch (code) {
      case CLIENT_DISCONNECT_REQUESTED_STOP:
        type = middleware::disconnect_notification_type_e::client_requested_stop;
        message = "Client reported user-requested disconnect";
        break;
      case CLIENT_DISCONNECT_DISPLACED:
        type = middleware::disconnect_notification_type_e::client_displaced;
        message = "Client reported being displaced by another session";
        break;
    }
    return {type, std::format("{} (ENet data=0x{:08X})", message, code)};
  }

  class disconnect_state_t {
  public:
    /**
     * @brief Record a fatal error before waking other session threads.
     */
    bool record(middleware::disconnect_notification_type_e type, std::string_view message) {
      std::lock_guard lock {_mutex};
      if (_reason) {
        return false;
      }
      _reason = disconnect_reason_t {type, std::string {message}};
      return true;
    }

    /**
     * @brief Claim the stop notification once, preserving any earlier error.
     */
    std::optional<disconnect_reason_t> claim(middleware::disconnect_notification_type_e type, std::string_view message) {
      std::lock_guard lock {_mutex};
      if (_claimed) {
        return std::nullopt;
      }
      if (!_reason) {
        _reason = disconnect_reason_t {type, std::string {message}};
      }
      _claimed = true;
      return _reason;
    }

  private:
    std::mutex _mutex;
    std::optional<disconnect_reason_t> _reason;
    bool _claimed {};
  };
}  // namespace stream::detail
