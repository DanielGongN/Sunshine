/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace stream {
  namespace detail {
    enum class timeout_notification_e {
      none,
      force,
      standby,
    };

    timeout_notification_e select_timeout_notification(
      std::int64_t session_seconds,
      std::int64_t idle_seconds,
      int force_timeout,
      int standby_timeout,
      bool already_notified
    );
  }  // namespace detail

  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
}

#include "../tests_common.h"

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(0, 2, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, sizeof(b1) + sizeof(b2) + 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e'};
  ASSERT_EQ(res, expected);
}

TEST(StreamTimeoutNotificationTests, DisabledTimeoutsDoNotNotify) {
  ASSERT_EQ(
    stream::detail::select_timeout_notification(3600, 3600, 0, 0, false),
    stream::detail::timeout_notification_e::none
  );
}

TEST(StreamTimeoutNotificationTests, ForceTimeoutUsesSessionDuration) {
  ASSERT_EQ(
    stream::detail::select_timeout_notification(900, 1, 900, 900, false),
    stream::detail::timeout_notification_e::force
  );
}

TEST(StreamTimeoutNotificationTests, StandbyTimeoutUsesIdleDuration) {
  ASSERT_EQ(
    stream::detail::select_timeout_notification(1, 900, 900, 900, false),
    stream::detail::timeout_notification_e::standby
  );
}

TEST(StreamTimeoutNotificationTests, ForceTimeoutHasPriorityWhenBothExpire) {
  ASSERT_EQ(
    stream::detail::select_timeout_notification(900, 900, 900, 900, false),
    stream::detail::timeout_notification_e::force
  );
}

TEST(StreamTimeoutNotificationTests, NotificationIsLatched) {
  ASSERT_EQ(
    stream::detail::select_timeout_notification(900, 900, 900, 900, true),
    stream::detail::timeout_notification_e::none
  );
}
