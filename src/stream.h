/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <utility>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "client_mic.h"
#include "crypto.h"
#include "middleware.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 14;
  constexpr auto CONTROL_PORT = 15;
  constexpr auto AUDIO_STREAM_PORT = 16;
  constexpr auto CLIENT_MIC_STREAM_PORT = 17;

  struct session_t;

  namespace detail {
    enum class timeout_notification_e {
      none,
      force,
      standby,
    };

    timeout_notification_e select_timeout_notification(
      bool force_hold_active,
      std::int64_t force_hold_seconds,
      std::int64_t idle_seconds,
      int force_timeout,
      int standby_timeout,
      bool already_notified
    );

    boost::asio::ip::address select_video_source_address(
      const boost::asio::ip::address &peer_address,
      const std::optional<boost::asio::ip::address> &routed_source_address
    );
  }  // namespace detail

  struct config_t {
    audio::config_t audio;
    client_mic::config_t client_mic;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
  };

  namespace session {
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    int start(session_t &session, const std::string &addr_string);
    void stop(
      session_t &session,
      middleware::disconnect_notification_type_e type = middleware::disconnect_notification_type_e::other,
      std::string_view message = "Streaming session ended without a specific reason"
    );
    void join(session_t &session);
    state_e state(session_t &session);
    const std::string &client_cert(session_t &session);
  }  // namespace session
}  // namespace stream
