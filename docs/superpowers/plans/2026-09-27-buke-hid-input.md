# Buke HID Input Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prefer the Buke HID DLL for Windows keyboard and mouse events when it is present and initializes, while preserving Sunshine input when it is unavailable.

**Architecture:** A Windows-only RAII adapter loads the sibling `buke_km_hid64.dll`, validates the vendor exports, initializes once, and serializes vendor calls. The existing platform input context owns the adapter and routes supported events to it when active. Unsupported horizontal scrolling and the inactive case use the existing `SendInput` implementation.

**Tech Stack:** C++23, Win32 dynamic loading and input APIs, CMake, GoogleTest, MSYS2/UCRT64.

**Spec:** `docs/superpowers/specs/2026-09-27-buke-hid-input-design.md`

## Global Constraints

- Use `buke_km_hid64.dll` only, loaded from Sunshine's executable directory by absolute path.
- Do not add vendor binaries, install drivers, or alter gamepad, touch, pen, or non-Windows paths.
- Keep the existing uncommitted `engine.conf` change untouched.
- Build commands run through `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c` and use a `cmake-build-*` directory.
- The test executable is `tests/test_sunshine`; C++ follows `.clang-format`.

---

## File Map

- `src/platform/windows/buke_hid_input.h`: adapter interface, typed vendor calls, and wheel state.
- `src/platform/windows/buke_hid_input.cpp`: executable-sibling DLL loading, export resolution, RAII, and event dispatch.
- `src/platform/windows/input.cpp`: select the adapter once and route Windows keyboard/mouse events.
- `cmake/compile_definitions/windows.cmake`: include the adapter source in Sunshine and test targets.
- `tests/unit/test_buke_hid_input.cpp`: adapter behavior and fallback tests.
- `tests/unit/buke_hid_fake.cpp`: fake vendor exports compiled into test DLLs only.
- `tests/CMakeLists.txt`: build the fake DLL beside `test_sunshine` and make the test target depend on it.

### Task 1: Build and test the DLL adapter

**Files:**
- Create: `src/platform/windows/buke_hid_input.h`
- Create: `src/platform/windows/buke_hid_input.cpp`
- Create: `tests/unit/buke_hid_fake.cpp`
- Create: `tests/unit/test_buke_hid_input.cpp`
- Modify: `cmake/compile_definitions/windows.cmake`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Win32 `LoadLibraryExW`, `GetProcAddress`, `GetModuleFileNameW`, `FreeLibrary` and Buke's `Init(unsigned int, unsigned char)` plus keyboard/mouse exports.
- Produces: `platf::buke_hid_input_t::create_from_executable_dir()`, `create_from_path(const std::wstring&)`, `key(uint16_t,bool)`, `move_relative(int,int)`, `move_absolute(int,int)`, `button(int,bool)`, and `wheel(int)`. The factories return `nullptr` when unavailable.

- [ ] **Step 1: Add a fake vendor DLL and failing tests.**

  Export `Init`, `KeyboardDown`, `KeyboardUp`, `MouseMove`, `MouseMoveR`, all five button down/up pairs, `MouseWheelUp`, and `MouseWheelDown`. Export `TestReset`, `TestSetInitResult`, and `TestReadEvents` to record calls in a compact buffer. Tests create the adapter from the fake DLL's absolute path and assert this sequence:

  ```cpp
  auto api = platf::buke_hid_input_t::create_from_path(test_dll_path(L"buke_hid_fake.dll"));
  ASSERT_NE(api, nullptr);
  api->key('W', false);
  api->key('W', true);
  api->move_relative(3, -2);
  api->move_absolute(100, 200);
  api->button(1, false);
  api->button(1, true);
  api->wheel(60);
  api->wheel(60);
  // Expected fake event record: key down/up, relative/absolute move,
  // left down/up, and exactly one wheel-up call.
  ```

  Also assert `create_from_path()` returns `nullptr` for a missing DLL and for `Init` returning false. A separate incomplete fake DLL target omits `KeyboardUp` to verify export validation. Remove `buke_hid_fake.cpp` from the executable's globbed source list so it is compiled only into the fake DLL targets.

- [ ] **Step 2: Build the test target and confirm the new tests fail before implementation.**

  ```sh
  cmake -S . -B cmake-build-buke-input -G Ninja -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
  cmake --build cmake-build-buke-input --target test_sunshine -j2
  ```

  The commands run inside the required MSYS2/UCRT64 shell. The first build should fail because the declared adapter API is not yet implemented.

