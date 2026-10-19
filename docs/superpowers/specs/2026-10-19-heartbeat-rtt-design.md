# Client RTT in upstream heartbeats

The upstream `client_heartbeat` message reports the client control connection's
smoothed ENet round-trip time in `data.rtt_delay`, as an integer in milliseconds.
The heartbeat interval remains 30 seconds, with the existing direct message
format, `message_id`, `data.type`, and `data.net_connected` fields.

Use ENet's existing ACK/ping measurement rather than a separate ICMP probe or a
new client protocol message. Read ENet peers only on the streaming control
thread. After each control iteration, publish the maximum valid RTT among
running sessions through an atomic snapshot owned by middleware. The heartbeat
thread consumes that snapshot without accessing ENet peers or session pointers.

An ENet peer supplies a valid measurement only when it is connected, has received
an acknowledged packet (`lastReceiveTime != 0`), and its RTT is positive. This
excludes the initial default RTT of 500 ms without rejecting a measured 500 ms.
An empty set of valid running peers publishes an unavailable snapshot. Clear the
snapshot when the final client disconnects, when the first client reconnects,
and when the control broadcast shuts down. Disconnected clients and connected
clients without a measurement report JSON `null`, never a fabricated zero.

For concurrent clients, `rtt_delay` is the maximum measured RTT, so the scalar
does not depend on session iteration order. This is control transport RTT; it
does not measure video processing or display latency.

Validation covers heartbeat serialization and measurement updates, missing
samples, disconnect/reconnect cleanup, ENet sample validity, and an actual
WebSocket heartbeat. Build and run the affected gtests with MSYS2 UCRT64 using
the existing `cmake-build-codex-tests/tests/test_sunshine.exe` executable. Apply
the repository `.clang-format`. Localization files remain outside this change.
