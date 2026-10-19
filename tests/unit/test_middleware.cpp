/**
 * @file tests/unit/test_middleware.cpp
 * @brief Test src/middleware.cpp helpers.
 */

#include "src/config.h"
#include "src/middleware.h"
#include "src/stream_disconnect.h"
#include "src/thread_safe.h"

#include <algorithm>
#include <barrier>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace middleware {
  extern boost::asio::io_context *g_ioc;
  extern boost::beast::websocket::stream<boost::asio::ip::tcp::socket> *g_ws;
  extern safe::queue_t<std::string> g_outgoing_queue;
  void clear_pending_connected_payload();
  std::optional<std::int64_t> pending_connected_message_id();
}  // namespace middleware

namespace middleware::detail {
  nlohmann::json make_heartbeat_payload(std::int64_t message_id);
  nlohmann::json make_connected_payload(std::int64_t message_id);
  nlohmann::json make_disconnected_payload(std::string_view message, disconnect_notification_type_e type);
  bool is_connected_ack_success(const nlohmann::json &msg, std::int64_t pending_message_id);
  void reset_disconnected_notification();
  bool handle_upstream_disconnected(const nlohmann::json &msg);
}  // namespace middleware::detail

#include "../tests_common.h"

class MiddlewareHeartbeatTests: public ::testing::Test {
protected:
  void SetUp() override {
    middleware::notify_client_rtt(std::nullopt);
  }

  void TearDown() override {
    disconnect();
    middleware::notify_client_rtt(std::nullopt);
  }

  void connect() {
    middleware::notify_client_state(true);
    connected = true;
  }

  void disconnect() {
    if (connected) {
      middleware::notify_client_state(false);
      connected = false;
    }
  }

  bool connected {};
};

TEST_F(MiddlewareHeartbeatTests, DisconnectedClientHasNullRtt) {
  middleware::notify_client_rtt(23);
  const auto msg = middleware::detail::make_heartbeat_payload(1783665405);

  EXPECT_EQ(msg.at("event"), "client_heartbeat");
  EXPECT_EQ(msg.at("message_id"), 1783665405);
  EXPECT_EQ(msg.at("data").at("type"), 1);
  EXPECT_EQ(msg.at("data").at("net_connected"), false);
  EXPECT_TRUE(msg.at("data").at("rtt_delay").is_null());
}

TEST_F(MiddlewareHeartbeatTests, ConnectedClientWithoutMeasurementHasNullRtt) {
  connect();
  const auto msg = middleware::detail::make_heartbeat_payload(1);

  EXPECT_EQ(msg.at("data").at("net_connected"), true);
  EXPECT_TRUE(msg.at("data").at("rtt_delay").is_null());
}

TEST_F(MiddlewareHeartbeatTests, ReportsLatestRttInMilliseconds) {
  connect();
  middleware::notify_client_rtt(23);
  EXPECT_EQ(middleware::detail::make_heartbeat_payload(1).at("data").at("rtt_delay"), 23);

  middleware::notify_client_rtt(47);
  EXPECT_EQ(middleware::detail::make_heartbeat_payload(2).at("data").at("rtt_delay"), 47);

  middleware::notify_client_rtt(std::nullopt);
  const auto msg = middleware::detail::make_heartbeat_payload(3);
  EXPECT_EQ(msg.at("data").at("net_connected"), true);
  EXPECT_TRUE(msg.at("data").at("rtt_delay").is_null());
}

TEST_F(MiddlewareHeartbeatTests, DisconnectAndReconnectDoNotReusePreviousRtt) {
  connect();
  middleware::notify_client_rtt(23);
  disconnect();
  EXPECT_TRUE(middleware::detail::make_heartbeat_payload(1).at("data").at("rtt_delay").is_null());

  connect();
  EXPECT_TRUE(middleware::detail::make_heartbeat_payload(2).at("data").at("rtt_delay").is_null());

  middleware::notify_client_rtt(71);
  EXPECT_EQ(middleware::detail::make_heartbeat_payload(3).at("data").at("rtt_delay"), 71);
}

