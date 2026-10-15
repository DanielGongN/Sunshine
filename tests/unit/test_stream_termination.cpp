/**
 * @file tests/unit/test_stream_termination.cpp
 * @brief Reliable termination must survive packet loss before session cleanup.
 */
#include "../tests_common.h"
#include "src/stream_disconnect.h"
#include "src/stream_termination.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using progress = stream::detail::termination_progress_e;
using termination = stream::detail::control_termination_t;

class StreamTerminationTests: public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_EQ(enet_initialize(), 0);
    initialized = true;
    current = this;
    ENetAddress address {};
    ASSERT_EQ(enet_address_set_host(&address, "127.0.0.1"), 0);
    enet_address_set_port(&address, 0);
    server.reset(enet_host_create(AF_INET, &address, 1, 1, 0, 0));
    client.reset(enet_host_create(AF_INET, nullptr, 1, 1, 0, 0));
    ASSERT_TRUE(server);
    ASSERT_TRUE(client);
    ASSERT_EQ(enet_socket_get_address(server->socket, &address), 0);
    client_peer = enet_host_connect(client.get(), &address, 1, 0);
    ASSERT_NE(client_peer, nullptr);
    ASSERT_TRUE(pump_until([&]() {
      return server_peer && client_peer->state == ENET_PEER_STATE_CONNECTED;
    }));
    client->intercept = [](ENetHost *host, ENetEvent *) {
      const auto first = host->receivedData;
      const auto last = first + host->receivedDataLength;
      if (current->drop_remaining != 0 && std::search(first, last, current->payload.begin(), current->payload.end()) != last) {
        if (current->drop_remaining > 0) {
          --current->drop_remaining;
        }
        ++current->dropped;
        return 1;
      }
      return 0;
    };
  }

  void TearDown() override {
    client.reset();
    server.reset();
    current = nullptr;
    if (initialized) {
      enet_deinitialize();
    }
  }

  void pump(ENetHost *host, bool is_server) {
    ENetEvent event {};
    const auto result = enet_host_service(host, &event, 1);
    ASSERT_GE(result, 0);
    if (result == 0) {
      return;
    }
    if (event.type == ENET_EVENT_TYPE_CONNECT && is_server) {
      server_peer = event.peer;
    } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
      if (!is_server) {
        received.emplace_back(event.packet->data, event.packet->data + event.packet->dataLength);
        received_before_disconnect = !client_disconnected;
      }
      enet_packet_destroy(event.packet);
    } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
      if (is_server) {
        server_disconnect_data = event.data;
        server_peer = nullptr;
      } else {
        client_disconnected = true;
      }
    }
  }

  template<class Predicate>
  bool pump_until(Predicate done) {
    const auto deadline = termination::clock::now() + 3s;
    while (!done() && termination::clock::now() < deadline) {
      pump(server.get(), true);
      pump(client.get(), false);
    }
    return done();
  }

  bool queue_code(std::uint32_t code) {
    payload = {0x09, 0x01, 0x04, 0x00, static_cast<std::uint8_t>(code >> 24), static_cast<std::uint8_t>(code >> 16), static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(code)};
    auto packet = enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE);
    if (!packet) {
      return false;
    }
    if (enet_peer_send(server_peer, 0, packet) != 0) {
      enet_packet_destroy(packet);
      return false;
    }
    return true;
  }

  using host_t = std::unique_ptr<ENetHost, decltype(&enet_host_destroy)>;
  host_t server {nullptr, enet_host_destroy};
  host_t client {nullptr, enet_host_destroy};
  ENetPeer *server_peer {};
  ENetPeer *client_peer {};
  std::array<std::uint8_t, 8> payload {};
  std::vector<std::vector<std::uint8_t>> received;
  std::optional<std::uint32_t> server_disconnect_data;
  bool initialized {};
  bool client_disconnected {};
  bool received_before_disconnect {};
  int drop_remaining {};
  int dropped {};
  inline static thread_local StreamTerminationTests *current {};
};

class ClientDisconnectCodeTests: public StreamTerminationTests, public ::testing::WithParamInterface<std::pair<std::uint32_t, middleware::disconnect_notification_type_e>> {};

TEST_P(ClientDisconnectCodeTests, ClientDataArrivesInHostOrderAndSelectsReason) {
  const auto [code, expected_type] = GetParam();
  enet_peer_disconnect(client_peer, code);
  enet_host_flush(client.get());
  ASSERT_TRUE(pump_until([&]() {
    return server_disconnect_data.has_value();
  }));
  ASSERT_EQ(*server_disconnect_data, code);
  EXPECT_EQ(stream::detail::client_disconnect_reason(*server_disconnect_data).type, expected_type);
}

INSTANTIATE_TEST_SUITE_P(
  EnetClientDisconnectCodes,
  ClientDisconnectCodeTests,
  ::testing::Values(
    std::pair {0x8004F001U, middleware::disconnect_notification_type_e::client_requested_stop},
    std::pair {0x8004F002U, middleware::disconnect_notification_type_e::client_displaced},
    std::pair {0U, middleware::disconnect_notification_type_e::peer_disconnected_unknown}
  )
);

TEST_F(StreamTerminationTests, ImmediateCloseLosesCodeWhenFirstDatagramIsDropped) {
  ASSERT_TRUE(queue_code(stream::detail::CONTROL_TERMINATION_STANDBY_TIMEOUT));
  drop_remaining = 1;
  enet_host_flush(server.get());
  enet_peer_disconnect_now(server_peer, 0);
  ASSERT_TRUE(pump_until([&]() {
    return client_disconnected;
  }));
  EXPECT_EQ(dropped, 1);
  EXPECT_TRUE(received.empty());
}

