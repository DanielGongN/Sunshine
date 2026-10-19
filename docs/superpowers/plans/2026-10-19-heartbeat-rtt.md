# Heartbeat RTT Implementation Plan

> Execute inline in this session, as requested by the user.

**Goal:** Add client RTT in milliseconds to `client_heartbeat.data.rtt_delay`.

**Architecture:** Sample ENet peers on the control thread and publish an atomic
optional RTT to middleware. Heartbeat serialization reads this snapshot; invalid
samples serialize as JSON null. Concurrent sessions report their maximum RTT.

**Tech Stack:** C++23, ENet, Boost.Asio/Beast, nlohmann JSON, gtest, MSYS2 UCRT64.

**Spec:** `docs/superpowers/specs/2026-10-19-heartbeat-rtt-design.md`

## Global constraints

- Use `rtt_delay` with integer milliseconds or JSON null.
- Preserve the 30-second heartbeat interval and direct message format.
- Access ENet peer data only on the streaming control thread.
- Use MSYS2 UCRT64 and build directories prefixed with `cmake-build-`.
- Format C++ using `.clang-format`; do not edit localization.

## Task 1: Publish and serialize client RTT

Files: `src/middleware.h`, `src/middleware.cpp`,
`tests/unit/test_middleware.cpp`.

Interface: `void middleware::notify_client_rtt(std::optional<std::uint32_t> rtt)`
publishes the aggregate snapshot. Internal
`nlohmann::json middleware::detail::make_heartbeat_payload(std::int64_t message_id)`
constructs the heartbeat used by the real send path and unit tests.

- [x] Add gtests for an absent client, connected without measurement, updated
      measurements, unavailable measurements, and disconnect/reconnect cleanup.
      Verify `data.rtt_delay` is null or the expected integer and retain the
      existing heartbeat fields.
- [x] Add a WebSocket delivery test that waits up to 32 seconds for a direct
      `client_heartbeat` with `data.rtt_delay == 23` and no upstream envelope.
- [x] Add an atomic signed 64-bit snapshot initialized to -1. Publish optional
      uint32 measurements with -1 representing unavailable. Clear it on the
      first connection and final disconnection in `notify_client_state`.
- [x] Extract heartbeat serialization to `make_heartbeat_payload` and call it
      from `send_heartbeat`, preserving queuing and timer behavior.

## Task 2: Sample ENet RTT and validate the complete flow

Files: `src/stream_control_diagnostics.h`, `src/stream.cpp`,
`tests/unit/test_stream_rtt.cpp` (explicitly track this file because `tests/` is ignored).

Interface:
`std::optional<std::uint32_t> stream::detail::control_peer_rtt(const ENetPeer &peer)`
returns a measurement only for a connected peer with a nonzero last ACK time
and positive RTT.

- [x] Add transport tests rejecting the initial default, unconnected peers,
      reset peers, and zero RTT; accept a measured 500 ms.
- [x] Read RTTs after control session timeout/cleanup processing, include only
      running peers, compute the maximum, and publish once per iteration.
      Publish null at control broadcast shutdown.
- [x] Build using the required MSYS2 shell:
      `cmake --build cmake-build-codex-tests --target test_sunshine -j 2`.
- [x] Run `cmake-build-codex-tests/tests/test_sunshine.exe` with
      `--gtest_filter=Middleware*:StreamControlDiagnosticsTests.*` and review
      results, including the real 30-second heartbeat delivery.
- [x] Apply `.clang-format` to changed C++ lines, review the diff, and report
      the resulting payload and validation results.

## Validation results

- MSYS2 UCRT64 `test_sunshine` build passed.
- `--gtest_filter=Middleware*:Stream*`: 80 tests passed, including the nine
  new RTT tests and actual WebSocket heartbeat delivery after 30 seconds.
- After moving the three transport RTT cases into the explicitly tracked
  `tests/unit/test_stream_rtt.cpp`, the test target rebuilt successfully and
  `--gtest_filter=MiddlewareHeartbeatTests.*:StreamControlDiagnosticsTests.*`
  passed all 11 tests.
- Changed C++ lines pass `.clang-format` validation and the diff passes
  whitespace checks.
- Existing gcov profiles emitted checksum warnings for recompiled files;
  the test process exited successfully and no coverage result is claimed.
