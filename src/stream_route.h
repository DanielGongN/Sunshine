/**
 * @file src/stream_route.h
 * @brief Synchronized media destinations and session-token port rebinding.
 */
#pragma once

#include <boost/asio.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace stream::detail {
  struct media_destination_t {
    boost::asio::ip::udp::endpoint peer;
    boost::asio::ip::address source;
  };

  class media_route_t {
  public:
    void initialize(media_destination_t destination, std::string token, bool gateway) {
      std::lock_guard lock {_mutex};
      if (_stopped) {
        return;
      }
      _destination = std::move(destination);
      _token = std::move(token);
      _gateway = gateway;
    }

    media_destination_t snapshot() const {
      std::lock_guard lock {_mutex};
      return _destination;
    }

    bool rebind(std::string_view token, const boost::asio::ip::udp::endpoint &peer) {
      std::lock_guard lock {_mutex};
      if (_stopped || _gateway || _token.empty() || token != _token || peer.port() == 0 || peer.address() != _destination.peer.address() || peer == _destination.peer) {
        return false;
      }
      _destination.peer = peer;
      return true;
    }

    void stop() {
      std::lock_guard lock {_mutex};
      _stopped = true;
    }

  private:
    mutable std::mutex _mutex;
    media_destination_t _destination;
    std::string _token;
    bool _gateway {};
    bool _stopped {};
  };
}  // namespace stream::detail
