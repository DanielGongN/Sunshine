# Client Disconnect Codes Implementation Plan

**Goal:** Classify client ENet disconnect codes and report distinct upstream types 5 and 6.

**Architecture:** Centralize the two recognized ENet values and reason classification in stream_disconnect.h. Call this classifier only when a RUNNING session receives an ENet disconnect, then use the existing session-stop and upstream notification pipeline.

**Tech Stack:** C++20, ENet, nlohmann/json, GoogleTest, MSYS2 UCRT64.

**Spec:** docs/superpowers/specs/2026-10-13-client-disconnect-codes-design.md

## Constraints

- F001 means client-requested exit; F002 means client-reported displacement.
- Outgoing data.type uses 5/6; unknown disconnects use 99.
- Only read the outer data.type for incoming platform commands.
- Preserve first-cause ownership, stopped-session behavior, and cycle notification suppression.
- Do not add 0x8004E005 or 0x8004E006.
- Prefix shell commands with the prescribed MSYS2 UCRT64 launcher.

## Implementation and verification

- [x] Add CLIENT_DISCONNECT_REQUESTED_STOP and CLIENT_DISCONNECT_DISPLACED constants and client_disconnect_reason(uint32_t) to src/stream_disconnect.h. Return client_requested_stop, client_displaced, or peer_disconnected_unknown with the hexadecimal code in the message.
- [x] Add internal client_displaced=13 in src/middleware.h. Map client_requested_stop to 5 and client_displaced to 6 in src/middleware.cpp.
- [x] In src/stream.cpp, use the classifier in the ENet disconnect handler after releasing the peer mapping, only while the session is RUNNING. Add hexadecimal disconnect data to its diagnostic log.
- [x] In tests/unit/test_stream_disconnect.cpp, verify F001/F002 and unknown values plus a pre-existing server error surviving a client reason.
- [x] In tests/unit/test_middleware.cpp, update active-exit expectations and verify types 5/6 traverse the real outgoing queue once without overwriting an earlier cause.
- [x] In tests/unit/test_stream_termination.cpp, use real loopback ENet client disconnects with F001/F002 to assert identical server event.data and classification.
- [x] Update disconnect-types.md and docs/stream-disconnect-notifications.md with both directions and the limits of attribution.
- [x] Run clang-format checks and git diff --check on edited files. Build test_sunshine and sunshine in cmake-build-codex-tests; run MiddlewareDisconnect*, StreamDisconnectTests*, and StreamTermination* tests.

Validation: both build targets succeeded; all 42 focused tests passed, including the three real ENet client-disconnect transport cases. XML results are in cmake-build-codex-tests/client-disconnect-tests.xml. GCC reported stale coverage-profile checksum warnings after recompilation; test exit status was zero. This verifies loopback transport and server behavior, not an Android UI or a deployed engine process.
