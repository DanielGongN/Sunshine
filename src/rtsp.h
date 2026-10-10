/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <atomic>
#include <mutex>
#include <utility>

// local includes
#include "crypto.h"
#include "middleware.h"
#include "thread_safe.h"

// forward declarations
namespace video {
  struct config_t;
}

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 26;

  struct launch_session_t {
    uint32_t id;

    crypto::aes_t gcm_key;
    crypto::aes_t iv;

    std::string av_ping_payload;
    std::string mic_ping_payload;
    uint32_t control_connect_data;

    bool host_audio;
    std::string unique_id;
    int width;
    int height;
    int fps;
    int gcmap;
    int appid;
    int surround_info;
    std::string surround_params;
    bool continuous_audio;
    bool enable_hdr;
    bool enable_sops;

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;
    std::string rtsp_url_scheme;
    uint32_t rtsp_iv_counter;
    std::string client_cert;

    bool display_mode_pending {};  ///< Wait for ANNOUNCE before applying a middleware launch's display mode.
    int fps_x100 {};  ///< Optional validated client refresh rate in hundredths of Hz.
  };

  namespace detail {
    /** Complete a middleware launch's pending display mode from validated ANNOUNCE parameters. */
    void apply_client_display_mode(launch_session_t &session, const video::config_t &client_config);

    class launch_slot_t {
    public:
      bool raise(std::shared_ptr<launch_session_t> session) {
        std::lock_guard lock {_mutex};
        if (_session) {
          return false;
        }
        _session = std::move(session);
        _announced = false;
        return true;
      }

      std::shared_ptr<launch_session_t> view() const {
        std::lock_guard lock {_mutex};
        return _session;
      }

      std::shared_ptr<launch_session_t> clear(std::uint32_t id) {
        std::lock_guard lock {_mutex};
        if (!_session || _session->id != id) {
          return nullptr;
        }
        return std::exchange(_session, nullptr);
      }

      bool claim_announce(std::uint32_t id) {
        std::lock_guard lock {_mutex};
        if (!_session || _session->id != id || _announced) {
          return false;
        }
        _announced = true;
        return true;
      }

    private:
      mutable std::mutex _mutex;
      std::shared_ptr<launch_session_t> _session;
      bool _announced {};
    };
  }  // namespace detail

  /** Returns false when another RTSP handshake is pending. */
  bool launch_session_raise(std::shared_ptr<launch_session_t> launch_session);
  bool launch_session_pending();

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();

  /**
   * @brief Get the number of running sessions.
   * @return Count of sessions that are actively running, excluding sessions still being cleaned up.
   */
  int running_session_count();

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions(
    middleware::disconnect_notification_type_e type = middleware::disconnect_notification_type_e::server_requested_stop,
    std::string_view message = "Server requested stream termination"
  );
  void terminate_sessions_by_cert(std::string_view cert);

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