TEST_F(MiddlewareHeartbeatTests, FirstConnectionDiscardsUnconnectedMeasurement) {
  middleware::notify_client_rtt(23);
  connect();
  EXPECT_TRUE(middleware::detail::make_heartbeat_payload(1).at("data").at("rtt_delay").is_null());
}

TEST(MiddlewareConnectedAckTests, ConnectedPayloadUsesNestedData) {
  auto msg = middleware::detail::make_connected_payload(1783665405);

  ASSERT_EQ(msg.value("event", ""), "connected");
  ASSERT_EQ(msg.value("message_id", std::int64_t {0}), 1783665405);
  ASSERT_FALSE(msg.contains("type"));
  ASSERT_TRUE(msg.contains("data"));
  ASSERT_TRUE(msg["data"].is_object());
  ASSERT_EQ(msg["data"].value("type", 0), 1);
}

TEST(MiddlewareConnectedAckTests, MatchingSuccessAckIsAccepted) {
  auto ack = nlohmann::json {
    {"event", "consume_record_create_success"},
    {"message_id", 1783665405},
    {"data",
     {{"code", 0},
      {"message", "success"},
      {"result", {{"time", "2026-07-10T14:36:44.52+08:00"}}}}}
  };

  ASSERT_TRUE(middleware::detail::is_connected_ack_success(ack, 1783665405));
}

TEST(MiddlewareConnectedAckTests, MismatchedMessageIdIsRejected) {
  auto ack = nlohmann::json {
    {"event", "consume_record_create_success"},
    {"message_id", 1783665405},
    {"data", {{"code", 0}}}
  };

  ASSERT_FALSE(middleware::detail::is_connected_ack_success(ack, 1783665406));
}

TEST(MiddlewareConnectedAckTests, NonZeroCodeIsRejected) {
  auto ack = nlohmann::json {
    {"event", "consume_record_create_success"},
    {"message_id", 1783665405},
    {"data", {{"code", 1}}}
  };

  ASSERT_FALSE(middleware::detail::is_connected_ack_success(ack, 1783665405));
}

TEST(MiddlewareConnectedAckTests, MissingCodeIsRejected) {
  auto ack = nlohmann::json {
    {"event", "consume_record_create_success"},
    {"message_id", 1783665405},
    {"data", {{"message", "success"}}}
  };

  ASSERT_FALSE(middleware::detail::is_connected_ack_success(ack, 1783665405));
}

class MiddlewareDeliveryTests: public ::testing::Test {
protected:
  void SetUp() override {
    previous_address = std::exchange(config::sunshine.middleware.address, "127.0.0.1");
    previous_port = std::exchange(config::sunshine.middleware.port, acceptor.local_endpoint().port());
    // Engine metadata only reads the certificate; it does not parse it.
    previous_cert = std::exchange(config::nvhttp.cert, SUNSHINE_SOURCE_DIR "/tests/fixtures/http/hello.txt");
    auto ready = mail::man->event<bool>(mail::nvhttp_ready);
    previously_ready = ready->peek();
    ready->raise(true);
    middleware::g_outgoing_queue.discard();
    acceptor.async_accept(ws.next_layer(), [this](boost::system::error_code ec) {
      ASSERT_FALSE(ec) << ec.message();
      ws.async_accept([this](boost::system::error_code ec) {
        ASSERT_FALSE(ec) << ec.message();
        read_next();
      });
    });
    client = middleware::start();
    ASSERT_TRUE(wait_for([this]() {
      return !messages.empty() && messages.back().value("event", "") == "send_to_upstream" &&
             messages.back().at("data").value("event", "") == "stream_engine_info";
    }));
    messages.clear();
  }

  void TearDown() override {
    if (ws.is_open()) {
      ws.async_close(boost::beast::websocket::close_code::normal, [this](boost::system::error_code) {
        closed = true;
      });
      EXPECT_TRUE(wait_for([this]() {
        return closed;
      }));
    }
    client.reset();
    boost::system::error_code ec;
    ws.next_layer().close(ec);
    acceptor.close(ec);
    ioc.restart();
    ioc.poll();
    middleware::g_outgoing_queue.discard();
    config::sunshine.middleware.address = std::move(previous_address);
    config::sunshine.middleware.port = previous_port;
    config::nvhttp.cert = std::move(previous_cert);
    if (!previously_ready) {
      mail::man->event<bool>(mail::nvhttp_ready)->reset();
    }
  }

