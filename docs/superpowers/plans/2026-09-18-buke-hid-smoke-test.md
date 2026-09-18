# Buke HID Smoke Test Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a portable Windows x64 smoke-test executable that loads the Buke HID SDK DLL beside the executable and emits a fixed mouse, click, and `W/A/S/D` sequence after a five-second countdown.

**Architecture:** A single Windows-only command-line tool owns the DLL module, resolves the six required Buke exports, and refuses to emit input until `Init(0, 0)` succeeds. A small scope guard releases any currently held test input before the DLL module is unloaded; no Buke binary is compiled into or committed to Sunshine.

**Tech Stack:** C++23, Win32 (`LoadLibraryW`, `GetProcAddress`, `GetModuleFileNameW`, `Sleep`), CMake, MSYS2/UCRT64, gtest regression suite.

**Spec:** `docs/superpowers/specs/2026-09-18-buke-hid-smoke-test-design.md`

## Global Constraints

- Windows builds use `E:\\WorkComp\\msys64\\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.
- Build directories begin with `cmake-build-`.
- C/C++ code follows the repository `.clang-format`.
- The vendor file must be named exactly `buke_km_hid64.dll` and must be loaded only from the test executable's directory.
- Do not add, copy, or commit the Buke DLL, driver, INF, CAT, installer, or license material.
- Do not modify Sunshine input handling or ViGEm support in this plan.
- Existing tests run from `<build-directory>/tests/test_sunshine`.

---

## File Structure

- Create `tools/buke_hid_test.cpp`: the complete Win32 DLL loader, Buke API wrapper, pressed-input cleanup guard, countdown, and fixed test sequence.
- Modify `tools/CMakeLists.txt`: declare the Windows `buke-hid-test` executable and apply Sunshine's normal compile options and platform libraries.

The diagnostic remains one source file because none of its private types are reused by Sunshine. Splitting a public library solely for this smoke test would expand the integration surface without improving the validation.

### Task 1: Register the Diagnostic Target

**Files:**
- Modify: `tools/CMakeLists.txt`
- Create later in Task 2: `tools/buke_hid_test.cpp`

**Interfaces:**
- Consumes: the existing `SUNSHINE_COMPILE_OPTIONS` and `PLATFORM_LIBRARIES` CMake variables.
- Produces: the CMake target `buke-hid-test` and output `tools/buke-hid-test.exe` in the selected build directory.

- [ ] **Step 1: Add the target before creating its source file**

Append this Windows tool target after `sunshinesvc`:

```cmake
add_executable(buke-hid-test buke_hid_test.cpp)
target_link_libraries(buke-hid-test
        ${PLATFORM_LIBRARIES})
