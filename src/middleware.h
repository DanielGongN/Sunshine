/**
 * @file src/middleware.h
 * @brief Declarations for the middle-platform WebSocket client.
 */
#pragma once

// lib includes
#include <nlohmann/json_fwd.hpp>

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

}  // namespace middleware