class StreamTerminationCodeTests: public StreamTerminationTests, public ::testing::WithParamInterface<std::uint32_t> {};

TEST_P(StreamTerminationCodeTests, LostFirstDatagramIsRetransmittedBeforeDisconnect) {
  ASSERT_TRUE(queue_code(GetParam()));
  drop_remaining = 1;
  termination drain;
  const auto start = termination::clock::now();
  ASSERT_EQ(drain.poll(server_peer, start), progress::waiting);
  enet_host_flush(server.get());
  progress result = progress::waiting;
  ASSERT_TRUE(pump_until([&]() {
    result = drain.poll(server_peer, termination::clock::now());
    return result != progress::waiting && client_disconnected;
  }));
  EXPECT_EQ(result, progress::complete);
  EXPECT_EQ(dropped, 1);
  ASSERT_EQ(received.size(), 1);
  EXPECT_EQ(received.front(), (std::vector<std::uint8_t> {payload.begin(), payload.end()}));
  EXPECT_TRUE(received_before_disconnect);
}

INSTANTIATE_TEST_SUITE_P(
  PrivateTerminationCodes,
  StreamTerminationCodeTests,
  ::testing::Values(
    stream::detail::CONTROL_TERMINATION_PLATFORM_DEFAULT_EXIT,
    stream::detail::CONTROL_TERMINATION_FORCE_TIMEOUT,
    stream::detail::CONTROL_TERMINATION_STANDBY_TIMEOUT,
    stream::detail::CONTROL_TERMINATION_GAME_PROCESS_EXIT,
    stream::detail::CONTROL_TERMINATION_NO_AVAILABLE_TIME
  )
);

TEST_F(StreamTerminationTests, SuccessfulForceTimeoutDeliveryClosesBeforeDeadline) {
  ASSERT_TRUE(queue_code(stream::detail::CONTROL_TERMINATION_FORCE_TIMEOUT));
  termination drain;
  const auto start = termination::clock::now();
  ASSERT_EQ(drain.poll(server_peer, start), progress::waiting);
  enet_host_flush(server.get());
  ASSERT_TRUE(pump_until([&]() {
    return drain.poll(server_peer, termination::clock::now()) == progress::complete && client_disconnected;
  }));
  ASSERT_EQ(received.size(), 1);
  EXPECT_EQ(received.front(), (std::vector<std::uint8_t> {payload.begin(), payload.end()}));
  EXPECT_TRUE(received_before_disconnect);
  EXPECT_LT(termination::clock::now() - start, termination::max_wait);
}

TEST_F(StreamTerminationTests, RepeatedPollsCannotExtendBlackholedPeerDeadline) {
  ASSERT_TRUE(queue_code(stream::detail::CONTROL_TERMINATION_STANDBY_TIMEOUT));
  termination drain;
  const termination::clock::time_point start {};
  EXPECT_FALSE(drain.started());
  ASSERT_EQ(drain.poll(server_peer, start), progress::waiting);
  EXPECT_TRUE(drain.started());
  for (int elapsed = 1; elapsed < 1000; ++elapsed) {
    ASSERT_EQ(drain.poll(server_peer, start + std::chrono::milliseconds {elapsed}), progress::waiting);
  }
  EXPECT_EQ(drain.poll(server_peer, start + termination::max_wait), progress::timed_out);
  EXPECT_EQ(drain.poll(server_peer, start + termination::max_wait + 1ms), progress::timed_out);
  EXPECT_EQ(drain.poll(nullptr, start + termination::max_wait), progress::complete);
}

TEST_F(StreamTerminationTests, ThirdConnectionRetainsItsOwnTerminationAndRetransmits) {
  constexpr std::array codes {
    stream::detail::CONTROL_TERMINATION_FORCE_TIMEOUT,
    stream::detail::CONTROL_TERMINATION_STANDBY_TIMEOUT,
    stream::detail::CONTROL_TERMINATION_GAME_PROCESS_EXIT
  };
  for (int attempt = 0; attempt < 3; ++attempt) {
    SCOPED_TRACE(attempt);
    if (attempt != 0) {
      received.clear();
      client_disconnected = false;
      received_before_disconnect = false;
      ENetAddress address {};
      ASSERT_EQ(enet_socket_get_address(server->socket, &address), 0);
      client_peer = enet_host_connect(client.get(), &address, 1, 0);
      ASSERT_NE(client_peer, nullptr);
      ASSERT_TRUE(pump_until([&]() {
        return server_peer && client_peer->state == ENET_PEER_STATE_CONNECTED;
      }));
    }
    ASSERT_TRUE(queue_code(codes[attempt]));
    drop_remaining = attempt == 2 ? 1 : 0;
    termination drain;
    EXPECT_FALSE(drain.started());
    ASSERT_EQ(drain.poll(server_peer, termination::clock::now()), progress::waiting);
    enet_host_flush(server.get());
    ASSERT_TRUE(pump_until([&]() {
      return drain.poll(server_peer, termination::clock::now()) == progress::complete && client_disconnected;
    }));
    ASSERT_EQ(received.size(), 1);
    EXPECT_EQ(received.front(), (std::vector<std::uint8_t> {payload.begin(), payload.end()}));
    EXPECT_TRUE(received_before_disconnect);
  }
  EXPECT_EQ(dropped, 1);
}
