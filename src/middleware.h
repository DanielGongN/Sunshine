/**
 * @file src/middleware.h
 * @brief Declarations for the middle-platform WebSocket client.
 */
#pragma once

// local includes
#include "platform/common.h"

namespace middleware {

  /**
   * @brief Start the middle-platform WebSocket connection.
   * @return A deinit guard that stops the connection on destruction.
   * @retval nullptr on failure (Sunshine continues without middleware).
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> start();

}  // namespace middleware
