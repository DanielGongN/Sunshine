/**
 * @file tests/unit/test_client_mic.cpp
 * @brief Test src/client_mic.*.
 */
#include "../tests_common.h"

#include <cstring>
#include <future>
#include <opus/opus.h>
#include <src/client_mic.h>
#include <src/client_mic_device.h>

extern "C" {
#include <moonlight-common-c/src/Video.h>
}

TEST(ClientMicConfigTests, AcceptsAdvertisedDefaults) {
  client_mic::config_t config;

  EXPECT_TRUE(client_mic::validate_config(config));
  EXPECT_EQ(480, config.frame_size());
}

TEST(ClientMicConfigTests, RejectsUnsupportedFormatChanges) {
  client_mic::config_t config;

  config.sample_rate = 44100;
  EXPECT_FALSE(client_mic::validate_config(config));

  config = {};
  config.packet_duration = 5;
  EXPECT_FALSE(client_mic::validate_config(config));

  config = {};
  config.channels = client_mic::MAX_CHANNELS + 1;
  EXPECT_FALSE(client_mic::validate_config(config));
}

TEST(ClientMicConfigTests, AcceptsCompatPacketDuration) {
  client_mic::config_t config;
  config.packet_duration = client_mic::COMPAT_PACKET_DURATION;

  EXPECT_TRUE(client_mic::validate_config(config));
  EXPECT_EQ(960, config.frame_size());
}

TEST(ClientMicDeviceTests, SelectsVirtualCablesInStableIdOrderAndPairsVacLines) {
  const std::vector<client_mic::sink_device_t> outputs {
    {"speaker", "Speakers", "Speakers", "Realtek Audio"},
    {"b", "Line 2 (Virtual Audio Cable)", "Line 2", "Virtual Audio Cable"},
    {"a", "Line 1 (Virtual Audio Cable)", "Line 1", "Virtual Audio Cable"},
  };
  const std::vector<client_mic::sink_device_t> inputs {
    {"mic2", "Line 2 (Virtual Audio Cable)", "Line 2", "Virtual Audio Cable"},
    {"mic1", "Line 1 (Virtual Audio Cable)", "Line 1", "Virtual Audio Cable"},
  };

  const auto pairs = client_mic::find_sink_pairs(outputs, inputs, "");
  ASSERT_EQ(pairs.size(), 2);
  EXPECT_EQ(pairs[0].playback.id, "a");
  EXPECT_EQ(pairs[0].recording.id, "mic1");
  EXPECT_EQ(pairs[1].recording.id, "mic2");
}

TEST(ClientMicDeviceTests, PairsVbCableInputAndOutputDespiteDifferentNames) {
  const auto pairs = client_mic::find_sink_pairs(
    {{"out", "CABLE Input (VB-Audio Virtual Cable)", "CABLE Input", "VB-Audio Virtual Cable"}},
    {{"in", "CABLE Output (VB-Audio Virtual Cable)", "CABLE Output", "VB-Audio Virtual Cable"}},
    ""
  );
  ASSERT_EQ(pairs.size(), 1);
  EXPECT_EQ(pairs.front().recording.id, "in");
}

TEST(ClientMicDeviceTests, RejectsAmbiguousAndMismatchedRecordingEndpoints) {
  const std::vector<client_mic::sink_device_t> outputs {{"out", "Line 1", "Line 1", "Virtual Audio Cable"}};
  EXPECT_TRUE(client_mic::find_sink_pairs(outputs, {{"in", "Line 2", "Line 2", "Virtual Audio Cable"}}, "").empty());
  EXPECT_TRUE(client_mic::find_sink_pairs(outputs, {
                                                     {"a", "Line 1", "Line 1", "Virtual Audio Cable"},
                                                     {"b", "Line 1", "Line 1", "Virtual Audio Cable"},
                                                   },
                                          "")
                .empty());
}

TEST(ClientMicDeviceTests, DoesNotAutomaticallySelectSpeakersOrVoicemeeter) {
  const std::vector<client_mic::sink_device_t> outputs {
    {"speaker", "Speakers", "Speakers", "Realtek"},
    {"mixer", "Voicemeeter Input", "Voicemeeter Input", "VB-Audio Voicemeeter VAIO"},
  };
  EXPECT_TRUE(client_mic::find_sink_pairs(outputs, {}, "").empty());
}

TEST(ClientMicDeviceTests, ExplicitDeviceIdHasPriorityAndNeverFallsBack) {
  const std::vector<client_mic::sink_device_t> outputs {
    {"first", "chosen", "Line 1", "Virtual Audio Cable"},
    {"chosen", "Line 2", "Line 2", "Virtual Audio Cable"},
  };
  const std::vector<client_mic::sink_device_t> inputs {
    {"mic1", "Line 1", "Line 1", "Virtual Audio Cable"},
    {"mic2", "Line 2", "Line 2", "Virtual Audio Cable"},
  };
  const auto pairs = client_mic::find_sink_pairs(outputs, inputs, "chosen");
  ASSERT_EQ(pairs.size(), 1);
  EXPECT_EQ(pairs.front().playback.id, "chosen");
  EXPECT_TRUE(client_mic::find_sink_pairs(outputs, inputs, "missing").empty());
}

