# Optional audio and microphone handshakes

## Requirements

- Missing microphone PING must not stop the streaming session.
- A negotiated microphone may start later, including after the previous 10-second deadline.
- The user also explicitly requested that ordinary audio initial PING be optional.
- Video remains the required media channel. Control connection health, explicit stop requests, and fatal video errors retain their existing stop behavior.

## Design

Use a cancellable initial-PING wait shared by video, audio, and microphone workers. Video supplies the configured startup timeout. Audio and microphone supply no deadline and keep their session routing registrations until a matching PING arrives or the session ends. Poll the message queue at intervals of at most 100 ms so a pending optional handshake cannot delay session teardown until the 10-second force-kill watchdog fires.

Keep the existing PING validation, session identifier matching, gateway routing, and microphone negotiation/encryption requirements. Late microphone activation applies to a session that negotiated microphone support; accepting unnegotiated microphone traffic is outside this change.

Audio and microphone worker exceptions are logged locally and do not call `session::stop`. Remove their fatal exit guards. Video worker failures continue to stop the session. A completed optional handshake starts the existing audio capture or microphone receive path without changing packet formats.

## Alternatives

- A finite, longer optional timeout would still reject sufficiently late microphone insertion.
- Returning from the optional worker after timeout would preserve video but lose the ability to accept a later PING.
- Waiting without a deadline and checking session cancellation satisfies both requirements and is the selected approach.

## Validation

Gtest coverage must verify a required handshake deadline, optional waiting beyond the former deadline, late microphone and audio activation, continued waiting after invalid PINGs, cancellation without a PING, cancellation with continuous invalid traffic, and shutdown taking precedence over a received PING. Build both `test_sunshine` and `sunshine` with MSYS2 UCRT64. Format changed C++ files using `.clang-format` and update the disconnect notification documentation so type 21 no longer lists optional audio or microphone startup.
