/**
 * @file src/client_mic.h
 * @brief Declarations for client microphone uplink handling.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>
#include <vector>

// local includes
#include "crypto.h"
#include "thread_safe.h"

namespace client_mic {
  constexpr std::uint32_t ENCRYPTION_FLAG = 0x08;
  constexpr std::uint8_t RTP_PAYLOAD_TYPE = 110;
  constexpr int SAMPLE_RATE = 48000;
  constexpr int DEFAULT_CHANNELS = 1;
  constexpr int DEFAULT_PACKET_DURATION = 10;
  constexpr int COMPAT_PACKET_DURATION = 20;
  constexpr int MAX_CHANNELS = 2;

  struct config_t {
    bool enabled {};
    int channels {DEFAULT_CHANNELS};
    int sample_rate {SAMPLE_RATE};
    int packet_duration {DEFAULT_PACKET_DURATION};
    bool encrypted {};

    [[nodiscard]] int frame_size() const;
  };

  using packet_queue_t = std::shared_ptr<safe::queue_t<std::vector<std::uint8_t>>>;

  [[nodiscard]] bool validate_config(const config_t &config);
  void receive(safe::mail_t mail, const config_t &config, packet_queue_t packets, crypto::cipher::gcm_t *cipher);
}  // namespace client_mic
