/**
 * @file src/stream_control_diagnostics.h
 * @brief Observe ENet disconnect commands without changing transport handling.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <enet/enet.h>
#include <optional>
#include <span>

namespace stream::detail {
  inline std::optional<std::uint16_t> observed_disconnect_header(std::span<const std::uint8_t> packet) {
    if (packet.size() < sizeof(std::uint16_t)) {
      return std::nullopt;
    }
    std::uint16_t raw_header;
    std::memcpy(&raw_header, packet.data(), sizeof(raw_header));
    const auto header = ENET_NET_TO_HOST_16(raw_header);
    if (header & ENET_PROTOCOL_HEADER_FLAG_COMPRESSED) {
      return std::nullopt;
    }
    std::size_t offset = header & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME ? sizeof(ENetProtocolHeader) : sizeof(std::uint16_t);
    constexpr std::array<std::size_t, ENET_PROTOCOL_COMMAND_COUNT> sizes {
      0,
      sizeof(ENetProtocolAcknowledge),
      sizeof(ENetProtocolConnect),
      sizeof(ENetProtocolVerifyConnect),
      sizeof(ENetProtocolDisconnect),
      sizeof(ENetProtocolPing),
      sizeof(ENetProtocolSendReliable),
      sizeof(ENetProtocolSendUnreliable),
      sizeof(ENetProtocolSendFragment),
      sizeof(ENetProtocolSendUnsequenced),
      sizeof(ENetProtocolBandwidthLimit),
      sizeof(ENetProtocolThrottleConfigure),
      sizeof(ENetProtocolSendFragment),
    };
    while (offset + sizeof(ENetProtocolCommandHeader) <= packet.size()) {
      const auto type = packet[offset] & ENET_PROTOCOL_COMMAND_MASK;
      if (type >= sizes.size() || sizes[type] == 0 || sizes[type] > packet.size() - offset) {
        return std::nullopt;
      }
      ENetProtocol command {};
      std::memcpy(&command, packet.data() + offset, sizes[type]);
      offset += sizes[type];
      if (type == ENET_PROTOCOL_COMMAND_DISCONNECT) {
        return header;
      }
      std::size_t payload = 0;
      switch (type) {
        case ENET_PROTOCOL_COMMAND_SEND_RELIABLE:
          payload = ENET_NET_TO_HOST_16(command.sendReliable.dataLength);
          break;
        case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE:
          payload = ENET_NET_TO_HOST_16(command.sendUnreliable.dataLength);
          break;
        case ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
          payload = ENET_NET_TO_HOST_16(command.sendUnsequenced.dataLength);
          break;
        case ENET_PROTOCOL_COMMAND_SEND_FRAGMENT:
        case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT:
          payload = ENET_NET_TO_HOST_16(command.sendFragment.dataLength);
          break;
        default:
          break;
      }
      if (payload > packet.size() - offset) {
        return std::nullopt;
      }
      offset += payload;
    }
    return std::nullopt;
  }

  struct control_peer_snapshot_t {
    std::uint32_t ack_age_ms {};
    std::uint32_t earliest_timeout_age_ms {};
    std::uint32_t timeout_minimum {};
    std::uint32_t timeout_maximum {};
    std::size_t pending_reliable {};
    std::uint16_t max_send_attempts {};
  };

  inline control_peer_snapshot_t snapshot_control_peer(const ENetPeer &peer, std::uint32_t now) {
    control_peer_snapshot_t result {
      peer.lastReceiveTime ? now - peer.lastReceiveTime : 0,
      peer.earliestTimeout ? now - peer.earliestTimeout : 0,
      peer.timeoutMinimum,
      peer.timeoutMaximum,
    };
    for (auto current = peer.sentReliableCommands.sentinel.next; current != &peer.sentReliableCommands.sentinel; current = current->next) {
      const auto command = reinterpret_cast<const ENetOutgoingCommand *>(current);
      ++result.pending_reliable;
      result.max_send_attempts = std::max(result.max_send_attempts, command->sendAttempts);
    }
    return result;
  }
}  // namespace stream::detail