  void read_next() {
    ws.async_read(buffer, [this](boost::system::error_code ec, std::size_t) {
      if (ec) {
        return;
      }
      messages.push_back(nlohmann::json::parse(boost::beast::buffers_to_string(buffer.data())));
      buffer.consume(buffer.size());
      read_next();
    });
  }

  template<class Predicate>
  bool wait_for(Predicate done, std::chrono::steady_clock::duration timeout = std::chrono::seconds {2}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      ioc.restart();
      ioc.run_for(std::chrono::milliseconds {5});
    }
    return done();
  }

  boost::asio::io_context ioc;
  boost::asio::ip::tcp::acceptor acceptor {ioc, {boost::asio::ip::make_address("127.0.0.1"), 0}};
  boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws {ioc};
  boost::beast::flat_buffer buffer;
  std::unique_ptr<platf::deinit_t> client;
  std::vector<nlohmann::json> messages;
  std::string previous_address;
  std::string previous_cert;
  int previous_port {};
  bool previously_ready {};
  bool closed {};
};

TEST_F(MiddlewareDeliveryTests, ConnectedIsSentWhileUpstreamIsSilent) {
  middleware::notify_client_connected();

  ASSERT_TRUE(wait_for([this]() {
    return !messages.empty();
  }))
    << "connected waited for upstream traffic or the heartbeat";
  ASSERT_EQ(messages.front().value("event", ""), "send_to_upstream");
  const auto &connected = messages.front().at("data");
  EXPECT_EQ(connected.value("event", ""), "connected");
  EXPECT_EQ(connected.at("data").value("type", 0), 1);
  EXPECT_EQ(connected.at("message_id").get<std::int64_t>(), middleware::pending_connected_message_id());
}

TEST_F(MiddlewareDeliveryTests, HeartbeatReportsRttWithoutUpstreamEnvelope) {
  middleware::notify_client_state(true);
  auto state_guard = util::fail_guard([]() {
    middleware::notify_client_state(false);
  });
  middleware::notify_client_rtt(23);

  ASSERT_TRUE(wait_for([this]() {
    return std::any_of(messages.begin(), messages.end(), [](const auto &message) {
      return message.value("event", "") == "client_heartbeat";
    });
  },
                       std::chrono::seconds {32}));

  const auto heartbeat = std::find_if(messages.begin(), messages.end(), [](const auto &message) {
    return message.value("event", "") == "client_heartbeat";
  });
  ASSERT_NE(heartbeat, messages.end());
  EXPECT_TRUE(heartbeat->at("message_id").is_number_integer());
  EXPECT_EQ(heartbeat->at("data").at("type"), 1);
  EXPECT_EQ(heartbeat->at("data").at("net_connected"), true);
  EXPECT_EQ(heartbeat->at("data").at("rtt_delay"), 23);
}

TEST_F(MiddlewareDeliveryTests, QueuedMessagesKeepTheirOrderWhileUpstreamIsSilent) {
  constexpr int count = 16;
  for (int index = 0; index < count; ++index) {
    middleware::send_to_upstream({{"event", "delivery_test"}, {"index", index}, {"payload", std::string(8192, 'x')}});
  }

  ASSERT_TRUE(wait_for([this]() {
    return messages.size() == count;
  }));
  for (int index = 0; index < count; ++index) {
    const auto &message = messages[index].at("data");
    EXPECT_EQ(message.value("event", ""), "delivery_test");
    EXPECT_EQ(message.value("index", -1), index);
    EXPECT_EQ(message.at("payload"), std::string(8192, 'x'));
  }
}

