# Direct Video Source Address Design

## Problem

Sunshine currently derives `session_t::localAddress` from the control
connection. When control traffic enters through the in-process gateway, the
control connection is loopback and `localAddress` becomes `127.0.0.1`.

If the client sends its video ping directly to Sunshine's video UDP port,
`session->video.peer` is a non-loopback client endpoint. Video transmission
still supplies the control-derived loopback address to `WSASendMsg()` through
`IP_PKTINFO`. Windows rejects the resulting loopback-to-remote send with
`WSAENETUNREACH` (10051).

## Goals

- Continue supporting video transported through the existing single-port
  gateway.
- Support direct video transport through Sunshine's video UDP port while
  control and audio continue through the gateway.
- Select a routable, address-family-compatible source address independently
  for video traffic.
- Preserve the existing source-address behavior for control and audio.

## Non-goals

- Removing the gateway video protocol.
- Changing the gateway framing format.
- Moving audio or control traffic away from the gateway.
- Changing video packetization, FEC, pacing, bitrate, or encryption.
- Advertising a public video endpoint through `stream_engine_info`.

## Design

### Video-specific source address

Add a source address to the video state in `session_t`. Both the batched and
unbatched video send paths use this address instead of the control-derived
`session_t::localAddress`.

The source address is selected after `recv_ping()` establishes the final video
peer:

1. If the video peer is loopback and gateway mode redirects it to the gateway
   outbound forwarder, use an address-family-compatible loopback source.
2. If the video peer is not loopback, determine the source address Windows
   would use to route a UDP datagram to that peer. Perform this lookup with a
   temporary UDP socket: open the peer's protocol, connect it to the peer, and
   read the socket's local endpoint. UDP `connect()` performs route selection
   without transmitting application data.
3. If route selection fails, use the unspecified address for the peer's address
   family (`0.0.0.0` or `::`). This instructs the platform send path to let the
   operating system choose the source address. Log the peer and error so a
   routing or address-family problem remains diagnosable.

The route lookup is performed once per video session, not once per packet or
frame.

### Gateway compatibility

Keep the existing gateway-mode peer rewrite. A video ping delivered by the
gateway is observed as loopback and continues to produce this path:

```text
Sunshine video socket -> 127.0.0.1:<video-port + UDP_FWD_OFFSET> -> gateway -> client
```

Its video source address is loopback, so the existing gateway transport remains
valid.

A direct video ping retains its real client endpoint and produces this path:

```text
Sunshine video socket -> client public/LAN endpoint
```

Its video source address is selected from the route to the client and is not
inherited from the loopback control connection.

### Interfaces and placement

- `src/stream.cpp` owns route selection because it owns the session video peer
  and source address.
- `session_t::video` stores the selected source address alongside its peer and
  QoS state.
- A small function in `stream::detail` selects the source address. Keeping the
  selection logic behind a function makes fallback behavior directly testable.
- `src/platform/windows/misc.cpp` and the cross-platform send interfaces remain
  unchanged.

### Error handling

- Failure to open, connect, or inspect the temporary UDP socket does not abort
  the stream.
- The selector returns the address-family-compatible unspecified address and
  emits one warning per affected video session.
- A returned source address must have the same IP version as the video peer.
- Gateway loopback mode does not perform an external route lookup.

## Testing

Add focused unit coverage in `tests/unit/test_stream.cpp` for deterministic
selection behavior:

- A loopback video peer selects a compatible loopback source.
- A direct IPv4 video peer uses a successfully resolved IPv4 source rather than
  a control-derived loopback address.
- A direct IPv6 video peer uses a successfully resolved IPv6 source.
- IPv4 and IPv6 lookup failures return `0.0.0.0` and `::`, respectively.
- A resolved address with the wrong family is rejected in favor of the safe
  unspecified fallback.

Run the complete `tests/test_sunshine` test executable from an MSYS2 UCRT64
`cmake-build-*` directory after building the change.

## Success criteria

- Direct video sessions no longer produce `WSASendMsg() failed: 10051` because
  of a loopback source address.
- Existing gateway video sessions still send through the gateway forwarder.
- Audio and control behavior is unchanged.
- Batched and fallback unbatched video sends use the same video-specific source
  address.
