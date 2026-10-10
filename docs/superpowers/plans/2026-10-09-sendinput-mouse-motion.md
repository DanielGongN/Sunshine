# SendInput Mouse Motion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve streamed desktop mouse movement events on the Windows `SendInput` path while batching already queued events without merging their deltas.

**Architecture:** Expose the selected Windows mouse backend through a small platform query. The common drain keeps original mouse packets for `SendInput`, and a Windows-only fast path submits up to 32 consecutive relative moves as one ordered `INPUT` array when at least eight are already queued. Keep existing dispatch for other events.

**Tech Stack:** C++20, Win32 `SendInput`, GoogleTest, MSYS2 UCRT64.

**Spec:** `docs/superpowers/specs/2026-10-09-sendinput-mouse-motion-design.md`

## Global Constraints

- Build commands use `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.
- Build directory names begin with `cmake-build-`; the test binary is `tests/test_sunshine`.
- Follow the repository `.clang-format` for C++.
- Preserve the user's existing `engine.conf` changes.

---

### Task 1: Preserve SendInput mouse packets

**Files:** `src/platform/common.h`, `src/platform/windows/input.cpp`, `src/input.cpp`, `tests/unit/test_input_activity.cpp`.

**Interfaces:** Add `platf::uses_sendinput_mouse(input_t &)` on Windows. Pass its result into the queue's mouse batching decision. Preserve all relative and absolute movement packets when it returns true; retain all other current batching rules.

- [x] Add a test showing that queued `(+8, 0)` then `(-8, 0)` remain two events in precise mode and that a button ends a movement run.
- [x] Run the focused `InputMouseBatchTests` filter and check that the new test fails.
- [x] Implement the backend query and mouse batching decision without changing other platform behavior.
- [x] Run the focused tests and confirm they pass.

### Task 2: Submit intact relative motion in bounded arrays

**Files:** `src/platform/common.h`, `src/platform/windows/input.cpp`, `src/input.cpp`, `tests/unit/test_input_activity.cpp`.

**Interfaces:** Add `platf::move_mouse_batch(input_t &, std::span<const mouse_delta_t>)` for Windows. The input drain uses it only for a run of 8 to 32 already queued relative moves on the SendInput backend. Each delta maps to one `INPUT`; nonmovement messages terminate the run.

- [x] Test the eight-event threshold, 32-event cap, original delta order, and a button or key barrier.
- [x] Implement the queue collector and platform array sender; count every original packet in input statistics.
- [x] Handle partial `SendInput` insertion using only the unsent suffix and retain desktop-switch retry behavior.
- [x] Run focused tests, then build the Sunshine target and run `tests/test_sunshine`.

### Task 3: Review and report

**Files:** Changed C++ files and this spec and plan.

- [x] Check formatting and inspect the diff for unrelated changes.
- [x] Verify the non-SendInput paths still use their previous batching behavior.
- [x] Report automated test results and the remaining manual streamed-session check.
