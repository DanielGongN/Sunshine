/**
 * @file src/gateway.h
 * @brief In-process single-port gateway for Sunshine traffic.
 *
 * The gateway listens on the public base port for TCP and UDP. TCP traffic is
 * either passed through directly for TLS ClientHello or framed by stream ID.
 * UDP traffic uses a 3-byte gateway header: [stream_id:1][length:2 BE].
 */
#pragma once

#include "platform/common.h"

#include <cstdint>
#include <memory>
#include <string>

namespace gateway {

  // Gateway protocol stream IDs.
  constexpr std::uint8_t STREAM_HTTPS = 0x01;
  constexpr std::uint8_t STREAM_VIDEO = 0x02;
  constexpr std::uint8_t STREAM_CONTROL = 0x03;
  constexpr std::uint8_t STREAM_AUDIO = 0x04;
  constexpr std::uint8_t STREAM_RTSP = 0x05;

  // First byte of a TLS ClientHello.
  constexpr std::uint8_t TLS_MAGIC = 0x16;

  // Internal port offsets used by the gateway.
  constexpr int INTERNAL_HTTPS_OFFSET = 100;
  constexpr int UDP_FWD_OFFSET = 200;

  /**
   * @brief Start the in-process gateway.
   * @return A deinit guard that stops the gateway on destruction.
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> start();

  /**
   * @brief Get the first external client IP recorded by the gateway.
   * @return The client IP string, or an empty string if no external client is known.
   */
  std::string get_client_ip();

  /**
   * @brief Get the last external UDP port recorded for a gateway stream.
   * @param stream_id Gateway stream ID, such as STREAM_VIDEO or STREAM_AUDIO.
   * @return The client UDP port, or 0 if no port has been recorded.
   */
  std::uint16_t get_client_port(std::uint8_t stream_id);

  /**
   * @brief Clear recorded UDP client endpoints so queued client-bound packets are dropped.
   */
  void clear_udp_clients();

}  // namespace gateway