TEST(ClientMicGateTests, LastActiveWriterMutesTheSharedRecordingEndpoint) {
  client_mic::sink_gate_t gate;
  std::vector<bool> changes;
  const auto mute = [&](bool value) {
    changes.push_back(value);
    return true;
  };
  EXPECT_TRUE(gate.silence(mute));
  EXPECT_TRUE(gate.acquire(mute));
  EXPECT_TRUE(gate.acquire(mute));
  EXPECT_TRUE(gate.release(mute));
  EXPECT_TRUE(gate.silence(mute));
  EXPECT_EQ(changes, (std::vector<bool> {true, false}));
  EXPECT_TRUE(gate.release(mute));
  EXPECT_EQ(changes, (std::vector<bool> {true, false, true}));
}

TEST(ClientMicGateTests, FailedUnmuteDoesNotAcquireAWriter) {
  client_mic::sink_gate_t gate;
  EXPECT_FALSE(gate.acquire([](bool) {
    return false;
  }));
  bool muted = false;
  EXPECT_TRUE(gate.silence([&](bool value) {
    muted = value;
    return true;
  }));
  EXPECT_TRUE(muted);
}

namespace {
  struct mic_sink_state_t {
    int writes {};
    int silences {};
    bool active {};
    std::function<void()> on_write;
    std::function<void()> on_silence;
  };

  class test_mic_sink_t: public platf::client_mic_sink_t {
  public:
    explicit test_mic_sink_t(mic_sink_state_t &state):
        state {state} {
    }

    platf::capture_e write(const float *, std::size_t frames, const std::function<bool()> &cancelled) override {
      if (!cancelled() && frames) {
        state.active = true;
        ++state.writes;
        if (state.on_write) {
          state.on_write();
        }
      }
      return platf::capture_e::ok;
    }

    void silence() override {
      state.active = false;
      ++state.silences;
      if (state.on_silence) {
        state.on_silence();
      }
    }

  private:
    mic_sink_state_t &state;
  };

  std::vector<std::uint8_t> microphone_packet(std::uint16_t sequence = 0) {
    int error = OPUS_OK;
    auto encoder = opus_encoder_create(client_mic::SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &error);
    EXPECT_EQ(error, OPUS_OK);
    if (!encoder) {
      return {};
    }
    std::vector<float> samples(480, 0.25f);
    std::vector<std::uint8_t> packet(sizeof(RTP_PACKET) + 1275);
    const auto size = opus_encode_float(encoder, samples.data(), 480, packet.data() + sizeof(RTP_PACKET), 1275);
    opus_encoder_destroy(encoder);
    EXPECT_GT(size, 0);
    if (size <= 0) {
      return {};
    }
    RTP_PACKET rtp {};
    rtp.header = 0x80;
    rtp.packetType = client_mic::RTP_PAYLOAD_TYPE;
    rtp.sequenceNumber = util::endian::big(sequence);
    std::memcpy(packet.data(), &rtp, sizeof(rtp));
    packet.resize(sizeof(rtp) + size);
    return packet;
  }

  std::vector<std::uint8_t> encrypted_microphone_packet(crypto::cipher::gcm_t &cipher, std::uint16_t sequence) {
    const auto packet = microphone_packet(sequence);
    RTP_PACKET rtp;
    std::memcpy(&rtp, packet.data(), sizeof(rtp));
    crypto::aes_t iv(12, 0);
    std::memcpy(iv.data(), &rtp.sequenceNumber, sizeof(rtp.sequenceNumber));
    std::memcpy(iv.data() + sizeof(rtp.sequenceNumber), &rtp.timestamp, sizeof(rtp.timestamp));
    iv[10] = 'C';
    iv[11] = 'M';

    std::vector<std::uint8_t> encrypted(packet.size() + crypto::cipher::tag_size);
    std::memcpy(encrypted.data(), &rtp, sizeof(rtp));
    const auto size = cipher.encrypt(
      {reinterpret_cast<const char *>(packet.data() + sizeof(rtp)), packet.size() - sizeof(rtp)},
      encrypted.data() + sizeof(rtp),
      &iv
    );
    EXPECT_GT(size, 0);
    encrypted.resize(sizeof(rtp) + crypto::cipher::tag_size + std::max(0, size));
    return encrypted;
  }

  struct ClientMicReceiveTests: testing::Test {
    safe::mail_t mail = std::make_shared<safe::mail_raw_t>();
    client_mic::packet_queue_t packets = std::make_shared<client_mic::packet_queue_t::element_type>();
    client_mic::config_t config {.enabled = true};
    mic_sink_state_t state;

