# Buke HID Smoke Test Design

## Purpose

Build a small Windows-only executable that verifies whether the installed Buke
virtual HID driver can deliver keyboard and mouse input to a focused game. This
is an isolated validation step before changing Sunshine's Windows input backend.

The test targets the Buke HID package whose INF identifies the device as
`buke\\km_hid`, driver version `09/01/2024,1.0.0.0`. It uses
`buke_km_hid64.dll`; it does not use the normal `buke_km64.dll` backend or the
Windows system driver `kbdclass.sys`.

## Scope

The change adds a single diagnostic tool and its CMake target. It does not
modify Sunshine input handling, package a vendor DLL, install a driver, change
ViGEm gamepad support, or attempt to automate game-specific validation.

## Build and Files

- Add `tools/buke_hid_test.cpp`.
- Add the `buke-hid-test` executable target to `tools/CMakeLists.txt` on
  Windows.
- Build with the repository's MSYS2/UCRT64 toolchain and a `cmake-build-*`
  build directory.
- Keep the executable self-contained apart from Windows system libraries and
  the vendor DLL. The existing Windows static linker configuration provides the
  portable runtime behavior expected by Sunshine's tools.

No Buke binaries are copied into or committed to the repository. For manual
testing, the operator copies these two files into the same directory on the
cloud host:

1. `buke-hid-test.exe`
2. `buke_km_hid64.dll` from the downloaded package's `hid` directory

## DLL Loading and Validation

The tool derives its own executable directory with `GetModuleFileNameW` and
constructs an absolute sibling path to `buke_km_hid64.dll`. It loads that exact
file rather than relying on the process search path.

After loading, the tool resolves these exports with `GetProcAddress`:

- `Init`
- `MouseMoveR`
- `MouseLeftDown`
- `MouseLeftUp`
- `KeyboardDown`
- `KeyboardUp`

It then calls `Init(0, 0)`. No input is emitted unless the DLL loads, every
required export resolves, and initialization succeeds.

## Test Sequence

On successful initialization, the tool prints a five-second countdown so the
operator can return focus to the game. It then performs this fixed sequence:

1. Move the mouse right by 100 units.
2. Move the mouse down by 100 units.
3. Move the mouse left by 100 units.
4. Move the mouse up by 100 units.
5. Press the left mouse button for 80 milliseconds, then release it.
6. Press and release `W`, `A`, `S`, and `D` in order, holding each key for 120
   milliseconds with a short pause between keys.

The square-shaped relative movement returns the pointer to its approximate
starting position while making each direction visible. The program reports the
current action to the console, though the console is not expected to remain in
the foreground during the test.

## Cleanup and Error Handling

The tool tracks whether the left mouse button or a test key is currently held.
Before normal exit, and when unwinding after a handled failure during the test,
it sends the corresponding release calls. The DLL is unloaded after cleanup.

Failures produce a concise diagnostic and a nonzero exit code:

- executable path cannot be determined;
- sibling DLL is missing or cannot be loaded, including the Windows error code;
- a required export is missing;
- `Init(0, 0)` returns false.

Because the vendor input functions return `void`, the tool can establish that
the DLL loaded and initialized but cannot programmatically prove that the game
accepted the emitted input. That final result is determined by observation on
the cloud host.

## Verification

Automated verification consists of:

- formatting the C++ source according to the repository `.clang-format`;
- configuring and building the `buke-hid-test` target under MSYS2/UCRT64;
- running the repository's `tests/test_sunshine` executable to ensure the new
  tool target does not regress existing tests;
- running the tool without the vendor DLL and confirming it exits safely with
  a useful error.

Manual cloud-host verification consists of:

1. Confirm `ROOT\\HIDCLASS\\0000` is started and uses `buke_km_hid_drv`.
2. Place the test executable and `buke_km_hid64.dll` together.
3. Start the test executable, focus the target game during the countdown, and
   observe mouse movement, click, and `W/A/S/D` input.
4. Repeat in a normal desktop application to distinguish driver/DLL failure
   from game-specific input filtering.

Testing a third-party input driver in an online game may be governed by the
game's rules. Validation should use an appropriate test environment or account.
