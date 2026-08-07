/**
 * @file src/client_mic.cpp
 * @brief Definitions for client microphone uplink handling.
 */
// standard includes
#include <algorithm>
#include <cstring>
#include <optional>
#include <string_view>

// lib includes
#include <opus/opus.h>

extern "C" {
#include <moonlight-common-c/src/Video.h>
}

// local includes
#include "client_mic.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#include "utility.h"

namespace client_mic {
  using namespace std::literals;
  using opus_decoder_t = util::safe_ptr<OpusDecoder, opus_decoder_destroy>;

  namespace {
    constexpr auto MAX_PLC_FRAMES = 2;
    constexpr auto QUEUE_WAIT = 10ms;

    std::int16_t sequence_delta(std::uint16_t lhs, std::uint16_t rhs) {
      return static_cast<std::int16_t>(lhs - rhs);
    }

    void make_mic_iv(const RTP_PACKET &rtp, crypto::aes_t &iv) {
      iv.assign(12, 0);
      std::memcpy(iv.data(), &rtp.sequenceNumber, sizeof(rtp.sequenceNumber));
      std::memcpy(iv.data() + sizeof(rtp.sequenceNumber), &rtp.timestamp, sizeof(rtp.timestamp));
      iv[10] = 'C';
      iv[11] = 'M';
    }

    bool decode_and_write(OpusDecoder *decoder, platf::client_mic_sink_t &sink, const std::string_view &payload, const config_t &config, std::vector<float> &pcm) {
      auto decoded_frames = opus_decode_float(
        decoder,
        reinterpret_cast<const unsigned char *>(payload.data()),
        static_cast<opus_int32>(payload.size()),
        pcm.data(),
        config.frame_size(),
        0
      );

      if (decoded_frames < 0) {
        BOOST_LOG(warning) << "Client microphone Opus decode failed: "sv << opus_strerror(decoded_frames);
        return true;
      }

      auto status = sink.write(pcm.data(), static_cast<std::size_t>(decoded_frames));
      switch (status) {
        case platf::capture_e::ok:
        case platf::capture_e::timeout:
          return true;
        case platf::capture_e::reinit:
          return false;
        default:
          BOOST_LOG(error) << "Client microphone sink write failed"sv;
          return false;
      }
    }

    bool write_plc(OpusDecoder *decoder, platf::client_mic_sink_t &sink, const config_t &config, std::vector<float> &pcm) {
      auto decoded_frames = opus_decode_float(decoder, nullptr, 0, pcm.data(), config.frame_size(), 0);
      if (decoded_frames < 0) {
        BOOST_LOG(warning) << "Client microphone Opus PLC failed: "sv << opus_strerror(decoded_frames);
        return true;
      }

      auto status = sink.write(pcm.data(), static_cast<std::size_t>(decoded_frames));
      return status == platf::capture_e::ok || status == platf::capture_e::timeout;
    }

    bool supported_packet_duration(int packet_duration) {
      return packet_duration == DEFAULT_PACKET_DURATION || packet_duration == COMPAT_PACKET_DURATION;
    }
  }  // namespace

  int config_t::frame_size() const {
    return packet_duration * sample_rate / 1000;
  }

  bool validate_config(const config_t &config) {
    return config.channels >= 1 &&
           config.channels <= MAX_CHANNELS &&
           config.sample_rate == SAMPLE_RATE &&
           supported_packet_duration(config.packet_duration);
  }

