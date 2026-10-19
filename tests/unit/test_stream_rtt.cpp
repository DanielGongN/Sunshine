/**
 * @file tests/unit/test_stream_rtt.cpp
 * @brief Validate ENet RTT measurements used by upstream heartbeats.
 */
#include "../tests_common.h"
#include "src/stream_control_diagnostics.h"

TEST(StreamControlDiagnosticsTests, RttRequiresAnAcknowledgementBeforeReportingDefault) {
  ENetPeer peer {};
  peer.state = ENET_PEER_STATE_CONNECTED;
  peer.roundTripTime = ENET_PEER_DEFAULT_ROUND_TRIP_TIME;

  EXPECT_FALSE(stream::detail::control_peer_rtt(peer));

  peer.lastReceiveTime = 100;
  EXPECT_EQ(stream::detail::control_peer_rtt(peer), 500);
}

TEST(StreamControlDiagnosticsTests, RttUsesSmoothedTransportMilliseconds) {
  ENetPeer peer {};
  peer.state = ENET_PEER_STATE_CONNECTED;
  peer.lastReceiveTime = 100;
  peer.roundTripTime = 23;

  EXPECT_EQ(stream::detail::control_peer_rtt(peer), 23);

  peer.roundTripTime = 47;
  EXPECT_EQ(stream::detail::control_peer_rtt(peer), 47);
}

TEST(StreamControlDiagnosticsTests, RttRejectsDisconnectedPeersAndZeroMeasurements) {
  ENetPeer peer {};
  peer.lastReceiveTime = 100;
  peer.roundTripTime = 23;
  for (const auto state : {ENET_PEER_STATE_DISCONNECTED, ENET_PEER_STATE_CONNECTING, ENET_PEER_STATE_DISCONNECT_LATER, ENET_PEER_STATE_ZOMBIE}) {
    peer.state = state;
    EXPECT_FALSE(stream::detail::control_peer_rtt(peer));
  }

  peer.state = ENET_PEER_STATE_CONNECTED;
  peer.roundTripTime = 0;
  EXPECT_FALSE(stream::detail::control_peer_rtt(peer));
}