TEST_F(MiddlewareDeliveryTests, ConnectedRetriesAfterThirtySecondsUntilAcknowledged) {
  const auto started = std::chrono::steady_clock::now();
  middleware::notify_client_connected();
  ASSERT_TRUE(wait_for([this]() {
    return !messages.empty();
  }));
  const auto original = messages.front().at("data");
  auto connected_count = [this]() {
    return std::count_if(messages.begin(), messages.end(), [](const auto &message) {
      return message.value("event", "") == "send_to_upstream" && message.at("data").value("event", "") == "connected";
    });
  };

  ASSERT_TRUE(wait_for([&connected_count]() {
    return connected_count() == 2;
  },
                       std::chrono::seconds {32}));
  EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::seconds {29});
  const auto retried = std::find_if(messages.rbegin(), messages.rend(), [](const auto &message) {
    return message.value("event", "") == "send_to_upstream" && message.at("data").value("event", "") == "connected";
  });
  ASSERT_NE(retried, messages.rend());
  EXPECT_EQ(retried->at("data"), original);

  const auto ack = std::make_shared<std::string>(nlohmann::json {
    {"event", "consume_record_create_success"},
    {"message_id", original.at("message_id")},
    {"data", {{"code", 0}}}
  }.dump());
  ws.async_write(boost::asio::buffer(*ack), [ack](boost::system::error_code ec, std::size_t) {
    EXPECT_FALSE(ec) << ec.message();
  });
  ASSERT_TRUE(wait_for([]() {
    return !middleware::pending_connected_message_id();
  }));
}

TEST(MiddlewareDisconnectTests, UnknownReasonUsesAbnormalTypeInData) {
  auto msg = middleware::detail::make_disconnected_payload(
    "network disconnected",
    middleware::disconnect_notification_type_e::other
  );

  ASSERT_EQ(msg.value("event", ""), "disconnected");
  ASSERT_EQ(msg.value("message", ""), "network disconnected");
  ASSERT_EQ(msg["data"].value("message", ""), "network disconnected");
  ASSERT_FALSE(msg.contains("type"));
  ASSERT_EQ(msg["data"].value("type", -1), 99);
}

TEST(MiddlewareDisconnectTests, ForceTimeoutUsesTypeOne) {
  auto msg = middleware::detail::make_disconnected_payload(
    "force timeout",
    middleware::disconnect_notification_type_e::force_timeout
  );

  ASSERT_EQ(msg["data"].value("type", -1), 1);
  ASSERT_EQ(msg["data"].value("message", ""), "force timeout");
}

TEST(MiddlewareDisconnectTests, StandbyTimeoutUsesTypeTwo) {
  auto msg = middleware::detail::make_disconnected_payload(
    "standby timeout",
    middleware::disconnect_notification_type_e::standby_timeout
  );

  ASSERT_EQ(msg["data"].value("type", -1), 2);
  ASSERT_EQ(msg["data"].value("message", ""), "standby timeout");
}

TEST(MiddlewareDisconnectTests, GroupedReasonsPreserveDetailedMessage) {
  using type_e = middleware::disconnect_notification_type_e;
  const std::vector<std::pair<type_e, int>> types {
    {type_e::client_requested_stop, 5},
    {type_e::client_displaced, 6},
    {type_e::client_reported_error, 99},
    {type_e::peer_disconnected_unknown, 99},
    {type_e::control_timeout, 99},
    {type_e::stream_start_timeout, 99},
    {type_e::upstream_requested_stop, 99},
    {type_e::server_requested_stop, 99},
    {type_e::server_shutdown, 99},
    {type_e::server_capture_error, 99},
    {type_e::server_encoder_error, 99},
    {type_e::server_internal_error, 99},
    {type_e::gateway_error, 99},
    {type_e::server_process_abnormal_exit, 99},
    {type_e::control_protocol_error, 99},
  };
  const std::string reason = "编码器初始化失败：NVENC error=8\nframe=123";

  for (const auto &[type, value] : types) {
    SCOPED_TRACE(value);
    auto msg = middleware::detail::make_disconnected_payload(reason, type);
    auto serialized = nlohmann::json::parse(msg.dump());
    EXPECT_EQ(serialized["data"]["type"], value);
    EXPECT_EQ(serialized["data"]["message"], reason);
    EXPECT_EQ(serialized["message"], reason);
  }
}