target_compile_options(buke-hid-test PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
```

- [ ] **Step 2: Configure and verify the target fails because the source is absent**

Run:

```bash
cmake -S . -B cmake-build-buke-hid -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
```

Expected: CMake generation fails with `Cannot find source file: buke_hid_test.cpp` or `No SOURCES given to target: buke-hid-test`. This is the initial failing check for the new deliverable.

- [ ] **Step 3: Inspect the staged diff**

Run:

```bash
git diff -- tools/CMakeLists.txt
```

Expected: only the three target declarations above are present.

### Task 2: Implement Safe DLL Loading and the Fixed Test Sequence

**Files:**
- Create: `tools/buke_hid_test.cpp`
- Modify: `tools/CMakeLists.txt` only if formatting from Task 1 requires adjustment

**Interfaces:**
- Consumes these vendor exports with C calling convention:
  - `bool __cdecl Init(unsigned int pwd, unsigned char key)`
  - `void __cdecl MouseMoveR(int rx, int ry)`
  - `void __cdecl MouseLeftDown()`
  - `void __cdecl MouseLeftUp()`
  - `void __cdecl KeyboardDown(unsigned short vk)`
  - `void __cdecl KeyboardUp(unsigned short vk)`
- Produces exit code `0` on a completed sequence, `1` when the executable path cannot be resolved, `2` when the DLL cannot be loaded, `3` when an export is missing, and `4` when `Init(0, 0)` fails.

- [ ] **Step 1: Add the headers, typed exports, and API owner**

Create `tools/buke_hid_test.cpp` with Win32 headers and these exact private declarations:

```cpp
#include <windows.h>

#include <array>
#include <cstdio>
#include <string>
#include <string_view>

namespace {
  constexpr wchar_t dll_name[] = L"buke_km_hid64.dll";

  using init_fn = bool(__cdecl *)(unsigned int, unsigned char);
  using mouse_move_r_fn = void(__cdecl *)(int, int);
  using mouse_button_fn = void(__cdecl *)();
  using keyboard_fn = void(__cdecl *)(unsigned short);

  struct buke_api_t {
    HMODULE module = nullptr;
    init_fn init = nullptr;
    mouse_move_r_fn mouse_move_r = nullptr;
    mouse_button_fn mouse_left_down = nullptr;
    mouse_button_fn mouse_left_up = nullptr;
    keyboard_fn keyboard_down = nullptr;
    keyboard_fn keyboard_up = nullptr;

    buke_api_t() = default;
    buke_api_t(const buke_api_t &) = delete;
    buke_api_t &operator=(const buke_api_t &) = delete;
    ~buke_api_t() {
      if (module) {
        FreeLibrary(module);
      }
    }
  };
}
```

- [ ] **Step 2: Implement exact sibling-path construction**

Add a helper with this signature:

```cpp
bool get_sibling_dll_path(std::wstring &path, DWORD &error);
```

Use a `std::array<wchar_t, 32768>` buffer with `GetModuleFileNameW(nullptr, ...)`. Reject a zero return or a return equal to the buffer size, find the final slash, and replace the executable filename with `dll_name`. On failure, assign `GetLastError()` where available or `ERROR_INSUFFICIENT_BUFFER`/`ERROR_INVALID_NAME` for the two locally detected cases.

- [ ] **Step 3: Implement checked export resolution**

Add a typed helper and loader:

```cpp
template<class Function>
bool resolve_export(HMODULE module, const char *name, Function &function) {
  function = reinterpret_cast<Function>(GetProcAddress(module, name));
  if (!function) {
    std::fprintf(stderr, "Missing required export: %s\\n", name);
    return false;
  }
  return true;
}

int load_buke_api(const std::wstring &dll_path, buke_api_t &api);
```

`load_buke_api` calls `LoadLibraryW` with the absolute sibling path, prints `GetLastError()` and returns `2` on failure, resolves all six exports, and returns `3` if any export is absent. It must leave `api.module` owned by `buke_api_t` so every post-load exit path unloads it.

- [ ] **Step 4: Add the held-input cleanup guard**

Add:

```cpp
struct pressed_input_guard_t {
  buke_api_t &api;
  bool left_down = false;
  unsigned short key_down = 0;

  void release_all() noexcept;
  ~pressed_input_guard_t() {
    release_all();
  }
};
```

`release_all()` calls `keyboard_up(key_down)` when `key_down != 0`, clears it, then calls `mouse_left_up()` when `left_down` is true and clears that flag. Update the flags immediately after each successful down/up call in the sequence.

- [ ] **Step 5: Implement the countdown and deterministic input sequence**

Add `int main()` with this order (the tool takes no command-line arguments):

```cpp
std::wstring dll_path;
DWORD path_error = ERROR_SUCCESS;
buke_api_t api;

if (!get_sibling_dll_path(dll_path, path_error)) {
  std::fwprintf(stderr, L"Unable to determine DLL path (Windows error %lu).\\n", path_error);
  return 1;
}

if (const int result = load_buke_api(dll_path, api); result != 0) {
  return result;
}
if (!api.init(0, 0)) {
  std::fprintf(stderr, "Buke Init(0, 0) failed.\\n");
  return 4;
}

pressed_input_guard_t pressed {api};
```

Print and sleep for five countdown iterations. Then call `mouse_move_r` with `(100, 0)`, `(0, 100)`, `(-100, 0)`, and `(0, -100)`, sleeping 250 milliseconds after each call. Press the left button for 80 milliseconds. Finally iterate this exact array, hold each key for 120 milliseconds, and pause 150 milliseconds after releasing it:

```cpp
constexpr std::array<unsigned short, 4> test_keys {'W', 'A', 'S', 'D'};
```

Call `pressed.release_all()` before printing completion and returning `0`; the destructor remains the fallback cleanup path.

- [ ] **Step 6: Format the changed C++ file**

Run:

```bash
clang-format -i tools/buke_hid_test.cpp
```

Expected: `git diff --check` reports no whitespace errors and the file follows `.clang-format`.

- [ ] **Step 7: Configure and build the new target**

Run:

```bash
cmake -S . -B cmake-build-buke-hid -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build cmake-build-buke-hid --target buke-hid-test -j2
```

Expected: both commands exit `0`, and `cmake-build-buke-hid/tools/buke-hid-test.exe` exists.

- [ ] **Step 8: Commit the diagnostic implementation**

Run:

```bash
git add tools/CMakeLists.txt tools/buke_hid_test.cpp
git commit -m "feat: add Buke HID smoke test"
```

### Task 3: Verify Failure Behavior, Regression Safety, and Delivery

**Files:**
- Verify: `cmake-build-buke-hid/tools/buke-hid-test.exe`
- Verify: `cmake-build-buke-hid/tests/test_sunshine.exe`
- Read-only external input: `D:/Download/buke_km_lib-master/buke_km_lib-master/buke_km_lib/buke_km_lib/hid/buke_km_hid64.dll`

**Interfaces:**
- Consumes: the `buke-hid-test` target from Task 2.
- Produces: a verified executable and exact two-file cloud-host delivery instructions.

- [ ] **Step 1: Verify the missing-DLL path is safe and diagnostic**

Ensure `cmake-build-buke-hid/tools/buke_km_hid64.dll` is absent, then run:

```bash
./cmake-build-buke-hid/tools/buke-hid-test.exe
```

Expected: the program prints the absolute attempted DLL path plus a Windows error code, emits no input, and exits with code `2`.

- [ ] **Step 2: Build the existing gtest executable**

Run:

```bash
cmake --build cmake-build-buke-hid --target test_sunshine -j2
```

Expected: build exits `0` and `cmake-build-buke-hid/tests/test_sunshine.exe` exists.

- [ ] **Step 3: Run the existing gtest suite**

Run:

```bash
./cmake-build-buke-hid/tests/test_sunshine.exe
```

Expected: gtest exits `0` with no failed test cases.

- [ ] **Step 4: Verify repository state and final artifact**

Run:

```bash
git diff --check
git status --short
file cmake-build-buke-hid/tools/buke-hid-test.exe
```

Expected: no uncommitted implementation changes remain, and `file` reports a PE32+ x86-64 Windows executable.

- [ ] **Step 5: Provide the cloud-host test bundle instructions**

Tell the operator to copy these two files into one directory on the cloud host:

```text
cmake-build-buke-hid/tools/buke-hid-test.exe
D:/Download/buke_km_lib-master/buke_km_lib-master/buke_km_lib/buke_km_lib/hid/buke_km_hid64.dll
```

The operator runs `buke-hid-test.exe`, focuses the target game during the five-second countdown, and observes the square mouse movement, left click, and `W/A/S/D` sequence. The vendor API returns no per-event status, so acceptance by the target game remains a manual observation.

## Follow-up Diagnostic (Separate Scope)

After the executable is complete, inspect `E:/work/cplusplus/agentexe` read-only as a separate diagnostic task. Trace its keyboard and mouse event path from network input to the Buke DLL/driver, then compare batching, timing, threading, relative-mouse handling, key-state deduplication, and cleanup with the smoke test and Sunshine. Report evidence-backed reasons for its smoother behavior without modifying that repository unless the user separately requests changes.
