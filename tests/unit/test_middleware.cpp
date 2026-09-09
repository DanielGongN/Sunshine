/**
 * @file tests/unit/test_middleware.cpp
 * @brief Test src/middleware.cpp helpers.
 */

#include "src/middleware.h"

#include <cstdint>
#include <nlohmann/json.hpp>

namespace middleware::detail {
  nlohmann::json make_connected_payload(std::int64_t message_id);
  nlohmann::json make_disconnected_payload(std::string_view message, disconnect_notification_type_e type);
  bool is_connected_ack_success(const nlohmann::json &msg, std::int64_t pending_message_id);
}  // namespace middleware::detail

#include "../tests_common.h"

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

TEST(MiddlewareDisconnectTests, OtherReasonUsesTypeZeroInData) {
  auto msg = middleware::detail::make_disconnected_payload(
    "network disconnected",
    middleware::disconnect_notification_type_e::other
  );

  ASSERT_EQ(msg.value("event", ""), "disconnected");
  ASSERT_EQ(msg.value("message", ""), "network disconnected");
  ASSERT_FALSE(msg.contains("type"));
  ASSERT_EQ(msg["data"].value("type", -1), 0);
}

TEST(MiddlewareDisconnectTests, ForceTimeoutUsesTypeOne) {
  auto msg = middleware::detail::make_disconnected_payload(
    "force timeout",
    middleware::disconnect_notification_type_e::force_timeout
  );

  ASSERT_EQ(msg["data"].value("type", -1), 1);
}

TEST(MiddlewareDisconnectTests, StandbyTimeoutUsesTypeTwo) {
  auto msg = middleware::detail::make_disconnected_payload(
    "standby timeout",
    middleware::disconnect_notification_type_e::standby_timeout
  );

  ASSERT_EQ(msg["data"].value("type", -1), 2);
}