TEST(MiddlewareDisconnectTests, EmptyReasonStillHasMessageField) {
  auto msg = middleware::detail::make_disconnected_payload("", middleware::disconnect_notification_type_e::other);
  ASSERT_TRUE(msg["data"].contains("message"));
  EXPECT_EQ(msg["data"]["message"], "");
}

class MiddlewareDisconnectCycleTests: public ::testing::Test {
protected:
  void SetUp() override {
    previous_ioc = std::exchange(middleware::g_ioc, &ioc);
    previous_ws = std::exchange(middleware::g_ws, &ws);
    middleware::g_outgoing_queue.discard();
    middleware::detail::reset_disconnected_notification();
    disconnect_requests = mail::man->queue<stream::detail::upstream_disconnect_t>(mail::force_disconnect);
    disconnect_requests->discard();
  }

  void TearDown() override {
    middleware::g_ioc = previous_ioc;
    middleware::g_ws = previous_ws;
    middleware::g_outgoing_queue.discard();
    middleware::clear_pending_connected_payload();
    disconnect_requests->discard();
    middleware::detail::reset_disconnected_notification();
  }

  nlohmann::json pop_notification() {
    auto payload = middleware::g_outgoing_queue.pop(std::chrono::milliseconds {0});
    if (!payload) {
      ADD_FAILURE() << "Expected an outgoing middleware notification";
      return nlohmann::json::object();
    }
    return nlohmann::json::parse(*payload).at("data");
  }

  // Do not run the io_context: inspect the real outgoing queue without network I/O.
  boost::asio::io_context ioc;
  boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws {ioc};
  boost::asio::io_context *previous_ioc {};
  boost::beast::websocket::stream<boost::asio::ip::tcp::socket> *previous_ws {};
  safe::mail_raw_t::queue_t<stream::detail::upstream_disconnect_t> disconnect_requests;
};

TEST_F(MiddlewareDisconnectCycleTests, FirstReasonSuppressesAllLaterDisconnectTypes) {
  middleware::notify_client_disconnected("Idle reminder", middleware::disconnect_notification_type_e::standby_timeout);
  middleware::notify_client_disconnected("Encoder failed", middleware::disconnect_notification_type_e::server_encoder_error);
  middleware::notify_client_disconnected("Stream stopped", middleware::disconnect_notification_type_e::client_requested_stop);

  auto msg = pop_notification();
  EXPECT_EQ(msg.value("event", ""), "disconnected");
  EXPECT_EQ(msg.at("data").value("type", -1), 2);
  EXPECT_EQ(msg.at("data").value("message", ""), "Idle reminder");
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());
}

TEST_F(MiddlewareDisconnectCycleTests, ClientDisconnectCodesQueueDistinctTypesOnce) {
  for (const auto &[code, expected_type] : std::vector<std::pair<std::uint32_t, int>> {{0x8004F001U, 5}, {0x8004F002U, 6}}) {
    SCOPED_TRACE(code);
    middleware::detail::reset_disconnected_notification();
    const auto reason = stream::detail::client_disconnect_reason(code);
    middleware::notify_client_disconnected(reason.message, reason.type);
    middleware::notify_client_disconnected("Later cleanup", middleware::disconnect_notification_type_e::server_shutdown);

    const auto msg = pop_notification();
    EXPECT_EQ(msg.value("event", ""), "disconnected");
    EXPECT_EQ(msg.at("data").value("type", -1), expected_type);
    EXPECT_EQ(msg.at("data").value("message", ""), reason.message);
    EXPECT_EQ(msg.value("message", ""), reason.message);
    EXPECT_FALSE(middleware::g_outgoing_queue.peek());
  }
}

TEST_F(MiddlewareDisconnectCycleTests, ClientConnectResetStartsOneNewNotificationCycle) {
  for (int cycle = 0; cycle < 3; ++cycle) {
    middleware::detail::reset_disconnected_notification();
    middleware::notify_client_disconnected("First disconnect");
    middleware::notify_client_disconnected("Later disconnect");

    EXPECT_EQ(pop_notification().value("message", ""), "First disconnect");
    EXPECT_FALSE(middleware::g_outgoing_queue.peek());
  }
}

