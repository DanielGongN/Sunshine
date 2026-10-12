/**
 * @file src/video_recovery.h
 * @brief Bound encoder reconstruction without recording recoverable disconnects.
 */
#pragma once

// local includes
#include "stream_disconnect.h"

namespace video::detail {
  class encoder_recovery_t {
  public:
    static constexpr unsigned max_restarts = 3;

    explicit encoder_recovery_t(stream::detail::disconnect_state_t &disconnect):
        _disconnect(disconnect) {
    }

    /**
     * @brief Retry supported encoder failures, recording only terminal failures.
     */
    bool retry(const stream::detail::disconnect_reason_t &failure, bool supports_recovery) {
      if (supports_recovery && failure.type == middleware::disconnect_notification_type_e::server_encoder_error && _restarts < max_restarts) {
        ++_restarts;
        return true;
      }
      _disconnect.record(failure.type, failure.message);
      return false;
    }

    unsigned restarts() const {
      return _restarts;
    }

  private:
    stream::detail::disconnect_state_t &_disconnect;
    unsigned _restarts {};
  };
}  // namespace video::detail
