# Gateway Control Fast Path Design

## Problem

Video traffic now normally bypasses the single-port gateway and connects
directly to Sunshine's video UDP port. Control traffic, including keyboard and
mouse input, must continue to use the gateway's public UDP port with the
existing three-byte framing protocol:

```text
[stream_id:1][payload_length:2 big-endian][payload]
```

The current control path adds avoidable work inside the gateway. TCP and UDP
handlers share one `io_context` worker, so HTTPS or RTSP callbacks can delay UDP
processing. Inbound control packets are copied from the public receive buffer,
copied again into `udp_control_in_queue`, and serialized through a user-space
queue before being sent to the internal ENet control port. Outbound control
packets are framed, inserted into `udp_control_out_queue`, and then submitted to
the public UDP socket.

The existing gateway statistics do not measure packet residence time and are
currently disabled. Consequently, they cannot demonstrate whether the gateway
keeps control forwarding latency below the required threshold.

## Goals

- Preserve the existing public single-port gateway protocol and all stream IDs.
- Keep control, audio, and client microphone traffic on the gateway UDP port.
- Keep direct video as the normal video path while retaining gateway video
  compatibility.
- Remove the inbound and outbound user-space control queues.
- Isolate UDP callbacks from gateway TCP work.
- Keep the gateway's internal control residence time below 1 ms in every active
  one-second measurement window under the defined stress workload.
- Make gateway control latency observable without adding per-packet logging to
  the hot path.

## Non-goals

- Changing the gateway UDP frame format or client implementation.
- Moving control, audio, or client microphone traffic to direct ports.
- Removing the gateway video stream ID or its fallback transport.
- Changing ENet reliability, encryption, input packet formats, or Sunshine's
  input queue.
- Treating network transit, ENet processing, or OS input injection time as
  gateway residence time.
- Enforcing wall-clock performance limits in ordinary CI environments.

## Considered Approaches

### Control fast path with TCP/UDP isolation

Split the gateway into separate TCP and UDP execution contexts. Run UDP on a
high-priority thread and submit control sends immediately from the receive
callbacks. Keep the existing bounded audio and video queue behavior.

This is the selected approach. It removes both known control queues and
prevents TLS or RTSP work from delaying UDP dispatch while preserving the
external protocol.

### Direct control sends on the existing gateway worker

Remove the control queues but keep one shared `io_context` and worker. This is a
smaller change and is likely to perform well during steady streaming, but TCP,
audio, logging, or microphone callbacks can still delay control callbacks. It
does not provide enough isolation for a stable sub-millisecond target.

### Dedicated control relay thread and ring buffer

Parse packets on the public UDP worker and transfer control packets to a
dedicated relay thread through a bounded ring buffer. This isolates send work
but introduces another cross-thread handoff and a new queue. Since all packets
must first arrive through the shared public UDP socket, it is more complex and
less direct than isolating the UDP execution context itself.

## Architecture

The gateway uses two independent Asio contexts and workers:

```text
Gateway
|-- TCP io_context / normal-priority worker
|   |-- TLS passthrough to NVHTTP
|   |-- framed HTTPS
|   `-- framed RTSP
`-- UDP io_context / high-priority worker
    |-- control fast path
    |-- bounded audio forwarding
    |-- client microphone forwarding
    `-- gateway video compatibility path
```

The TCP acceptor and all TCP sockets use `tcp_ioc`. The public UDP socket,
internal UDP sockets, and UDP statistics timer use `udp_ioc`. The UDP worker
must not perform synchronous file I/O, DNS resolution, waits, or per-packet info
logging.

Most UDP forwarding state remains confined to the UDP worker. Public accessors
for the recorded client address and ports retain their existing synchronization.
Cross-thread requests such as `clear_udp_clients()` post their state mutation to
`udp_ioc` instead of directly touching UDP-owned containers.

## Control Data Flow

### Client to internal ENet

```text
client public UDP packet
  -> parse and validate gateway header
  -> record the current control endpoint
  -> copy the control payload once because the receive buffer is reused
  -> immediately submit async_send to 127.0.0.1:CONTROL_PORT
  -> internal ENet control server
```

`udp_control_in_queue` and `udp_control_in_send_active` are removed. The public
receive operation is rearmed without waiting for the internal send to complete.
The send buffer is retained by its completion handler.

The internal control socket may be connected to the fixed loopback ENet
endpoint so inbound forwarding can use `async_send`. It continues receiving
ENet responses on the same ephemeral source port.

### Internal ENet to client

```text
internal ENet datagram
  -> verify a current control client endpoint exists
  -> allocate and populate one framed gateway datagram
  -> verify endpoint generation before submission
  -> immediately submit async_send_to on the public UDP socket
  -> client
