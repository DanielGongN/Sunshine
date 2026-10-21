# Control gaps without media suspension

Approved in chat on 2026-10-21: remove the coupling between one second without
valid control and outgoing audio/video suspension, while retaining input release,
authenticated liveness, final timeout, media route rebinding and disconnect diagnostics.
This supersedes the outgoing-media suspension contract in the 2026-10-10 stream
recovery design.

## Behavior

- Keep the one-second control waiting transition and release input once per gap.
- Track control responsiveness separately from the lifetime of outgoing media.
  Video and host audio remain sendable throughout a transient control gap.
- Keep the configured final control deadline (10 seconds by default) and the
  existing ENet timeout margin. Expiry or explicit stop disables outgoing media;
  late packets cannot revive a stopped session.
- Keep authenticated control-message validation as the only liveness refresh.
- Request an IDR when control resumes, without discarding an otherwise sendable
  video frame or requiring a new media generation before continuing transmission.
- Remove the gateway's control-recovery pause flag and queue-clearing API.
  Keep its normal bounded queues, route versions and stale-route rejection.
- Keep the client's microphone silence protection tied to control responsiveness,
  independently of outgoing video and host audio.
- Preserve exact-token media route rebinding, terminal ENet handling, disconnect
  reason reporting and transport diagnostics.
- Keep gateway routing, public ports, client protocols and encoder bitrate unchanged.

## Implementation

`control_recovery_t` retains independent atomic control and media availability.
The control flag transitions at one second and on recovery; the media flag changes
only on start, final expiry or stop. Remove the old transient media-generation gate.
`stream.cpp` uses media availability for outgoing A/V and control availability for
microphone silence; input release and recovery IDR remain in their existing paths.
Remove the unused pause API from `gateway.h` and `gateway.cpp`.

## Validation

Use deterministic GTests for 50 repeated short gaps, the one-second boundary,
uninterrupted media across wait/resume, final deadline expiry, late packets and
explicit stop. Run existing route, control diagnostic, disconnect, gateway,
initial PING and microphone regressions. Compile `sunshine` and `test_sunshine`
with MSYS2 UCRT64 in `cmake-build-codex-tests`, and apply `.clang-format`.
No client or physical-network latency improvement is claimed by these tests.
