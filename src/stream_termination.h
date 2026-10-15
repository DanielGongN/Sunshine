/**
 * @file src/stream_termination.h
 * @brief Bounded ENet termination delivery before releasing a session.
 */
#pragma once

#include <chrono>
#include <enet/enet.h>
#include <optional>

namespace stream::detail {
  enum class termination_progress_e {
    waiting,
    complete,
    timed_out,
  };

  class control_termination_t {
  public:
    using clock = std::chrono::steady_clock;
    static constexpr auto max_wait = std::chrono::milliseconds {1000};

    bool started() const {
      return _deadline.has_value();
    }

    termination_progress_e poll(ENetPeer *peer, clock::time_point now) {
      if (!peer || peer->state == ENET_PEER_STATE_DISCONNECTED || peer->state == ENET_PEER_STATE_ZOMBIE) {
        return termination_progress_e::complete;
      }
      if (!_deadline) {
        _deadline = now + max_wait;
        enet_peer_disconnect_later(peer, 0);
      }
      return now < *_deadline ? termination_progress_e::waiting : termination_progress_e::timed_out;
    }

  private:
    std::optional<clock::time_point> _deadline;
  };
}  // namespace stream::detail
