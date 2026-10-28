/**
 * @file tests/unit/test_gateway.cpp
 * @brief Test src/gateway.*.
 */

#include "src/config.h"
#include "src/gateway.h"

#include <array>
#include <boost/asio.hpp>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace gateway {
  std::uint16_t read_u16_be(char high, char low);
}

#include "../tests_common.h"

namespace {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  using udp = asio::ip::udp;

  class GatewayHttpsForwardingTests: public testing::Test {
  protected:
    void SetUp() override {
      logging::log_flush();
      std::ifstream log("test_sunshine.log", std::ios::binary | std::ios::ate);
      log_offset = log.tellg();
      original_port = config::sunshine.port;
      original_client_mic = config::audio.client_mic;
      for (int attempt = 0; attempt < 20; ++attempt) {
        listener = tcp::acceptor(io, {asio::ip::address_v4::loopback(), 0});
        auto internal_port = listener.local_endpoint().port();
        if (internal_port < 1124 || internal_port > 65000) {
          continue;
        }
        base_port = internal_port - gateway::INTERNAL_HTTPS_OFFSET;
        try {
          tcp::acceptor tcp_probe(io, {asio::ip::address_v4::loopback(), base_port});
          udp::socket udp_probe(io, {udp::v4(), base_port});
          udp::socket video_probe(io, {asio::ip::address_v4::loopback(), static_cast<unsigned short>(base_port + 214)});
          udp::socket audio_probe(io, {asio::ip::address_v4::loopback(), static_cast<unsigned short>(base_port + 216)});
        } catch (const boost::system::system_error &) {
          continue;
        }
        config::sunshine.port = base_port;
        config::audio.client_mic = false;
        instance = gateway::start();
        return;
      }
      FAIL() << "Could not reserve gateway test ports";
    }

    void TearDown() override {
      finish();
      instance.reset();
      config::sunshine.port = original_port;
      config::audio.client_mic = original_client_mic;
    }

    void finish() {
      boost::system::error_code ec;
      deadline.cancel();
      close_timer.cancel();
      client.close(ec);
      internal.close(ec);
      io.stop();
    }

    bool check(boost::system::error_code ec) {
      if (ec) {
        ADD_FAILURE() << ec.message();
        finish();
        return false;
      }
      return true;
    }

    void run() {
      deadline.expires_after(std::chrono::seconds(10));
      deadline.async_wait([this](auto ec) {
        if (!ec) {
          ADD_FAILURE() << "Gateway exchange timed out";
          finish();
        }
      });
      io.run();
      instance.reset();
    }

    void connect_and_send(const std::vector<std::uint8_t> &wire) {
      client.connect({asio::ip::address_v4::loopback(), base_port});
      asio::write(client, asio::buffer(wire));
    }

    void read_response() {
      asio::async_read(client, asio::buffer(header), [this](auto ec, auto) {
        if (!check(ec)) {
          return;
        }
        const auto length = (static_cast<std::size_t>(header[1]) << 8) | header[2];
        EXPECT_EQ(header[0], gateway::STREAM_HTTPS);
        EXPECT_GT(length, 0);
        if (header[0] != gateway::STREAM_HTTPS || length == 0 || received.size() + length > response.size()) {
          ADD_FAILURE() << "Invalid HTTPS response frame length: " << length;
          finish();
          return;
        }
        frame.resize(length);
        asio::async_read(client, asio::buffer(frame), [this](auto ec, auto) {
          if (!check(ec)) {
            return;
          }
          received.insert(received.end(), frame.begin(), frame.end());
          if (received.size() == response.size()) {
            finish();
          } else {
            read_response();
          }
        });
      });
    }

    void run_partial_frame(const std::vector<std::uint8_t> &wire) {
      listener.async_accept(internal, [this](auto ec) {
        if (!check(ec)) {
          return;
        }
        close_timer.expires_after(std::chrono::milliseconds(100));
        close_timer.async_wait([this](auto ec) {
          if (!ec) {
            boost::system::error_code ignored;
            internal.close(ignored);
          }
        });
      });
      connect_and_send(wire);
      client.async_read_some(asio::buffer(header), [this](auto ec, auto n) {
        EXPECT_TRUE(ec == asio::error::eof || ec == asio::error::connection_reset) << ec.message();
        EXPECT_EQ(n, 0);
        finish();
      });
      run();
    }

    bool contains_log(std::string_view expected) {
      logging::log_flush();
      std::ifstream log("test_sunshine.log", std::ios::binary);
      log.seekg(log_offset);
      for (std::string line; std::getline(log, line);) {
        if (line.find(expected) != std::string::npos) {
          return true;
        }
      }
      return false;
    }

    asio::io_context io;
    tcp::acceptor listener {io};
    tcp::socket client {io};
    tcp::socket internal {io};
    asio::steady_timer deadline {io};
    asio::steady_timer close_timer {io};
    std::unique_ptr<platf::deinit_t> instance;
    std::uint16_t original_port {};
    std::uint16_t base_port {};
    bool original_client_mic {};
    std::streampos log_offset {};
    std::array<std::uint8_t, 3> header {};
    std::vector<std::uint8_t> frame;
    std::vector<std::uint8_t> response;
    std::vector<std::uint8_t> received;
  };
}  // namespace

