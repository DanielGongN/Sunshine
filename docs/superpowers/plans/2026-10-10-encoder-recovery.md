# Encoder Recovery Implementation Plan

> Execute inline in this session under the user's implementation request. Preserve unrelated changes and track the tasks below.

**Goal:** Recover from transient NVENC encoding failures without immediately disconnecting the client, with a bounded restart budget and useful upstream stability fixes.

**Architecture:** Return structured failures from the asynchronous encoding run. A small recovery controller defers fatal first-cause recording until recovery is unsupported or the per-session budget is exhausted. The outer capture loop owns encoder reconstruction and retains the existing transport/session state.

**Tech Stack:** C++23, GTest, FFmpeg/NVENC, MSYS2 UCRT64, CMake/Ninja.

**Spec:** `docs/superpowers/specs/2026-10-10-encoder-recovery-design.md`

## Global Constraints

- Three rebuilds per streaming session; retain the 100 ms NVENC wait.
- Preserve terminal disconnect types, first-cause tracking, and explicit shutdown.
- Use `.clang-format`, `cmake-build-codex-tests`, and `tests/test_sunshine.exe`.
- Preserve existing uncommitted changes; localization changes are unnecessary.

## Task 1: Recovery decisions and nonblocking image drain

Files: create `src/video_recovery.h` and `tests/unit/test_video_recovery.cpp`; modify `src/thread_safe.h`.

- [x] Add GTests for recoverable failures followed by an explicit stop, three successful retries followed by exhaustion, fatal capture/nonrecoverable encoder errors, and preservation of a prior terminal reason.
- [x] Add GTests for nonblocking event consumption after a competing consumer, on an empty/stopped event, and for pointer-valued images.
- [x] Implement `video::detail::encoder_recovery_t::retry(const stream::detail::disconnect_reason_t &, bool)` and `restarts()`. Record a fatal disconnect only when returning false.
- [x] Add upstream `event_t::try_pop()` and use the existing generic empty-value representation in timed `pop()`.

## Task 2: Restore the encoder reconstruction path and upstream fixes

Files: modify `src/video.cpp` and `docs/stream-disconnect-notifications.md`.

- [x] Return `std::optional<stream::detail::disconnect_reason_t>` from `encode_run()`; return no failure for normal reinitialization/shutdown.
- [x] Let `capture_async()` consult its per-session recovery controller and continue reconstruction without recording a recoverable error; log the attempt and retain frame numbering.
- [x] Request IDR for every new encoding session and check shutdown/reinitialization immediately before encoding.
- [x] Replace image draining with `try_pop()`, guard AVCodec flushing by a valid context and positive frame count, and correct AV1 constraints per #5554.
- [x] Document recovery, the retry limit, and terminal notification behavior.

## Task 3: Validate and review

- [x] Format only affected C++ files with the repository style.
- [x] Build both `test_sunshine` and `sunshine` using the MSYS2 UCRT64 launcher.
- [x] Run encoder recovery, stream disconnect/recovery/route/control, video framerate, and relevant existing regressions.
- [x] Review the final diff, preserve unrelated changes, and report the resulting executable and hardware validation limits.

## Validation results

- MSYS2 UCRT64 builds of `sunshine` and `test_sunshine` completed successfully.
- Final focused regression run: **79 tests passed, 0 failures, 0 errors**, including five encoder recovery cases and four capture-event cases. Results: `cmake-build-codex-tests/tests/encoder-recovery-tests.xml` and `encoder-recovery-tests.log` in the same directory.
- `clang-format --dry-run --Werror` passed for all four affected C++ files; the focused `git diff --check` passed.
- Executable: `cmake-build-codex-tests/ws_engine.exe` (68,416,394 bytes). SHA-256: `A2EB76B416404D95809EF5E42627A2A9412D982D7A87A9A5B94148F7F6455D90`.
- Existing profiling data produced a libgcov checksum warning after the new test object was rebuilt. This run validates tests, not coverage measurements.
- Real GPU timeout recovery and uninterrupted client playback still require hardware streaming verification. The tests do not establish the root cause of the original GPU/driver timeout.