- [ ] **Step 3: Implement the adapter.**

  Resolve every export needed for the listed event types before calling `Init(0, 0)`. Keep the module in a non-copyable RAII object, guard vendor calls and wheel remainder with one mutex, and use `std::unique_ptr<buke_hid_input_t>` only after initialization succeeds. Derive the production path from `GetModuleFileNameW`, append `buke_km_hid64.dll`, and load it with an absolute path and restricted DLL search flags. Pass VK codes directly; map Sunshine buttons `1..5` to left, middle, right, X1, X2; accumulate vertical wheel distance by `WHEEL_DELTA`.

- [ ] **Step 4: Format and run the focused tests.**

  ```sh
  clang-format -i src/platform/windows/buke_hid_input.h src/platform/windows/buke_hid_input.cpp tests/unit/buke_hid_fake.cpp tests/unit/test_buke_hid_input.cpp
  cmake --build cmake-build-buke-input --target test_sunshine -j2
  ./cmake-build-buke-input/tests/test_sunshine.exe --gtest_filter='BukeHidInput.*'
  ```

  Expected: adapter tests pass without the real vendor DLL or driver.

### Task 2: Route Sunshine Windows input through the selected backend

**Files:**
- Modify: `src/platform/windows/input.cpp`
- Test: `tests/unit/test_buke_hid_input.cpp`

**Interfaces:**
- Consumes: the adapter API from Task 1.
- Produces: preferred Buke injection for keyboard, relative/absolute mouse, buttons, and vertical wheel; existing `SendInput` fallback when the adapter is inactive, plus existing horizontal-wheel behavior.

- [ ] **Step 1: Add routing assertions where adapter-level behavior needs coverage.**

  Verify `button(2)` records middle, `button(3)` records right, `button(4/5)` records X1/X2, negative wheel distance records wheel-down, and a partial positive detent followed by a partial negative detent cancels without emitting a step.

- [ ] **Step 2: Attach an adapter to `input_raw_t` and select it in `input()`.**

  ```cpp
  struct input_raw_t {
    std::unique_ptr<buke_hid_input_t> buke_hid;
    // Existing members remain.
  };
  ```

  Construct from the executable-sibling path once per context. The adapter logs success or failure once during creation. Do not reselect mid-session.

- [ ] **Step 3: Route each supported event before constructing `INPUT`.**

  In `move_mouse`, `abs_mouse`, `button_mouse`, `scroll`, and `keyboard_update`, call the adapter and return when active. For absolute motion, use the already offset host coordinate from `client_to_touchport()` and clamp to the virtual desktop bounds without adding the display offset twice. Leave `hscroll`, Unicode text injection, and synthetic pointer input on their existing paths.

- [ ] **Step 4: Format and build Sunshine and tests.**

  ```sh
  clang-format -i src/platform/windows/input.cpp
  cmake --build cmake-build-buke-input --target sunshine test_sunshine -j2
  ./cmake-build-buke-input/tests/test_sunshine.exe
  ```

  Expected: both targets build and the complete test suite passes, except any independently confirmed pre-existing failure.

### Task 3: Final checks and handoff

**Files:**
- Check: all files from Tasks 1 and 2.

**Interfaces:**
- Consumes: built Sunshine and test artifacts.
- Produces: a concise report of implemented mapping, automated test results, and manual driver validation still needed.

- [ ] **Step 1: Verify the normal DLL-absent path without injecting input.**

  Run the adapter's missing-path test, inspect startup log output, and confirm no Buke binary has been copied into the source or build directories by the implementation.

- [ ] **Step 2: Review the diff and status.**

  ```sh
  git diff --check
  git status --short
  ```

  Confirm only intended source, CMake, test, and plan files changed and that `engine.conf` remains unmodified by this work.

- [ ] **Step 3: Report validation limits.**

  The real DLL's event functions have no return code, and automated tests use a fake. Confirm actual input delivery manually on a host with the Buke HID driver and DLL using relative and absolute movement, keys, five buttons, and vertical scrolling.

## Execution Results

- `sunshine` built as `cmake-build-buke-input/ws_engine.exe` with MSYS2/UCRT64.
- The four `BukeHidInput` tests passed with fake DLLs; the real local Buke HID DLL's export table contains every required symbol.
- The complete suite ran 389 tests: 380 passed, 8 skipped, and the existing `ConfigConsistencyTest.AllConfigOptionsExistInAllFiles` failed because middleware and timeout options are missing from configuration documentation and `en.json`.
- Manual input delivery through the real Buke HID driver remains unverified.
