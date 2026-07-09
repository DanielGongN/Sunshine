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
   *
   * Thread-safe. Emits a disconnected event with type=1, meaning Sunshine stream disconnect.
   */
  void notify_client_disconnected(std::string_view message);
  void notify_client_disconnected(std::u8string_view message);

  /**
   * @brief Notify middleware that a Moonlight client connection state changed.
   * @param connected true if a client just connected, false if it disconnected.
   *
   * Thread-safe. Used by the heartbeat to report net_connected status.
   */
  void notify_client_state(bool connected);

}  // namespace middleware
