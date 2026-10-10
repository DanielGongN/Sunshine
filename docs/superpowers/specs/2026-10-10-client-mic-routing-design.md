# Automatic client microphone routing and channel shutdown

## Approved behavior

An empty `client_mic_sink` selects the first usable, active Windows virtual cable
playback endpoint in device-ID order. An explicit name or ID retains priority and
does not silently fall back to another device. Automatic selection recognizes
Virtual Audio Cable and VB-Audio Cable adapters, requires a matching active
recording endpoint, and never falls back to ordinary speakers.

The recording endpoint is dedicated to the client microphone. It remains muted
until valid client microphone audio arrives. Closing the streaming session, stopping
the microphone packet queue, or receiving no valid microphone audio for 500 ms
stops and resets the render stream and mutes that recording endpoint. New valid
audio can resume it without reconnecting. Concurrent sessions using the same
recording endpoint keep it unmuted until the last active writer stops.

This endpoint gate also prevents newly generated host audio routed into the cable
from being captured by QQ after the client microphone channel closes. It cannot
separate different applications mixed into the same cable while the channel is
active. QQ playback and general system playback should use a separate output.

## Implementation boundaries

- Keep discovery policy and writer reference counting independent of Windows COM
  so deterministic unit tests can cover matching and concurrent shutdown.
- Enumerate only active endpoints; pair VAC lines by adapter and endpoint
  description, and VB-Audio Cable endpoints by their cable adapter. Ambiguous
  pairs are rejected instead of selecting an arbitrary recording endpoint.
- Control the paired recording endpoint with `IAudioEndpointVolume`. If pairing
  or mute control fails, microphone initialization fails with a diagnostic.
- Authenticate encrypted audio before packet-loss concealment or opening the gate.
- Recheck cancellation after queue waits and before writing. Stop/reset on idle,
  queue closure, session shutdown, and sink errors. Do not disconnect video when
  the optional microphone is unavailable.
- Remove the nonempty-sink requirement from RTSP advertisement, gateway setup,
  and the internal microphone listener.
- Update configuration documentation and only the `en` locale.

## Validation

GTest covers virtual-cable identification, paired endpoint selection, stable
ordering, explicit selection behavior, concurrent gate ownership, idle closure,
resume, queue closure, shutdown after a queued packet, and rejected encrypted
packets. Build and run `test_sunshine` with MSYS2/UCRT64 in a `cmake-build-`
directory. Real Windows cable/QQ verification remains necessary for driver mute
behavior and application-specific device routing.
