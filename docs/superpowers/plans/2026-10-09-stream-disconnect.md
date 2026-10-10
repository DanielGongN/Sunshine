# Stream Disconnect Implementation Plan

> **For agentic workers:** Execute inline in the current session. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Report classified stream disconnects with a concrete `data.message` while preserving the first cause and existing payload compatibility.

**Architecture:** A shared mutex-protected disconnect state records the first cause and lets only one stop claim its notification. Capture and encoding report through this state; session stop owns notification and shutdown. Explicit RTSP/API stop operations pass their classified reason.

**Tech Stack:** C++23, nlohmann JSON, Boost, gtest, MSYS2 UCRT64, CMake/Ninja.

**Spec:** `docs/superpowers/specs/2026-10-09-stream-disconnect-design.md`

## Global Constraints

- Run shell/build commands through `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.
- Build directories start with `cmake-build-`; tests use `tests/test_sunshine.exe`.
- Follow `.clang-format`; do not change localization files.
- Preserve values 0/1/2 and the top-level message. Types 11/43/44 require evidence that this repository cannot currently produce and remain reserved.

### Task 1: Payload and first-cause state

**Files:** `src/middleware.h`, `src/middleware.cpp`, new `src/stream_disconnect.h`, `tests/unit/test_middleware.cpp`, new `tests/unit/test_stream_disconnect.cpp`.

**Interfaces:** `disconnect_state_t::record(type, message)` preserves the first cause; `claim(type, message)` returns an optional reason exactly once.

- [x] Add tests that assert `msg["data"]["message"] == msg["message"]`, all published enum values, first cause ownership, and one successful claim under concurrent calls.
- [x] Build and run the focused tests to confirm the missing behavior (three missing-message failures before implementation).
- [x] Extend enum values and JSON data with `{"type", ...}, {"message", ...}`. Implement the latch using a mutex, optional owned reason, and claimed flag.
- [x] Rebuild and run the focused tests (16 passed).

### Task 2: Wire actual session termination paths

**Files:** `src/stream.h`, `src/stream.cpp`, `src/video.h`, `src/video.cpp`, `src/rtsp.h`, `src/rtsp.cpp`, `src/nvhttp.cpp`.

**Interfaces:** `session::stop(session, type, message)` claims the first reason, shuts down, logs, and notifies once. `video::capture` accepts shared disconnect state; RTSP termination accepts a type/message.

- [x] Pass typed reasons for peer disconnect, control timeout, initial PING timeout, protocol validation errors, and upstream stop. Keep 1/2 advisory notifications separate.
- [x] Share the latch with sync/async video contexts. Record fatal capture/encoder errors before shutdown signals and queue stops; preserve display reinitialization and recoverable audio behavior.
- [x] Classify explicit `/cancel` as 10, certificate revocation as 31, server shutdown as 32, and unexpected shared broadcast termination as 42.
- [x] Format changed C++ files and build `test_sunshine` and `sunshine`.
- [x] Run focused disconnect/timeout tests plus the full existing test suite; investigate failures attributable to this change.

### Task 3: Contract documentation and final review

**Files:** new `docs/stream-disconnect-notifications.md`.

- [x] Document all values, message payload, advisory events, reserved values, and evidence limits.
- [x] Inspect the final diff for unintended changes, duplicate notifications, misclassified recoverable errors, and shutdown races.
- [x] Record actual validation results and report remaining environmental limitations.

## Validation results

- UCRT64 `test_sunshine` and `sunshine` targets compile successfully (the application output is `ws_engine.exe`).
- Focused tests: 16 passed, including payload serialization, type values, first cause preservation, concurrent notification claims, and existing timeout advisories.
- Full suite: 410 tests, 401 passed, 8 skipped, 1 existing failure in `ConfigConsistencyTest.AllConfigOptionsExistInAllFiles` for six middleware options absent from UI/docs/en locale definitions. All inputs for that test match HEAD when ignoring line endings; this feature adds no configuration options.
- `clang-format --dry-run --Werror` passes for all changed C++ files. `git -c core.whitespace=cr-at-eol diff --check` passes; the checkout already has CRLF differences.
- Client error reporting, fatal gateway attribution, and process crash monitoring remain reserved (`11/43/44`); no evidence is invented for these types.
