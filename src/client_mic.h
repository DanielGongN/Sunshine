/**
 * @file src/client_mic.h
 * @brief Declarations for client microphone uplink handling.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// local includes
#include "crypto.h"
#include "platform/common.h"
#include "thread_safe.h"

namespace client_mic {
  constexpr std::uint32_t ENCRYPTION_FLAG = 0x08;
  constexpr std::uint8_t RTP_PAYLOAD_TYPE = 110;
  constexpr int SAMPLE_RATE = 48000;
  constexpr int DEFAULT_CHANNELS = 1;
  constexpr int DEFAULT_PACKET_DURATION = 10;
  constexpr int COMPAT_PACKET_DURATION = 20;
  constexpr int MAX_CHANNELS = 2;
  constexpr auto IDLE_TIMEOUT = std::chrono::milliseconds {500};

  struct config_t {
    bool enabled {};
    int channels {DEFAULT_CHANNELS};
    int sample_rate {SAMPLE_RATE};
    int packet_duration {DEFAULT_PACKET_DURATION};
    bool encrypted {};

    [[nodiscard]] int frame_size() const;
  };

  using packet_queue_t = std::shared_ptr<safe::queue_t<std::vector<std::uint8_t>>>;
  using sink_factory_t = std::function<std::unique_ptr<platf::client_mic_sink_t>()>;

  [[nodiscard]] bool validate_config(const config_t &config);
  void receive(safe::mail_t mail, const config_t &config, packet_queue_t packets, crypto::cipher::gcm_t *cipher, const std::function<bool()> &suspended = {});
  void receive(safe::mail_t mail, const config_t &config, packet_queue_t packets, crypto::cipher::gcm_t *cipher, const sink_factory_t &make_sink, const std::function<std::chrono::steady_clock::time_point()> &now = std::chrono::steady_clock::now, const std::function<bool()> &suspended = {});
}  // namespace client_mic
