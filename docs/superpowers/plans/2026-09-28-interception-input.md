# Interception Input Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prefer Interception for Windows streamed keyboard and mouse events when its DLL and driver are usable, then fall back to Buke HID and `SendInput`.

**Architecture:** A Windows-only adapter dynamically loads the public Interception C API, owns its context and chosen keyboard/mouse devices, and sends strokes. `input.cpp` selects one backend when the platform input context starts and routes events without changing the network protocol.

**Tech Stack:** C++20, Windows API, Interception C ABI, CMake, GTest, MSYS2/UCRT64.

**Spec:** `docs/superpowers/specs/2026-09-28-interception-input-design.md`

## Global Constraints

- Load only `interception.dll` beside the Sunshine executable; never install or bundle the driver or DLL.
- Backend order is Interception, Buke HID, then `SendInput`; do not initialize Buke after Interception succeeds.
- Keep the selected backend for the platform input context lifetime. Do not switch after a failed individual send.
- Preserve the user's uncommitted `engine.conf`. The user explicitly asked to remove the existing `src/platform/windows/input.cpp` Buke task pool initialization change.
- Compile via `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`; use a `cmake-build-*` directory and run `tests/test_sunshine`.
- Format changed C++ according to `.clang-format`.

---

### Task 1: Interception adapter and isolated tests

**Files:**
- Create: `src/platform/windows/interception_input.h`
- Create: `src/platform/windows/interception_input.cpp`
- Create: `tests/unit/interception_fake.cpp`
- Create: `tests/unit/test_interception_input.cpp`
- Modify: `cmake/compile_definitions/windows.cmake`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produce `platf::interception_input_t::create_from_executable_dir()` and `create_from_path(const std::wstring &)`, both returning `std::unique_ptr<interception_input_t>`.
- Produce `bool key(std::uint16_t scan_code, std::uint16_t state)`, `bool move_relative(int dx, int dy)`, `bool button(int button, bool release)`, and `bool wheel(int distance, bool horizontal)`. Every method sends through the selected device and reports success.

- [x] **Step 1: Write fake DLL and failing adapter tests.** Export the four required C functions and `TestReset`, `TestSetContextResult`, `TestSetDeviceAvailability`, and `TestReadEvents`. Build a second fake DLL with a compile definition that omits `interception_send`. The fake `get_hardware_id` returns a nonzero size only for enabled device IDs; `send` records the passed stroke and returns one. Tests assert missing DLL/export/context/device rejects activation, and keyboard/mouse sends reach the expected IDs and stroke fields. Cover five buttons and both wheel axes, including a distance outside signed 16-bit range.

```cpp
auto input = platf::interception_input_t::create_from_path(test_dll_path(L"interception_fake.dll"));
ASSERT_NE(input, nullptr);
EXPECT_TRUE(input->key(0x11, 0));
EXPECT_TRUE(input->move_relative(12, -7));
EXPECT_STREQ(fake.read_events(), "K:1,17,0;M:11,0,0,0,12,-7;");
```

- [x] **Step 2: Run the new test filter and confirm failure before implementation.**

```text
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-buke-input --target test_sunshine -j4"
cmake-build-buke-input\tests\test_sunshine.exe --gtest_filter=InterceptionInput.*
```

Expected: compilation or linking fails because the adapter is absent.

- [x] **Step 3: Implement the adapter and add CMake sources.** Use `LoadLibraryExW` with an absolute sibling path, resolve the four exports with `GetProcAddress`, call `interception_create_context`, then scan keyboard IDs 1–10 and mouse IDs 11–20 with `interception_get_hardware_id`. Release context before `FreeLibrary`. Define the exact 8-byte key and 20-byte mouse ABI structs with `static_assert`. Serialize sends with a mutex; rate-limit logs when `interception_send` does not return one. Split wheel values into signed 16-bit chunks.

```cpp
struct key_stroke_t { std::uint16_t code, state; std::uint32_t information; };
struct mouse_stroke_t {
  std::uint16_t state, flags;
  std::int16_t rolling;
  std::int32_t x, y;
  std::uint32_t information;
};
static_assert(sizeof(key_stroke_t) == 8);
static_assert(sizeof(mouse_stroke_t) == 20);
```

- [x] **Step 4: Run adapter tests and commit the self-contained adapter.**

```text
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-buke-input --target test_sunshine -j4"
cmake-build-buke-input\tests\test_sunshine.exe --gtest_filter=InterceptionInput.*
```

Expected: all Interception adapter tests pass without sending real input.