TEST_F(MiddlewareDisconnectCycleTests, ConcurrentDisconnectsQueueExactlyOneNotification) {
  constexpr int thread_count = 8;
  std::barrier ready {thread_count};
  std::vector<std::thread> threads;
  for (int i = 0; i < thread_count; ++i) {
    threads.emplace_back([&]() {
      ready.arrive_and_wait();
      middleware::notify_client_disconnected("Concurrent stream stop");
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  EXPECT_EQ(pop_notification().value("event", ""), "disconnected");
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());
}

TEST_F(MiddlewareDisconnectCycleTests, StringAndUtf8OverloadsShareTheSameCycle) {
  middleware::notify_client_disconnected(u8"长时间挂机", middleware::disconnect_notification_type_e::force_timeout);
  middleware::notify_client_disconnected("Stream stopped");
  EXPECT_EQ(pop_notification().value("message", ""), "长时间挂机");
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());

  middleware::detail::reset_disconnected_notification();
  middleware::notify_client_disconnected("Stream stopped");
  middleware::notify_client_disconnected(u8"长时间挂机", middleware::disconnect_notification_type_e::force_timeout);
  EXPECT_EQ(pop_notification().value("message", ""), "Stream stopped");
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());
}

TEST_F(MiddlewareDisconnectCycleTests, OutgoingConnectedDoesNotResetSuppression) {
  middleware::notify_client_disconnected("Stream stopped");
  EXPECT_EQ(pop_notification().value("event", ""), "disconnected");

  middleware::notify_client_connected();
  EXPECT_EQ(pop_notification().value("event", ""), "connected");
  middleware::notify_client_disconnected("Later stream stopped");
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());
}

TEST_F(MiddlewareDisconnectCycleTests, MiddlewareAvailabilityDoesNotResetSuppression) {
  middleware::g_ioc = nullptr;
  middleware::g_ws = nullptr;
  middleware::notify_client_disconnected("Stream stopped while middleware was unavailable");

  middleware::g_ioc = &ioc;
  middleware::g_ws = &ws;
  middleware::notify_client_disconnected("Later stream stopped");
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());

  middleware::detail::reset_disconnected_notification();
  middleware::notify_client_disconnected("New client cycle stopped");
  EXPECT_EQ(pop_notification().value("message", ""), "New client cycle stopped");
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());
}

TEST_F(MiddlewareDisconnectCycleTests, GameProcessExitQueuesDistinctClientCodeWithoutEcho) {
  auto msg = nlohmann::json {
    {"event", "disconnected"},
    {"data", {{"type", 3}, {"message", "游戏进程退出，断开连接"}}}
  };
  ASSERT_TRUE(middleware::detail::handle_upstream_disconnected(msg));

  auto request = disconnect_requests->pop(std::chrono::milliseconds {0});
  ASSERT_TRUE(request);
  EXPECT_EQ(request->termination_code, 0x8004E003U);
  EXPECT_NE(request->termination_code, stream::detail::CONTROL_TERMINATION_GRACEFUL);
  EXPECT_EQ(request->message, "游戏进程退出，断开连接");
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());

  middleware::notify_client_disconnected("Upstream stopped the stream", middleware::disconnect_notification_type_e::upstream_requested_stop);
  middleware::notify_client_disconnected("Later video cleanup failed", middleware::disconnect_notification_type_e::server_encoder_error);
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());
}

TEST_F(MiddlewareDisconnectCycleTests, GenericUpstreamStopRetainsGracefulCodeWithoutEcho) {
  for (auto data : {nlohmann::json::object(), nlohmann::json {{"type", -1}}, nlohmann::json {{"type", 5}}, nlohmann::json {{"type", 99}}}) {
    ASSERT_TRUE(middleware::detail::handle_upstream_disconnected({{"event", "disconnected"}, {"data", data}}));
    auto request = disconnect_requests->pop(std::chrono::milliseconds {0});
    ASSERT_TRUE(request);
    EXPECT_EQ(request->termination_code, stream::detail::CONTROL_TERMINATION_GRACEFUL);
    EXPECT_FALSE(request->message.empty());
  }
  middleware::notify_client_disconnected("Upstream stopped the stream", middleware::disconnect_notification_type_e::upstream_requested_stop);
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());
}

