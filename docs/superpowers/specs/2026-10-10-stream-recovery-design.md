# Stream recovery

Approved in chat on 2026-10-10; the user requested implementation.

## Server contract

- Preserve the configured control recovery deadline (10 seconds by default). Set ENet's minimum and maximum reliable-command timeout to that deadline plus 2 seconds; the application remains responsible for its own deadline. Establishment still uses the existing deadline.
- Only successfully processed current-session control messages refresh liveness. One second without control enters a transient waiting state; release input, suppress outgoing media, and silence the microphone without reporting a completed disconnect. Valid control restores media and requests an IDR. Each recovery has a fresh deadline and a media generation; in-flight video from an earlier generation stays invalid after recovery.
- An ENet disconnect remains terminal. Capture transport diagnostics before ENet resets its peer and report observed peer-disconnect commands separately from the absence of such evidence. Preserve existing disconnect classification and notification deduplication.
- Keep token-bearing media PING registration for the session lifetime. Direct media can rebind to a new port on the same IP only with its exact negotiated token. Share immutable endpoint/source snapshots between receiver and sender; never mutate the sender's endpoint concurrently. Legacy PING remains initial-only. Gateway routes retain their internal forwarding destination.
- Track gateway route generations per channel. Clearing all routes invalidates every channel, but a port change on one channel does not invalidate another channel's queued packets. Discard queued media during a recovery wait.
- Serialize final-session teardown callbacks with new-session startup. Pending RTSP launches must explicitly reject a conflicting request rather than returning success for a silently ignored launch. Retain middleware stream configuration if preparing a launch fails.
- Microphone inactivity gating from the previous change remains in force. During control recovery, even continuing microphone packets must not reopen its recording endpoint.

## Client dependency

Full reconstruction after terminal ENet disconnect requires the client's authenticated startup/RTSP flow and fresh session keys. No client implementation is available in this workspace yet. Do not fabricate a client change or claim an end-to-end hardware test. The server changes remain useful independently.

During implementation, the user explicitly confirmed “不管客户端”. Complete and validate the server changes without extending work into the client.

## Validation

Use deterministic GTest coverage for 50 repeated short outages, deadline boundaries, stopped sessions, stale packets, exact-token route rebinding, per-channel gateway generations, microphone suppression/resume, and pending-launch conflicts. Add ENet-backed timeout/diagnostic tests where practical. Build `sunshine` and `test_sunshine` using MSYS2 UCRT64 in `cmake-build-codex-tests`. Follow `.clang-format`; localization changes, if any, are limited to `en`. Real cable tests and client integration require the user's client environment.