```

`udp_control_out_queue` and `udp_control_out_send_active` are removed. Multiple
UDP send operations may be outstanding. They are initiated from the same UDP
worker in receive order. ENet retains its existing sequence and reliability
semantics over UDP.

Audio, microphone, and fallback video processing keep their existing data
flows. Direct video does not enter the gateway hot path.

## Overload Protection

The fast path tracks inbound and outbound outstanding control sends.

- A soft threshold of 64 outstanding sends emits a rate-limited warning and is
  included in aggregate statistics.
- A hard threshold of 512 outstanding sends rejects the current datagram and
  increments an overload-drop counter instead of growing memory without bound.
- A completion decrements the appropriate outstanding count regardless of
  success, cancellation, or failure.

The expected steady-state count for ordinary keyboard and mouse traffic is zero
to two. Reaching the soft threshold indicates socket or scheduler congestion
and invalidates the sub-millisecond performance result for that window.

## Metrics and Success Criteria

Metrics use fixed latency buckets and counters so measurement does not allocate
memory per packet. The gateway emits one aggregate record per active second:

```text
[gateway][control-fast] in_packets=... out_packets=...
  in_flight(in/out)=.../...
  submit_us(p50/p95/p99/max)=.../.../.../...
  complete_us(p50/p95/p99/max)=.../.../.../...
  drops=... errors=...
```

`submit_us` measures the time from completion of gateway frame parsing until
the corresponding asynchronous send is submitted. This is the formal gateway
queue/residence metric. `complete_us` measures from parsing through the send
completion callback and diagnoses kernel or socket congestion.

For each active one-second window during the stress workload:

- `submit_us max` is less than 1000 microseconds.
- No overload drops occur.
- No non-cancellation send errors occur.
- Outstanding sends remain below the soft threshold.

If these criteria pass while keyboard or mouse input still feels delayed, the
next investigation boundary is ENet dispatch through `input_queue`; the gateway
is then excluded as the source of the measured delay.

## Error Handling and Shutdown

- A control send failure does not terminate the gateway. It increments an error
  counter and appears in the next aggregate record.
- Errors caused by cancellation during gateway shutdown do not count as runtime
  failures.
- Stale endpoint generations are rejected before an outbound send is submitted.
  Already-submitted sends may complete, but new packets use only the latest
  endpoint.
- Reaching the hard outstanding-operation limit drops the current UDP datagram,
  records its direction, and leaves the gateway running.
- TCP and UDP resources are cancelled and closed on their owning contexts.
  Both contexts are stopped and both worker threads are joined during shutdown.
- An uncaught UDP worker exception is logged as fatal because it disables all
  gateway control traffic. TCP worker failures are reported independently.

## Compatibility

The following remain unchanged:

- Public gateway TCP and UDP port numbers.
- UDP header layout, byte order, and payload-length validation.
- Stream IDs for HTTPS, video, control, audio, RTSP, and client microphone.
- Client endpoint discovery and generation checks.
- Audio and video queue limits and drop-oldest behavior.
- ENet control payloads and encryption.
- Middleware `stream_engine_info` schema.

The gateway video forwarder remains available for clients that still use it.
When video is direct, its sockets and empty queues do not participate in packet
processing and are not expected to influence control latency.

## Testing

### Unit tests

- Encode and decode the existing UDP gateway header.
- Reject truncated headers, zero-length payloads, overdeclared lengths, and
  unknown stream IDs.
- Preserve payload bytes for valid control frames.
- Reject stale client generations.
- Exercise outstanding-operation threshold accounting and completion cleanup.
- Exercise fixed latency histogram bucket and percentile calculations.

### Integration tests

Use loopback UDP sockets to represent a gateway client and the internal ENet
endpoint:

- Verify client-to-internal control forwarding strips exactly one gateway
  header and preserves the payload.
- Verify internal-to-client forwarding adds the existing gateway header and
  preserves the payload.
- Verify bidirectional bursts complete without user-space control queue growth.
- Verify endpoint replacement prevents new packets from going to the stale
  endpoint.
- Verify audio, client microphone, and gateway video compatibility paths still
  forward correctly.
- Verify TCP TLS, framed HTTPS, and framed RTSP continue running on the separate
  TCP context.

### Performance validation

Run a dedicated 60-second workload with concurrent high-frequency control,
audio, and client microphone packets. Validate every active one-second metrics
window against the success criteria. Do not place a strict sub-millisecond wall
clock assertion in ordinary CI because virtualized or overloaded runners cannot
provide a reliable scheduler guarantee.

Build with the MSYS2 UCRT64 environment required by `AGENTS.md`, use a build
directory prefixed with `cmake-build-`, and run
`<build-directory>/tests/test_sunshine` after implementation.
