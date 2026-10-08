# Client Connect Gamepad Preinitialization Implementation Plan

> **For agentic workers:** Execute these checked steps in order in the current session. No subagent dispatch is needed for this focused change.

**Goal:** Create the persistent Windows virtual gamepad only after an authorized middleware `client_connect` event when `middleware_gamepad_preinit` is enabled.

**Architecture:** Keep the WebSocket event branch responsible for authorization and acknowledgment. Submit the requested initialization to Sunshine's single-worker input task pool and synchronously wait for completion. Keep all ViGEm creation and cleanup in the Windows platform input implementation.

**Tech Stack:** C++23, Boost.Asio/Beast, ViGEm, CMake, GoogleTest, MSYS2 UCRT64.

**Spec:** `docs/superpowers/specs/2026-10-08-client-connect-gamepad-preinit-design.md`

## Global Constraints

- On Windows, prefix build and test commands with `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.
- Build directory names start with `cmake-build-`; the GoogleTest executable is `tests/test_sunshine` inside the build directory.
- Follow `.clang-format` for C++ code.
- Do not change localization outside `en`; this change needs no localization edits.
- Preserve the existing user change in `engine.conf`.

---

### Task 1: Move ViGEm preinitialization behind an explicit platform operation

**Files:**
- Modify: `src/platform/windows/input.cpp`
- Modify: `src/platform/common.h`

**Interfaces:**
- Produce: `bool platf::preinitialize_gamepads(input_t &input)` on Windows.
- Return true when all configured persistent slots exist; false when ViGEm allocation fails or a dynamic target occupies a persistent slot.

- [x] **Step 1: Extract the precreation block from `vigem_t::init()` into `vigem_t::preinitialize_gamepads()`**. Keep startup probe and `gamepads.resize(MAX_GAMEPADS)` in `init()`.
- [x] **Step 2: Make repeated calls a no-op after success**. Check `gamepad.preinitialized` before allocating, and refuse to replace an existing dynamic `gamepad.gp`.
- [x] **Step 3: Preserve the existing target allocation, XInput wait, notification registration, mask updates, and partial-failure cleanup**. On failure clear only targets created by this operation so a later call can retry.
- [x] **Step 4: Add the Windows platform wrapper**. It obtains `input_raw_t::vigem` from `input_t`, returns false if unavailable, and calls `vigem_t::preinitialize_gamepads()`.

### Task 2: Trigger preinitialization after `client_connect` authorization

**Files:**
- Modify: `src/input.h`
- Modify: `src/input.cpp`
- Modify: `src/middleware.cpp`
- Modify: `src/main.cpp`

**Interfaces:**
- Produce: `bool input::preinitialize_gamepads()`. It is called from the middleware WebSocket thread, queues the platform operation to the single-worker task pool, and waits on `std::future<bool>::get()`.
- On non-Windows builds it returns true without creating a target.

- [x] **Step 1: Declare and define `input::preinitialize_gamepads()`**. Guard the platform call with `_WIN32` so other platform implementations need no new symbol.
- [x] **Step 2: In the successful `client_connect` branch, call the new entry point only when `config::sunshine.middleware.gamepad_preinit` is true**. Log failure, leave certificate authorization status unchanged, then send the existing acknowledgment.
- [x] **Step 3: Confirm malformed or rejected events and disabled configuration cannot call the new entry point** by inspecting the final branch structure.
- [x] **Step 4: Destroy the middleware guard before `task_pool.stop()` in `main()`** so shutdown cannot leave the WebSocket worker waiting on a stopped pool.

### Task 3: Update current documentation and verify

**Files:**
- Modify: `docs/architecture/sunshine-middleware-gateway-architecture.md`
- Modify: `conf.md`

- [x] **Step 1: Document that successful `client_connect` now triggers the persistent slot and that startup only probes ViGEm**. State that repeat events reuse the slot and failed attempts can retry.
- [x] **Step 2: Build an existing Windows `cmake-build-*` configuration with the required MSYS2 UCRT64 shell**. Reconfigure only if the existing cache cannot build the changed sources.
- [x] **Step 3: Run the built `tests/test_sunshine` GoogleTest executable**. If a driver-dependent scenario cannot be run, report that limitation explicitly.
- [x] **Step 4: Review `git diff --check` and `git status --short`**. Ensure the pre-existing `engine.conf` change remains untouched.