TEST_F(MiddlewareDisconnectCycleTests, PlatformDefaultExitUsesPrivateCodeWithoutEcho) {
  ASSERT_TRUE(middleware::detail::handle_upstream_disconnected({{"event", "disconnected"}, {"data", {{"type", 0}}}}));
  auto request = disconnect_requests->pop(std::chrono::milliseconds {0});
  ASSERT_TRUE(request);
  EXPECT_EQ(request->termination_code, stream::detail::CONTROL_TERMINATION_PLATFORM_DEFAULT_EXIT);
  EXPECT_EQ(request->message, "Upstream requested default platform exit");

  middleware::notify_client_disconnected("Normal exit cleanup", middleware::disconnect_notification_type_e::client_requested_stop);
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());
}

TEST_F(MiddlewareDisconnectCycleTests, NoAvailableTimeUsesPrivateCodeAndDataMessageWithoutEcho) {
  ASSERT_TRUE(middleware::detail::handle_upstream_disconnected({{"event", "disconnected"}, {"message", "Platform exit"}, {"data", {{"type", 4}, {"message", "无可用时长，断开连接"}}}}));
  auto request = disconnect_requests->pop(std::chrono::milliseconds {0});
  ASSERT_TRUE(request);
  EXPECT_EQ(request->termination_code, stream::detail::CONTROL_TERMINATION_NO_AVAILABLE_TIME);
  EXPECT_EQ(request->message, "无可用时长，断开连接");

  middleware::notify_client_disconnected("No available time cleanup", middleware::disconnect_notification_type_e::upstream_requested_stop);
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());
}

TEST_F(MiddlewareDisconnectCycleTests, ForceTimeoutQueuesDistinctClientCodeAndReportsUpstream) {
  ASSERT_TRUE(middleware::detail::handle_upstream_disconnected({{"event", "disconnected"}, {"data", {{"type", 1}}}}));
  auto request = disconnect_requests->pop(std::chrono::milliseconds {0});
  ASSERT_TRUE(request);
  EXPECT_EQ(request->termination_code, 0x8004E001U);
  EXPECT_EQ(request->message, "Upstream requested disconnect after force timeout");

  auto notification = pop_notification();
  ASSERT_TRUE(notification.contains("data"));
  EXPECT_EQ(notification.value("event", ""), "disconnected");
  EXPECT_EQ(notification.at("data").value("type", -1), 1);
  EXPECT_EQ(notification.at("data").value("message", ""), request->message);
  EXPECT_EQ(notification.value("message", ""), request->message);
  middleware::notify_client_disconnected("Force timeout cleanup");
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());
}

TEST_F(MiddlewareDisconnectCycleTests, StandbyTimeoutQueuesDistinctClientCodeAndReportsUpstream) {
  ASSERT_TRUE(middleware::detail::handle_upstream_disconnected({{"event", "disconnected"}, {"message", "Platform timeout"}, {"data", {{"type", 2}, {"message", "长时间无操作，断开连接"}}}}));
  auto request = disconnect_requests->pop(std::chrono::milliseconds {0});
  ASSERT_TRUE(request);
  EXPECT_EQ(request->termination_code, 0x8004E002U);
  EXPECT_EQ(request->message, "长时间无操作，断开连接");

  auto notification = pop_notification();
  ASSERT_TRUE(notification.contains("data"));
  EXPECT_EQ(notification.value("event", ""), "disconnected");
  EXPECT_EQ(notification.at("data").value("type", -1), 2);
  EXPECT_EQ(notification.at("data").value("message", ""), request->message);
  EXPECT_EQ(notification.value("message", ""), request->message);
  middleware::notify_client_disconnected("Standby timeout cleanup");
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());
}

