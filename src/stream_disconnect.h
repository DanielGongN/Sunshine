/**
 * @file src/stream_disconnect.h
 * @brief Thread-safe first-cause tracking for a streaming session.
 */
#pragma once

// standard includes
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// local includes
#include "middleware.h"

namespace stream::detail {
  struct disconnect_reason_t {
    middleware::disconnect_notification_type_e type;
    std::string message;
  };

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
