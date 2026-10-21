# Control Media Decoupling Implementation Plan

> Execute inline in this chat; the user has confirmed the behavior and requested implementation.

**Goal:** Continue outgoing A/V during transient control gaps while retaining
input release, authenticated liveness, final timeout, route rebinding and diagnostics.

**Architecture:** Separate control responsiveness from media lifetime in the
existing recovery state. Remove gateway suspension and transient frame-generation
gating; retain independent microphone protection and recovery keyframe requests.

**Tech Stack:** C++23, Boost.Asio, ENet, GTest, MSYS2 UCRT64.

**Spec:** `docs/superpowers/specs/2026-10-21-control-media-decoupling-design.md`

## Global Constraints

- Default final control deadline: 10 seconds; retain the existing ENet margin.
- Input release remains after one second without valid control.
- Microphone silence remains tied to control responsiveness.
- Preserve routing, ports, protocols, disconnect handling and unrelated changes.
- Follow `.clang-format`; build in `cmake-build-codex-tests` using MSYS2 UCRT64.

## Task 1: Recovery behavior and regression tests

**Files:** `src/stream_recovery.h`, `tests/unit/test_stream_recovery.cpp`.

**Interfaces:** `control_available() const` reports control responsiveness;
`media_available() const` reports whether media may be sent before terminal stop.
Keep `start`, `receive`, `poll`, `stop`, `idle_ms` and the transition enum.

- [x] Add the regression assertion that a waiting control stream still permits media:

  ```cpp
  recovery.start(start);
  EXPECT_EQ(recovery.poll(start + 1s, 10s), transition::waiting);
  EXPECT_FALSE(recovery.control_available());
  EXPECT_TRUE(recovery.media_available());
  ```

- [x] Cover repeated gaps, recovery, stop while waiting, custom final deadlines,
  and packets arriving at or beyond the deadline. Build once to observe the old
  implementation failing the new contract.
- [x] Replace the single availability/generation state with independent control
  and media atomics. `poll` clears only control availability on waiting; expiry
  and `stop` clear both. `receive` restores control and reports recovery without
  interrupting media availability.

## Task 2: Stream and gateway integration

**Files:** `src/stream.cpp`, `src/gateway.cpp`, `src/gateway.h`,
`docs/stream-disconnect-notifications.md`.

**Consumes:** Recovery interfaces from Task 1.

- [x] Gate outgoing A/V with the existing RUNNING/peer checks plus
  `session->recovery->media_available()`.
- [x] Remove `video_recovery_generation` and transient generation checks from
  video batches. Keep IDR requests on the recovered transition.
- [x] Keep `input::reset` on waiting. Describe the non-pausing behavior in its log.
- [x] Use `control_available()` for microphone suspension.
- [x] Delete `set_media_paused`, its callers and the gateway pause member/branches.
  Retain bounded queues, per-channel route versions and stale-route checks.
- [x] Update the current disconnect guide to describe continuing media and the
  independent input/microphone response during a short control gap.

## Task 3: Verification and commit

- [x] Format only the changed C++ files.
- [x] Build:

  ```bash
  cmake --build cmake-build-codex-tests --target test_sunshine sunshine --parallel 4
  ```

- [x] From `cmake-build-codex-tests/tests`, run:

  ```bash
  ./test_sunshine.exe --gtest_filter='Stream*:MediaSource*:RtspLaunch*:Gateway*:ClientMic*:Middleware*' --gtest_color=no
  ```

- [x] Inspect the final diff for remaining transient media gates and preserve
  unchanged route/timeout/diagnostic behavior. Run `git diff --check`.
- [x] Commit only task files with `fix: 解除控制短暂中断与音视频暂停的绑定`
  and a numbered Chinese body describing behavior and validation.

## Validation result

MSYS2 UCRT64 built both sunshine (ws_engine.exe) and test_sunshine successfully.
All 118 selected tests from 22 suites passed, including nine recovery-state tests,
50 repeated nine-second control gaps with uninterrupted media availability, terminal
deadlines, explicit stop, route rebinding, control diagnostics, termination delivery,
gateway handling, source address selection and microphone suspension.

The reused coverage output emitted stale gcov checksum warnings after source changes;
GTest reported zero failures and returned success. Coverage percentages were not used.
C++ formatting and git diff --check passed. No client deployment or physical-network
latency measurement was performed.