TEST_F(MiddlewareDisconnectCycleTests, TimeoutCommandsDoNotDuplicateAnAlreadyReportedReminder) {
  for (auto type : {middleware::disconnect_notification_type_e::force_timeout, middleware::disconnect_notification_type_e::standby_timeout}) {
    SCOPED_TRACE(static_cast<int>(type));
    middleware::detail::reset_disconnected_notification();
    middleware::notify_client_disconnected("Local timeout reminder", type);
    EXPECT_EQ(pop_notification().at("data").value("type", -1), static_cast<int>(type));

    ASSERT_TRUE(middleware::detail::handle_upstream_disconnected({{"event", "disconnected"}, {"data", {{"type", static_cast<int>(type)}}}}));
    auto request = disconnect_requests->pop(std::chrono::milliseconds {0});
    ASSERT_TRUE(request);
    EXPECT_EQ(request->termination_code, type == middleware::disconnect_notification_type_e::force_timeout ? 0x8004E001U : 0x8004E002U);
    middleware::notify_client_disconnected("Timeout cleanup");
    EXPECT_FALSE(middleware::g_outgoing_queue.peek());
  }
}

TEST_F(MiddlewareDisconnectCycleTests, TypeThreeWithoutMessageUsesProcessExitDefault) {
  ASSERT_TRUE(middleware::detail::handle_upstream_disconnected({{"event", "disconnected"}, {"data", {{"type", 3}}}}));
  auto request = disconnect_requests->pop(std::chrono::milliseconds {0});
  ASSERT_TRUE(request);
  EXPECT_EQ(request->termination_code, stream::detail::CONTROL_TERMINATION_GAME_PROCESS_EXIT);
  EXPECT_EQ(request->message, "Upstream reported game process exit");
}

TEST_F(MiddlewareDisconnectCycleTests, MalformedCommandsDoNotStopOrSuppressLocalReports) {
  const std::vector<nlohmann::json> invalid {
    nullptr,
    {{"event", "connected"}},
    {{"event", "disconnected"}, {"data", "invalid"}},
    {{"event", "disconnected"}, {"data", {{"type", "3"}}}},
    {{"event", "disconnected"}, {"data", {{"type", 3.5}}}},
    {{"event", "disconnected"}, {"data", {{"type", nullptr}}}},
    {{"event", "disconnected"}, {"data", {{"type", UINT64_MAX}}}},
  };
  for (const auto &msg : invalid) {
    SCOPED_TRACE(msg.dump());
    EXPECT_FALSE(middleware::detail::handle_upstream_disconnected(msg));
  }
  EXPECT_FALSE(disconnect_requests->peek());
  middleware::notify_client_disconnected("Local encoding failed", middleware::disconnect_notification_type_e::server_encoder_error);
  EXPECT_EQ(pop_notification().at("data").value("type", -1), 99);
}

TEST_F(MiddlewareDisconnectCycleTests, UpstreamCommandsPreserveArrivalOrderAndDistinctCodes) {
  ASSERT_TRUE(middleware::detail::handle_upstream_disconnected({{"event", "disconnected"}, {"data", {{"type", 3}}}}));
  ASSERT_TRUE(middleware::detail::handle_upstream_disconnected({{"event", "disconnected"}, {"message", "Platform requested stop"}}));

  auto first = disconnect_requests->pop(std::chrono::milliseconds {0});
  auto second = disconnect_requests->pop(std::chrono::milliseconds {0});
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first->termination_code, stream::detail::CONTROL_TERMINATION_GAME_PROCESS_EXIT);
  EXPECT_EQ(second->termination_code, stream::detail::CONTROL_TERMINATION_GRACEFUL);
  EXPECT_EQ(second->message, "Platform requested stop");
  EXPECT_FALSE(disconnect_requests->peek());
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());
}

TEST_F(MiddlewareDisconnectCycleTests, ClientConnectResetReenablesReportsAfterUpstreamStop) {
  ASSERT_TRUE(middleware::detail::handle_upstream_disconnected({{"event", "disconnected"}, {"data", {{"type", 3}}}}));
  middleware::notify_client_disconnected("Cleanup");
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());

  middleware::detail::reset_disconnected_notification();
  middleware::notify_client_disconnected("Client stopped the next stream", middleware::disconnect_notification_type_e::client_requested_stop);
  EXPECT_EQ(pop_notification().at("data").value("type", -1), 5);
  EXPECT_FALSE(middleware::g_outgoing_queue.peek());
}
