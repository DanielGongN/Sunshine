/**
 * @file src/stream_recovery.h
 * @brief Independent control responsiveness and outgoing media lifetime.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace stream::detail {
  constexpr auto RECOVERY_WAIT = std::chrono::seconds {1};

  inline std::uint32_t control_transport_timeout(std::chrono::milliseconds timeout) {
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(timeout.count(), 0, UINT32_MAX - 2000) + 2000);
  }

  enum class recovery_transition_e {
    none,
    waiting,
    recovered,
    expired,
  };

  class control_recovery_t {
  public:
    using clock = std::chrono::steady_clock;

    void start(clock::time_point now) {
      std::lock_guard lock {_mutex};
      _last_control = now;
      _stopped = false;
      _control_available.store(true, std::memory_order_release);
      _media_available.store(true, std::memory_order_release);
    }

    recovery_transition_e receive(clock::time_point now, std::chrono::milliseconds timeout) {
      std::lock_guard lock {_mutex};
      if (_stopped || now - _last_control >= timeout) {
        _stopped = true;
        _control_available.store(false, std::memory_order_release);
        _media_available.store(false, std::memory_order_release);
        return recovery_transition_e::expired;
      }
      _last_control = now;
      return _control_available.exchange(true, std::memory_order_acq_rel) ? recovery_transition_e::none : recovery_transition_e::recovered;
    }

    recovery_transition_e poll(clock::time_point now, std::chrono::milliseconds timeout) {
      std::lock_guard lock {_mutex};
      if (_stopped || now - _last_control >= timeout) {
        _stopped = true;
        _control_available.store(false, std::memory_order_release);
        _media_available.store(false, std::memory_order_release);
        return recovery_transition_e::expired;
      }
      // A transient control gap releases input without interrupting outgoing media.
      if (now - _last_control >= RECOVERY_WAIT && _control_available.exchange(false, std::memory_order_acq_rel)) {
        return recovery_transition_e::waiting;
      }
      return recovery_transition_e::none;
    }

    void stop() {
      std::lock_guard lock {_mutex};
      _stopped = true;
      _control_available.store(false, std::memory_order_release);
      _media_available.store(false, std::memory_order_release);
    }

    bool control_available() const {
      return _control_available.load(std::memory_order_acquire);
    }

    bool media_available() const {
      return _media_available.load(std::memory_order_acquire);
    }

    std::int64_t idle_ms(clock::time_point now) const {
      std::lock_guard lock {_mutex};
      return std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_control).count();
    }

  private:
    mutable std::mutex _mutex;
    clock::time_point _last_control {};
    bool _stopped {true};
    std::atomic_bool _control_available {false};
    std::atomic_bool _media_available {false};
  };
}  // namespace stream::detail
