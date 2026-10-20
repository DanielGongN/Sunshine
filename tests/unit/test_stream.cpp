/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>
#include <cstdint>
#include <functional>
#include <optional>
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
      bool force_hold_active,
      std::int64_t force_hold_seconds,
      std::int64_t idle_seconds,
      int force_timeout,
      int standby_timeout,
      bool already_notified
    );

    boost::asio::ip::address select_media_source_address(
      const boost::asio::ip::address &peer_address,
      const std::optional<boost::asio::ip::address> &routed_source_address
    );
  }  // namespace detail

  boost::asio::ip::address resolve_media_source_address(
    const boost::asio::ip::udp::endpoint &peer,
    const boost::asio::ip::address &bound_address,
    std::string_view stream_type
  );

  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
}  // namespace stream

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
    stream::detail::select_timeout_notification(false, 3600, 3600, 0, 0, false),
    stream::detail::timeout_notification_e::none
  );
}

TEST(StreamTimeoutNotificationTests, ActiveHoldSelectsForceAtDeadline) {
  ASSERT_EQ(
    stream::detail::select_timeout_notification(true, 1800, 9999, 1800, 1, false),
    stream::detail::timeout_notification_e::force
  );
}

TEST(StreamTimeoutNotificationTests, ActiveHoldPausesStandby) {
  ASSERT_EQ(
    stream::detail::select_timeout_notification(true, 10, 9999, 1800, 1, false),
    stream::detail::timeout_notification_e::none
  );
}

TEST(StreamTimeoutNotificationTests, DisabledForceStillPausesStandbyDuringHold) {
  ASSERT_EQ(
    stream::detail::select_timeout_notification(true, 9999, 9999, 0, 1, false),
    stream::detail::timeout_notification_e::none
  );
}

TEST(StreamTimeoutNotificationTests, EmptyHoldSelectsStandbyAtDeadline) {
  ASSERT_EQ(
    stream::detail::select_timeout_notification(false, 9999, 1800, 1, 1800, false),
    stream::detail::timeout_notification_e::standby
  );
}

TEST(StreamTimeoutNotificationTests, EmptyHoldCannotSelectForce) {
  ASSERT_EQ(
    stream::detail::select_timeout_notification(false, 9999, 1, 1, 1800, false),
    stream::detail::timeout_notification_e::none
  );
}

TEST(StreamTimeoutNotificationTests, NotificationIsLatched) {
  ASSERT_EQ(
    stream::detail::select_timeout_notification(true, 900, 900, 900, 900, true),
    stream::detail::timeout_notification_e::none
  );
}

TEST(MediaSourceAddressTests, KeepsLoopbackForGatewayPeer) {
  auto peer = boost::asio::ip::make_address("127.0.0.1");
  auto routed = boost::asio::ip::make_address("192.168.1.10");

  EXPECT_EQ(stream::detail::select_media_source_address(peer, routed), peer);
}

TEST(MediaSourceAddressTests, UsesResolvedIpv4AddressForDirectPeer) {
  auto peer = boost::asio::ip::make_address("223.85.203.217");
  auto routed = boost::asio::ip::make_address("192.168.1.10");

  EXPECT_EQ(stream::detail::select_media_source_address(peer, routed), routed);
}

TEST(MediaSourceAddressTests, UsesResolvedIpv6AddressForDirectPeer) {
  auto peer = boost::asio::ip::make_address("2001:db8::20");
  auto routed = boost::asio::ip::make_address("2001:db8::10");

  EXPECT_EQ(stream::detail::select_media_source_address(peer, routed), routed);
}

TEST(MediaSourceAddressTests, FallsBackToIpv4AnyWhenRouteIsMissing) {
  auto peer = boost::asio::ip::make_address("223.85.203.217");

  EXPECT_EQ(
    stream::detail::select_media_source_address(peer, std::nullopt),
    boost::asio::ip::address {boost::asio::ip::address_v4::any()}
  );
}

TEST(MediaSourceAddressTests, FallsBackToIpv6AnyForWrongAddressFamily) {
  auto peer = boost::asio::ip::make_address("2001:db8::20");
  auto routed = boost::asio::ip::make_address("192.168.1.10");

  EXPECT_EQ(
    stream::detail::select_media_source_address(peer, routed),
    boost::asio::ip::address {boost::asio::ip::address_v6::any()}
  );
}

TEST(MediaSourceAddressTests, KeepsIpv4LoopbackBeforeControlConnects) {
  auto peer = boost::asio::ip::make_address("127.0.0.1");

  EXPECT_EQ(stream::detail::select_media_source_address(peer, std::nullopt), peer);
}

TEST(MediaSourceAddressTests, KeepsIpv6LoopbackBeforeControlConnects) {
  auto peer = boost::asio::ip::make_address("::1");

  EXPECT_EQ(stream::detail::select_media_source_address(peer, std::nullopt), peer);
}

TEST(MediaSourceResolutionTests, ResolvesGatewayAudioFromWildcardSocketBinding) {
  const boost::asio::ip::udp::endpoint peer {boost::asio::ip::make_address("127.0.0.1"), 41416};

  EXPECT_EQ(
    stream::resolve_media_source_address(peer, boost::asio::ip::address_v4::any(), "audio"),
    peer.address()
  );
}

TEST(MediaSourceResolutionTests, ResolvesIpv6LoopbackFromWildcardSocketBinding) {
  const boost::asio::ip::udp::endpoint peer {boost::asio::ip::make_address("::1"), 41416};

  EXPECT_EQ(
    stream::resolve_media_source_address(peer, boost::asio::ip::address_v6::any(), "audio"),
    peer.address()
  );
}

TEST(MediaSourceResolutionTests, PreservesBoundSourceForDirectAudio) {
  const boost::asio::ip::udp::endpoint peer {boost::asio::ip::make_address("192.0.2.1"), 5000};
  auto source = boost::asio::ip::make_address("192.0.2.2");

  EXPECT_EQ(stream::resolve_media_source_address(peer, source, "audio"), source);
}

#ifdef _WIN32
TEST(MediaSourceResolutionTests, SendsGatewayAudioBeforeControlConnects) {
  namespace asio = boost::asio;
  using asio::ip::udp;

  asio::io_context io_context;
  udp::socket gateway {io_context, udp::endpoint {asio::ip::address_v4::loopback(), 0}};
  udp::socket audio {io_context, udp::endpoint {udp::v4(), 0}};
  const auto peer = gateway.local_endpoint();
  auto peer_address = peer.address();
  auto source = stream::resolve_media_source_address(peer, audio.local_endpoint().address(), "audio");
  ASSERT_EQ(source, peer_address);

  const std::string payload = "audio-before-control";
  auto send_info = platf::send_info_t {
    nullptr,
    0,
    payload.data(),
    payload.size(),
    static_cast<std::uintptr_t>(audio.native_handle()),
    peer_address,
    peer.port(),
    source,
  };

  // An unspecified IP_PKTINFO source causes WSASendMsg() to fail with WSAEINVAL.
  EXPECT_TRUE(platf::send(send_info));
}
#endif
