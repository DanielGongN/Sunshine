# Stream disconnect classification

Approved in chat on 2026-10-09. Extend `data.type` and include the concrete reason in `data.message`; preserve the existing top-level `message` for compatibility.

## Contract

| Type | Meaning |
| --- | --- |
| 0 | Unknown |
| 1 | Force hold timeout notification (not a completed disconnect) |
| 2 | Idle timeout notification (not a completed disconnect) |
| 10 | Explicit client stop request |
| 11 | Explicit client error report (reserved until the client provides one) |
| 12 | Peer disconnected, intent/root cause unknown |
| 20 | Control timeout, root cause unknown |
| 21 | Initial stream handshake timeout |
| 30 | Upstream requested stop |
| 31 | Server management requested stop |
| 32 | Normal server shutdown |
| 40 | Fatal server capture failure |
| 41 | Fatal server encoder failure |
| 42 | Other fatal server internal failure |
| 43 | Confirmed fatal gateway failure (reserved; packet errors alone do not prove this) |
| 44 | Unexpected process exit (reserved for external monitoring) |
| 50 | Fatal control protocol/authentication failure |

## Lifecycle

Share a thread-safe first-cause latch between a session and its video capture/encoding contexts. Record failures before waking other threads. The first stop reports one actual disconnect, preserving any earlier recorded concrete cause. Cleanup, later timeout checks, and sibling thread exits cannot overwrite or resend that report. Keep types 1/2 as separate advisory events.

Tag peer disconnect, ping timeout, initial PING failure, upstream commands, `/cancel`, certificate revocation, server shutdown, protocol failures, and fatal video capture/encoder exits. Recoverable audio failures, display reinitialization, transient send errors, and individual packet drops do not become fatal disconnects.

Do not infer client failure from a timeout, or user intent from an ENet disconnect. No new client wire protocol or external crash monitor is introduced. Log session ID, type, and message for every reported stop.

## Validation

Use the existing UCRT64 CMake build and gtest executable `cmake-build-codex-tests/tests/test_sunshine.exe`. Verify nested/top-level message compatibility, enum values, first cause preservation, concurrent stop deduplication, and existing timeout notification behavior. Format changed C++ files with the repository `.clang-format`.
