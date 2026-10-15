# Reliable termination before connection cleanup

The user's repeated 60-second disconnect report is now covered by the engine log: all three sessions select standby timeout and queue `0x8004E002`; the process continues logging more than ten seconds after the third stop. This does not show a watchdog crash or prove receipt by Android.

The control sender uses reliable ENet packets, but current cleanup immediately calls `enet_peer_disconnect_now`, which discards unacknowledged packets. A lost first datagram therefore cannot be retransmitted. Gateway routes can also be cleared before the final loopback datagram is processed.

Keep immediate media shutdown and platform reporting. While a session is STOPPING, retain its peer mapping, broadcast ownership, and gateway routes for at most 1000 ms. Request `enet_peer_disconnect_later` so ENet can service ACKs and retransmit queued packets before its disconnect handshake. Finish cleanup when the peer disconnects or the deadline expires. Do not block the shared control thread or reset the deadline on repeated polls. Clear the peer pointer and mapping before releasing the session to prevent reuse of an old ENet slot. Ignore incoming application messages for stopped sessions while ENet continues handling transport packets.

Log termination as queued rather than delivered, log the drain window, and record completion or timeout with the selected code. A timeout still completes shutdown within the existing ten-second session watchdog budget. Global process shutdown retains its existing bounded lifecycle; this change targets normal session termination, including local type 1/2 timeouts.

Alternatives: sleeping before immediate disconnect would block the control worker and would not process ACKs; switching to disconnect_later without retaining the session would allow host/routes to disappear too early. A bounded, nonblocking STOPPING phase preserves the existing ownership model.

Validate with real loopback ENet hosts: dropping the first termination datagram must still deliver the exact `0x8004E001`, `0x8004E002`, or `0x8004E003` code before disconnect; normal delivery must finish early; a blackholed peer must retain the original deadline and time out; repeated sessions must not inherit closing state. Demonstrate the old immediate-close failure, then run targeted and full gtest suites and compile the service with UCRT64.