### Task 2: Sunshine input routing and priority

**Files:**
- Modify: `src/platform/windows/input.cpp`
- Create: `src/platform/windows/keyboard_mouse_backend.h`
- Create: `src/platform/windows/keyboard_mouse_backend.cpp`
- Modify: `tests/unit/test_interception_input.cpp`
- Modify: `cmake/compile_definitions/windows.cmake`

**Interfaces:**
- Consume the Task 1 adapter methods. Keep the existing `buke_hid_input_t` adapter unchanged.
- Produce `keyboard_mouse_backend_t select_keyboard_mouse_backend(const std::wstring &interception_path, const std::wstring &buke_path)`, returning two nullable `unique_ptr` members. Only one member can be non-null. Also produce `select_keyboard_mouse_backend_from_executable_dir()` to construct the two absolute sibling paths with `GetModuleFileNameW`; `platf::input()` calls it directly.

- [x] **Step 1: Add failing backend-priority tests.** Call `select_keyboard_mouse_backend` with both fake DLL paths and expect only Interception to be active; force the fake Interception context to fail and expect Buke; pass two missing paths and expect both members null.

```cpp
auto backend = platf::select_keyboard_mouse_backend(
  test_dll_path(L"interception_fake.dll"), test_dll_path(L"buke_hid_fake.dll"));
EXPECT_NE(backend.interception, nullptr);
EXPECT_EQ(backend.buke_hid, nullptr);
```

- [x] **Step 2: Implement selection and route events in `input.cpp`.** Add the backend holder to `input_raw_t`; select it directly during platform input initialization. Send relative movement, button, vertical wheel, and horizontal wheel through Interception when selected. For absolute movement, clamp virtual-desktop pixel coordinates and call `SetCursorPos`, then use the existing `SendInput` absolute path if it fails. Keep Buke and `SendInput` branches as fallbacks.

```cpp
raw.keyboard_mouse = select_keyboard_mouse_backend_from_executable_dir();
```

- [x] **Step 3: Reuse Sunshine's keyboard scan-code conversion.** Compute `ki.wScan` and `KEYEVENTF_EXTENDEDKEY` as the existing `SendInput` branch does. When Interception is active and `ki.wScan != 0`, convert release and extended flags to Interception state bits and call `key`; otherwise use the existing `SendInput` call. A key's down and up use the same mapping.

```cpp
if (raw.keyboard_mouse.interception && ki.wScan) {
  const std::uint16_t state = (release ? 0x01 : 0) | ((ki.dwFlags & KEYEVENTF_EXTENDEDKEY) ? 0x02 : 0);
  raw.keyboard_mouse.interception->key(ki.wScan, state);
  return;
}
```

- [x] **Step 4: Build and run focused tests.** Confirm the previous Buke task pool initialization change is removed as the user requested.

```text
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-buke-input --target sunshine test_sunshine -j4"
cmake-build-buke-input\tests\test_sunshine.exe --gtest_filter=InterceptionInput.*:BukeHidInput.*
```

Expected: build succeeds and both backend test suites pass.

### Task 3: Final verification and deployment notes

**Files:**
- Modify: `docs/superpowers/plans/2026-09-28-interception-input.md` (mark completed steps)

- [x] **Step 1: Format and run the complete test executable.**

```text
E:\WorkComp\LLVM\bin\clang-format.exe -i src/platform/windows/interception_input.h src/platform/windows/interception_input.cpp src/platform/windows/input.cpp src/platform/windows/keyboard_mouse_backend.h src/platform/windows/keyboard_mouse_backend.cpp tests/unit/interception_fake.cpp tests/unit/test_interception_input.cpp
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-buke-input --target sunshine test_sunshine -j4"
cmake-build-buke-input\tests\test_sunshine.exe
```

Result: 390 tests passed, 8 skipped, and the existing `ConfigConsistencyTest.AllConfigOptionsExistInAllFiles` failed because `force_disconnected_timeout`, `standby_disconnected_timeout`, and four `middleware_*` options are absent from `config.html`, `configuration.md`, and `en.json`. The new fake-DLL tests do not send real device input.

- [x] **Step 2: Review the diff and report runtime verification limits.** Check `git diff --check` and `git status --short`. Confirm `engine.conf` remains untouched and the pre-existing `input.cpp` task pool change is gone. Report that the user must place `interception.dll` beside the deployed `ws_engine.exe` and have the Interception driver installed; local fake-DLL tests cannot prove acceptance on the cloud host.