  void receive(safe::mail_t mail, const config_t &config, packet_queue_t packets, crypto::cipher::gcm_t *cipher) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);

    if (!config.enabled) {
      shutdown_event->view();
      return;
    }

    if (!validate_config(config)) {
      BOOST_LOG(error) << "Invalid client microphone configuration"sv;
      return;
    }

    if (config.encrypted && !cipher) {
      BOOST_LOG(error) << "Client microphone encryption was negotiated without a cipher"sv;
      return;
    }

    opus_decoder_t decoder {opus_decoder_create(config.sample_rate, config.channels, nullptr)};
    if (!decoder) {
      BOOST_LOG(error) << "Couldn't initialize client microphone Opus decoder"sv;
      return;
    }

    auto sink = platf::client_mic_sink(config::audio.client_mic_sink, config.sample_rate, static_cast<std::uint32_t>(config.frame_size()), static_cast<std::uint32_t>(config.channels));
    if (!sink) {
      BOOST_LOG(error) << "Unable to initialize client microphone sink"sv;
      shutdown_event->view();
      return;
    }

    BOOST_LOG(info) << "Client microphone initialized: "sv << config.sample_rate / 1000 << " kHz, "sv
                    << config.channels << " channel(s), "sv
                    << config.packet_duration << " ms Opus frames"sv
                    << (config.encrypted ? ", encrypted"sv : ""sv);

    std::vector<float> pcm(static_cast<std::size_t>(config.frame_size() * config.channels));
    std::vector<std::uint8_t> plaintext;
    std::optional<std::uint16_t> expected_sequence;
    crypto::aes_t iv;

    while (!shutdown_event->peek()) {
      auto packet = packets->pop(QUEUE_WAIT);
      if (!packet) {
        continue;
      }

      if (packet->size() <= sizeof(RTP_PACKET)) {
        BOOST_LOG(debug) << "Dropping short client microphone packet: "sv << packet->size();
        continue;
      }

      RTP_PACKET rtp;
      std::memcpy(&rtp, packet->data(), sizeof(rtp));
      if ((rtp.header & 0xC0) != 0x80 || (rtp.header & 0x3F) != 0) {
        BOOST_LOG(debug) << "Dropping client microphone packet with unsupported RTP header 0x"sv << util::hex(rtp.header).to_string_view();
        continue;
      }

      if ((rtp.packetType & 0x7F) != RTP_PAYLOAD_TYPE) {
        BOOST_LOG(debug) << "Dropping client microphone packet with unexpected RTP payload type "sv << static_cast<int>(rtp.packetType & 0x7F);
        continue;
      }

      const auto sequence = util::endian::big(rtp.sequenceNumber);
      if (expected_sequence) {
        const auto delta = sequence_delta(sequence, *expected_sequence);
        if (delta < 0) {
          BOOST_LOG(debug) << "Dropping late client microphone packet seq="sv << sequence;
          continue;
        }

        if (delta > 0) {
          if (delta <= MAX_PLC_FRAMES) {
            BOOST_LOG(debug) << "Client microphone lost "sv << delta << " packet(s), applying Opus PLC"sv;
            for (auto i = 0; i < delta && !shutdown_event->peek(); ++i) {
              if (!write_plc(decoder.get(), *sink, config, pcm)) {
                break;
              }
            }
          } else {
            BOOST_LOG(warning) << "Client microphone sequence gap too large, resyncing: expected="sv << *expected_sequence << " got="sv << sequence;
          }
        }
      }

      std::string_view payload {
        reinterpret_cast<const char *>(packet->data() + sizeof(RTP_PACKET)),
        packet->size() - sizeof(RTP_PACKET)
      };

      if (config.encrypted) {
        if (payload.size() <= crypto::cipher::tag_size) {
          BOOST_LOG(debug) << "Dropping encrypted client microphone packet without a complete GCM tag seq="sv << sequence;
          continue;
        }

        plaintext.clear();
        make_mic_iv(rtp, iv);
        if (cipher->decrypt(payload, plaintext, &iv)) {
          BOOST_LOG(warning) << "Dropping client microphone packet with invalid encryption tag seq="sv << sequence;
          continue;
        }

        payload = std::string_view {
          reinterpret_cast<const char *>(plaintext.data()),
          plaintext.size()
        };
      }

      if (!decode_and_write(decoder.get(), *sink, payload, config, pcm)) {
        BOOST_LOG(info) << "Reinitializing client microphone sink"sv;
        sink = platf::client_mic_sink(config::audio.client_mic_sink, config.sample_rate, static_cast<std::uint32_t>(config.frame_size()), static_cast<std::uint32_t>(config.channels));
        if (!sink) {
          BOOST_LOG(error) << "Couldn't reinitialize client microphone sink"sv;
          return;
        }
      }

      expected_sequence = static_cast<std::uint16_t>(sequence + 1);
    }
  }
}  // namespace client_mic