TEST_F(GatewayHttpsForwardingTests, PreservesMultipleRequestFramesAndLargeResponse) {
  const std::vector<std::uint8_t> request(257, 0xAB);
  std::vector<std::uint8_t> forwarded(request.size());
  response.resize(1024 * 1024);
  for (std::size_t i = 0; i < response.size(); ++i) {
    response[i] = i % 251;
  }
  listener.async_accept(internal, [this, &forwarded](auto ec) {
    if (check(ec)) {
      asio::async_read(internal, asio::buffer(forwarded), [this](auto ec, auto) {
        if (check(ec)) {
          asio::async_write(internal, asio::buffer(response), [this](auto ec, auto) {
            check(ec);
          });
        }
      });
    }
  });
  std::vector<std::uint8_t> wire {gateway::STREAM_HTTPS, 0, 128};
  wire.insert(wire.end(), request.begin(), request.begin() + 128);
  wire.insert(wire.end(), {gateway::STREAM_HTTPS, 0, 129});
  wire.insert(wire.end(), request.begin() + 128, request.end());
  connect_and_send(wire);
  read_response();
  run();
  EXPECT_EQ(forwarded, request);
  EXPECT_EQ(received, response);
  EXPECT_TRUE(contains_log("first_declared=128 first_received=128 c2i_bytes=257 i2c_bytes=1048576"));
}

TEST_F(GatewayHttpsForwardingTests, LogsActualPartialFirstPayloadWhenInternalCloses) {
  std::vector<std::uint8_t> wire(3 + 128, 0xAB);
  wire[0] = gateway::STREAM_HTTPS;
  wire[1] = 0x10;
  wire[2] = 0;
  run_partial_frame(wire);
  EXPECT_TRUE(contains_log("first_declared=4096 first_received=128 c2i_bytes=0 i2c_bytes=0"));
}

TEST_F(GatewayHttpsForwardingTests, LogsUnknownLengthWhenFirstHeaderIsIncomplete) {
  run_partial_frame({gateway::STREAM_HTTPS, 0x10});
  EXPECT_TRUE(contains_log("first_header_bytes=1 first_declared=unknown first_received=0 c2i_bytes=0 i2c_bytes=0"));
}

TEST(GatewayLengthParsingTests, ParsesSignedCharBytesAsUnsigned) {
  const std::array<char, 2> len_173 {static_cast<char>(0x00), static_cast<char>(0xAD)};
  const std::array<char, 2> len_230 {static_cast<char>(0x00), static_cast<char>(0xE6)};
  const std::array<char, 2> len_392 {static_cast<char>(0x01), static_cast<char>(0x88)};
  const std::array<char, 2> len_752 {static_cast<char>(0x02), static_cast<char>(0xF0)};

  EXPECT_EQ(173, gateway::read_u16_be(len_173[0], len_173[1]));
  EXPECT_EQ(230, gateway::read_u16_be(len_230[0], len_230[1]));
  EXPECT_EQ(392, gateway::read_u16_be(len_392[0], len_392[1]));
  EXPECT_EQ(752, gateway::read_u16_be(len_752[0], len_752[1]));
}

TEST(GatewayControlAdmissionTests, AllowsUpToHardLimit) {
  gateway::detail::control_send_admission_t admission {};

  for (std::size_t i = 0; i < gateway::detail::CONTROL_MAX_IN_FLIGHT; ++i) {
    ASSERT_TRUE(admission.try_acquire());
  }
  EXPECT_FALSE(admission.try_acquire());
  EXPECT_EQ(admission.in_flight, gateway::detail::CONTROL_MAX_IN_FLIGHT);
}

TEST(GatewayControlAdmissionTests, ReleaseMakesCapacityAvailable) {
  gateway::detail::control_send_admission_t admission {gateway::detail::CONTROL_MAX_IN_FLIGHT};

  EXPECT_FALSE(admission.try_acquire());
  admission.release();
  EXPECT_TRUE(admission.try_acquire());
  EXPECT_EQ(admission.in_flight, gateway::detail::CONTROL_MAX_IN_FLIGHT);
}

TEST(GatewayRouteTests, RebindingOneChannelDoesNotInvalidateAnother) {
  gateway::detail::udp_route_versions_t versions;
  const auto control = versions.get(gateway::STREAM_CONTROL);
  const auto audio = versions.get(gateway::STREAM_AUDIO);
  versions.change(gateway::STREAM_AUDIO);
  EXPECT_EQ(versions.get(gateway::STREAM_CONTROL), control);
  EXPECT_NE(versions.get(gateway::STREAM_AUDIO), audio);
  versions.change(gateway::STREAM_AUDIO);
  EXPECT_NE(versions.get(gateway::STREAM_AUDIO), audio);
}

TEST(GatewayRouteTests, ClearingRoutesInvalidatesEveryChannel) {
  gateway::detail::udp_route_versions_t versions;
  versions.change(gateway::STREAM_AUDIO);
  const auto audio = versions.get(gateway::STREAM_AUDIO);
  const auto control = versions.get(gateway::STREAM_CONTROL);
  versions.clear();
  EXPECT_NE(versions.get(gateway::STREAM_AUDIO), audio);
  EXPECT_NE(versions.get(gateway::STREAM_CONTROL), control);
}
