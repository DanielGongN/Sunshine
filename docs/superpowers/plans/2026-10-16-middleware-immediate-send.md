# Immediate Middleware Event Delivery Implementation Plan

**Goal:** Send queued connected events immediately and retain 30-second retries.

**Architecture:** Serialize asynchronous writes on the existing middleware
io_context, independently of the outstanding asynchronous read. Share queue
draining between notifications, heartbeats, and read completions.

**Tech Stack:** C++23, Boost.Asio, Boost.Beast, gtest, MSYS2 UCRT64.

**Spec:** ../specs/2026-10-16-middleware-immediate-send-design.md

## Constraints

- Use MSYS2 UCRT64 and the existing cmake-build-codex-tests build directory.
- Follow .clang-format and preserve existing unrelated working-tree changes.
- Keep the connected payload, matching acknowledgement, and 30-second retry interval.
- Execute this focused change in the current session.

## Task 1: Immediate serialized writes and regression coverage

Files: src/middleware.cpp, src/middleware.h, tests/unit/test_middleware.cpp.

- [x] Add a loopback WebSocket fixture that runs the real middleware client,
  accepts its subscriptions and engine information, and remains silent.
- [x] Add a test that calls notify_client_connected and requires delivery within
  two seconds. Run it against the existing code and confirm the timeout failure.
- [x] Replace the read-progress guard and synchronous queue draining with
  async_write, one active message buffer, and completion-driven queue draining.
- [x] Wake the writer from send_to_upstream regardless of pending reads, and
  share that writer with heartbeat and read-completion paths.
- [x] Add coverage for ordered bursts and a real 30-second retry carrying the
  original inner message ID; acknowledge it and verify pending state is cleared.
- [x] Build test_sunshine and sunshine, run the middleware tests, and check the
  final diff and formatting for only the intended changes.

## Validation

- The silent-upstream regression failed on the original code after two seconds.
- MSYS2 UCRT64 built both test_sunshine and sunshine (ws_engine.exe).
- All 31 Middleware* tests passed, including immediate delivery, an ordered
  16-message burst, and a real 30-second retry followed by acknowledgement.
- clang-format 19.1.5 dry-run and the scoped git diff whitespace check passed.
