# Disconnected Notification Cycle Implementation Plan

> Execute inline in this session under the user's specified event lifecycle. Preserve existing working-tree changes.

**Goal:** Send at most one `disconnected` notification until the next incoming `client_connect`.

**Architecture:** Add an atomic middleware latch, claim it in the common disconnect notifier, and reset it at the beginning of the upstream client-connect handler. Keep session-local first-cause tracking and the existing delivery queue.

**Tech Stack:** C++23, GTest, Boost.Asio middleware, MSYS2 UCRT64, CMake/Ninja.

**Spec:** `docs/superpowers/specs/2026-10-10-disconnected-cycle-design.md`

## Global Constraints

- All disconnect types and both string overloads share one latch.
- Only incoming `client_connect` resets the cycle, before payload validation.
- Preserve existing disconnect payloads, stop behavior, and prior encoder changes.
- Build in `cmake-build-codex-tests` through the MSYS2 UCRT64 launcher; test executable is `tests/test_sunshine.exe`.

## Task 1: Notification cycle

Files: modify `src/middleware.cpp`, `src/middleware.h`, `tests/unit/test_middleware.cpp`, and `docs/stream-disconnect-notifications.md`.

- [x] Add six tests inspecting the real outgoing queue for one notification per cycle, repeat resets, concurrent notifications, both notifier overloads, outgoing connected events, and middleware availability preserving suppression.
- [x] Run the new tests before implementation and confirm the expected missing helper failure.
- [x] Implement `detail::claim_disconnected_notification()` using atomic exchange and `detail::reset_disconnected_notification()` using atomic store. Gate the common notifier and reset at entry to `client_connect`.
- [x] Document that type 1/2 reminders consume the same cycle and preserve existing offline-drop semantics.

## Task 2: Validation

- [x] Format affected C++ files and build `test_sunshine` and `sunshine`.
- [x] Run middleware, stream lifecycle/recovery, encoder recovery, and adjacent regression tests.
- [x] Review the final diff and report the updated executable and test results.

## Validation results

- Test-first build failed at link time as expected because `detail::reset_disconnected_notification()` was not implemented yet.
- MSYS2 UCRT64 builds of `sunshine` and `test_sunshine` completed successfully after implementation. `clang-format --dry-run --Werror` passed for the three affected C++ files; the focused `git diff --check` passed.
- Final regression run on 2026-10-10 at 18:50:59: **85 tests passed, 0 failures, 0 errors**, including six `MiddlewareDisconnectCycleTests`. Results: `cmake-build-codex-tests/tests/disconnected-cycle-tests.xml` and `disconnected-cycle-tests.log` in the same directory.
- Executable: `cmake-build-codex-tests/ws_engine.exe` (68,417,163 bytes). SHA-256: `707384015E929AFD07A286F6D7F9CD8DDD77BBF8BC0077C50197780F8C1FC5C6`.
- Existing profiling data emitted libgcov checksum warnings for the changed middleware objects. This run validates behavior, not coverage measurements. Queue tests avoid actual upstream network traffic.
