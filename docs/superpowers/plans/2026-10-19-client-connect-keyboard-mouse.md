# Client Connect Keyboard/Mouse Implementation Plan

Execute inline for the user's requested event-driven initialization change.

**Goal:** Initialize Windows keyboard/mouse backends after authorized
`client_connect`, preferring Interception and preserving SendInput fallback.

**Architecture:** Keep platform input allocation at startup. Select the backend
once on the input worker when the event arrives; preserve initialization at
startup when middleware is disabled.

**Tech stack:** C++23, Windows input, Boost.Asio middleware, gtest, MSYS2 UCRT64.

**Spec:** `docs/superpowers/specs/2026-10-19-client-connect-keyboard-mouse-design.md`

## Constraints

- Keep `interception.dll` selection ahead of SendInput and search beside the executable.
- Initialize only after authorization, before acknowledgment, regardless of gamepad preinit.
- Repeated events reuse the selected backend, including a SendInput fallback.
- Preserve standalone initialization, gamepads, touch/pen, and non-Windows behavior.
- Use the existing input worker, `.clang-format`, and `cmake-build-*` directories.
- Do not change localization or unrelated working tree edits.
- Create a Chinese `feature:` commit after validation, with numbered details.

## Task 1: Explicit, idempotent Windows backend loading

Files: `src/platform/windows/keyboard_mouse_backend.h`,
`src/platform/windows/keyboard_mouse_backend.cpp`,
`src/platform/windows/input.cpp`, `src/platform/common.h`,
`tests/unit/test_interception_input.cpp`.

- [x] Add `keyboard_mouse_backend_t::initialize(const std::wstring &path)`,
      `initialize_from_executable_dir()`, and initialization state. Existing
      selector functions delegate to these operations.
- [x] Move backend loading and cursor-mode logging from `platf::input()` into
      `bool platf::preinitialize_keyboard_mouse(input_t &input)`; repeated calls
      return success without changing backend or handles.
- [x] Add fake-DLL tests for explicit initialization, repeated initialization,
      and retaining SendInput when the first initialization falls back.

## Task 2: Trigger initialization from authorization and validate

Files: `src/input.h`, `src/input.cpp`, `src/middleware.cpp`.

- [x] Add `bool input::preinitialize_keyboard_mouse()`; on Windows, fail for
      absent platform input, otherwise submit the platform call and wait for
      its future. On other platforms return true.
- [x] Invoke it from `input::init()` only when middleware is disabled.
- [x] Invoke it in the successful `client_connect` branch before gamepad
      preinitialization and before the acknowledgment; log unexpected failure.
- [x] Format changed lines and run `git diff --check`.
- [x] Build `test_sunshine` using MSYS2 UCRT64, then run relevant Interception,
      input, mouse, desktop cursor, and middleware suites.
- [x] Review startup and event sequencing, record validation, stage only the
      scoped files, and create the requested Chinese `feature:` commit.

## Validation results

- MSYS2 UCRT64 build of `test_sunshine` passed.
- `--gtest_filter=InterceptionInput.*:Input*:DesktopCursorMapper.*:Middleware*:MouseInputs/*`
  ran 75 tests: 71 passed, and the four existing Windows mouse HID tests were
  skipped by their existing `TODO Windows` guard.
- The fake Interception DLL tests cover explicit loading, preserving the selected
  backend across repeated initialization, and stable SendInput fallback, as well
  as existing DLL/export/context/device failures and input mappings.
- Changed C++ lines passed `.clang-format` validation. The scoped patch excludes
  pre-existing CRLF-only changes and passes whitespace checks.
- Existing gcov cache files emitted checksum warnings after recompilation;
  the test process succeeded. No coverage result or real-driver verification is
  claimed.