    client_mic::sink_factory_t factory() {
      return [&]() {
        return std::make_unique<test_mic_sink_t>(state);
      };
    }
  };
}  // namespace

TEST_F(ClientMicReceiveTests, QueueClosureStopsAndSilencesWithoutWritingQueuedAudio) {
  packets->raise(microphone_packet());
  auto make_sink = factory();
  client_mic::receive(mail, config, packets, nullptr, [&]() {
    packets->stop();
    return make_sink();
  });
  EXPECT_EQ(state.writes, 0);
  EXPECT_EQ(state.silences, 1);
}

TEST_F(ClientMicReceiveTests, SessionShutdownAfterQueueWaitDropsTheNextPacket) {
  std::promise<void> ready;
  auto make_sink = factory();
  auto worker = std::async(std::launch::async, [&]() {
    client_mic::receive(mail, config, packets, nullptr, [&]() {
      auto sink = make_sink();
      ready.set_value();
      return sink;
    });
  });
  ready.get_future().wait();
  mail->event<bool>(mail::shutdown)->raise(true);
  packets->raise(microphone_packet());
  ASSERT_EQ(worker.wait_for(std::chrono::seconds {2}), std::future_status::ready);
  worker.get();
  EXPECT_EQ(state.writes, 0);
  EXPECT_EQ(state.silences, 1);
}

TEST_F(ClientMicReceiveTests, IdleMutesAndNewAudioResumesWithResetSequence) {
  std::chrono::steady_clock::time_point time {};
  packets->raise(microphone_packet(100));
  state.on_write = [&]() {
    if (state.writes == 2) {
      packets->stop();
    }
  };
  state.on_silence = [&]() {
    if (state.silences == 1) {
      packets->raise(microphone_packet(0));
    }
  };
  client_mic::receive(mail, config, packets, nullptr, factory(), [&]() {
    time += client_mic::IDLE_TIMEOUT;
    return time;
  });
  EXPECT_EQ(state.writes, 2);
  EXPECT_EQ(state.silences, 2);
  EXPECT_FALSE(state.active);
}

TEST_F(ClientMicReceiveTests, ShutdownDuringWriteDoesNotDrainBufferedPackets) {
  packets->raise(microphone_packet(0));
  packets->raise(microphone_packet(1));
  state.on_write = [&]() {
    mail->event<bool>(mail::shutdown)->raise(true);
  };
  client_mic::receive(mail, config, packets, nullptr, factory());
  EXPECT_EQ(state.writes, 1);
  EXPECT_FALSE(state.active);
}

TEST_F(ClientMicReceiveTests, RecoveryDropsAudioAndResumesWithAResetSequence) {
  bool suspended = false;
  packets->raise(microphone_packet(100));
  packets->raise(microphone_packet(101));
  packets->raise(microphone_packet(102));
  state.on_write = [&]() {
    if (state.writes == 1) {
      suspended = true;
    } else {
      packets->stop();
    }
  };
  state.on_silence = [&]() {
    if (state.silences == 1) {
      suspended = false;
      packets->raise(microphone_packet(0));
    }
  };
  client_mic::receive(mail, config, packets, nullptr, factory(), std::chrono::steady_clock::now, [&]() {
    return suspended;
  });
  EXPECT_EQ(state.writes, 2);
  EXPECT_EQ(state.silences, 2);
  EXPECT_FALSE(state.active);
}

TEST_F(ClientMicReceiveTests, InvalidEncryptedPacketNeverOpensTheGate) {
  config.encrypted = true;
  crypto::aes_t key(16, 0);
  crypto::cipher::gcm_t cipher {key, false};
  packets->raise(microphone_packet());
  bool rejected = false;
  client_mic::receive(mail, config, packets, &cipher, factory(), [&]() {
    if (rejected) {
      packets->stop();
    }
    rejected = true;
    return std::chrono::steady_clock::time_point {};
  });
  EXPECT_EQ(state.writes, 0);
  EXPECT_FALSE(state.active);
}

TEST_F(ClientMicReceiveTests, InvalidEncryptionTagCannotTriggerLossConcealment) {
  config.encrypted = true;
  crypto::aes_t key(16, 0);
  crypto::cipher::gcm_t cipher {key, false};
  packets->raise(encrypted_microphone_packet(cipher, 0));
  auto invalid = encrypted_microphone_packet(cipher, 2);
  invalid[sizeof(RTP_PACKET)] ^= 0xFF;
  packets->raise(std::move(invalid));
  int clock_reads = 0;
  client_mic::receive(mail, config, packets, &cipher, factory(), [&]() {
    if (++clock_reads == 4) {
      packets->stop();
    }
    return std::chrono::steady_clock::time_point {};
  });
  EXPECT_EQ(state.writes, 1);
  EXPECT_FALSE(state.active);
}
