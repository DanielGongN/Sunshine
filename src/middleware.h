/**
 * @file src/middleware.h
 * @brief Declarations for the middle-platform WebSocket client.
 */
#pragma once

// lib includes
#include <nlohmann/json_fwd.hpp>

// standard includes
#include <string_view>

// local includes
#include "platform/common.h"

namespace middleware {

  // Internal reasons remain distinct; outgoing data.type uses 1/2/5/6/99.
  enum class disconnect_notification_type_e : int {
    other = 0,
    force_timeout = 1,
    standby_timeout = 2,
    client_requested_stop = 10,
    client_reported_error = 11,  ///< Reserved for explicit client error reports.
    peer_disconnected_unknown = 12,
    client_displaced = 13,  ///< Client reported being displaced by another session.
    control_timeout = 20,
    stream_start_timeout = 21,
    upstream_requested_stop = 30,
    server_requested_stop = 31,
    server_shutdown = 32,
    server_capture_error = 40,
    server_encoder_error = 41,
    server_internal_error = 42,
    gateway_error = 43,  ///< Reserved for confirmed fatal gateway failures.
    server_process_abnormal_exit = 44,  ///< Reserved for an external process monitor.
    control_protocol_error = 50,
  };

  /**
   * @brief Start the middle-platform WebSocket connection.
   * @return A deinit guard that stops the connection on destruction.
   * @retval nullptr on failure (Sunshine continues without middleware).
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> start();

  /**
   * @brief Send a JSON message to the upstream platform.
   * @param msg The JSON message to send.
   *
   * Thread-safe — may be called from any thread.
   * The message is queued and sent on the middleware's io_context thread.
   * If the middleware is not connected, the message is silently dropped.
   */
  void send_to_upstream(nlohmann::json msg);

  /**
   * @brief Notify upstream that a Moonlight client connected.
   *
   * Thread-safe. Emits one event for each client session that reaches the running state.
   */
  void notify_client_connected();

  /**
   * @brief Notify upstream that a Sunshine stream disconnected.
   * @param message Human-readable disconnect reason.
   * @param type Machine-readable disconnect reason type.
   *
   * Thread-safe. Only the first notification attempt is sent until the next incoming
   * client_connect event. All types share this limit, including timeout reminders.
   * Explicit client stops use data.type=0, timeout reminders retain 1/2, and other
   * reasons use 99. Upstream timeout commands retain 1/2 reporting; other upstream
   * commands suppress reports until client_connect. Session cleanup does not report again.
   * The top-level message is retained for compatibility.
   */
  void notify_client_disconnected(std::string_view message, disconnect_notification_type_e type = disconnect_notification_type_e::other);
  void notify_client_disconnected(std::u8string_view message, disconnect_notification_type_e type = disconnect_notification_type_e::other);

  /**
   * @brief Notify middleware that a Moonlight client connection state changed.
   * @param connected true if a client just connected, false if it disconnected.
   *
   * Thread-safe. Used by the heartbeat to report net_connected status.
   */
  void notify_client_state(bool connected);

}  // namespace middleware
