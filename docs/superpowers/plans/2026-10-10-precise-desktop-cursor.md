# Precise Desktop Cursor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in precise desktop cursor mode to the Windows SendInput path.

**Architecture:** Keep the existing input queue and Windows backend selection. A small pure C++ mapper owns the logical pixel target and converts it to normalized virtual-desktop coordinates. The Windows backend queries cursor and desktop state, invokes the mapper, and injects absolute `INPUT` events via the existing SendInput wrapper.

**Tech Stack:** C++23, Win32 User32, CMake, GoogleTest, Vue configuration page.

**Spec:** `docs/superpowers/specs/2026-10-10-precise-desktop-cursor-design.md`

## Global Constraints

- Build on Windows with `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.
- Build directories start with `cmake-build-`; the test executable is `tests/test_sunshine`.
- Follow `.clang-format`; add localization only to `en.json`.
- Preserve existing uncommitted Buke removal changes.

---

### Task 1: Configuration and documentation

**Files:** `src/config.h`, `src/config.cpp`, `engine.conf`, `src_assets/common/assets/web/config.html`, `src_assets/common/assets/web/configs/tabs/Inputs.vue`, `src_assets/common/assets/web/public/assets/locale/en.json`, `docs/configuration.md`.

**Interface:** `config::input.desktop_mouse_absolute` is `false` unless enabled with `desktop_mouse_absolute = enabled`.

- [x] Add the bool field, default, parser, web form value and checkbox; document Windows-only scope and enable the mode in this checkout's `engine.conf`.
- [x] Confirm only `en.json` changed among locale files and check option spelling across all files.

### Task 2: Coordinate mapper and tests

**Files:** `src/platform/windows/desktop_cursor.h`, `tests/unit/test_desktop_cursor.cpp`.

**Interface:** A mapper accepts virtual-desktop bounds, a seeded pixel point, and ordered relative deltas; it returns clamped pixel and normalized absolute coordinates. It exposes reset and idle/bounds re-sync decisions.

- [x] Write GoogleTests for sequential movement, negative origin, bounds, idle re-sync and reset.
- [x] Implement the mapper with pure C++ arithmetic and bounded coordinates.
- [x] Build and run the focused tests.

### Task 3: Windows backend integration

**Files:** `src/platform/windows/input.cpp`, `src/platform/common.h`, `src/input.cpp`.

**Interface:** Existing `platf::move_mouse` and `platf::move_mouse_batch` use the mapper only when the option is enabled and Interception is absent. `platf::reset_desktop_mouse` clears state on stream reset.

- [x] Add a synchronized mapper to the Windows input context.
- [x] Use the existing `send_inputs` wrapper to inject absolute events and reset on failure.
- [x] Invalidate mapper state after incoming absolute events and stream reset; re-sync after idle gaps and display changes.
- [x] Build `test_sunshine` and run mapper and existing input tests.

### Task 4: Final checks

**Files:** All touched files.

- [x] Check `.clang-format` on touched C++ files and review the final diff for unrelated changes.
- [x] Report tests and any remaining host/client manual checks.
