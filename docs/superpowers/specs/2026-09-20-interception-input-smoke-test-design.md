# Interception Input Smoke Test Design

## Goal

Build a standalone Windows diagnostic executable that proves whether the Interception keyboard and mouse path already used by `agentexe` can inject input on the target cloud gaming host. The first phase must not change Sunshine's production input path, ViGEm gamepad handling, installed drivers, or driver configuration.

Success means that an administrator can run the executable on the cloud host and observe mouse movement, one left click, and `W`, `A`, `S`, and `D` key transitions in Notepad or another input test target.

## Selected approach

The executable will dynamically load the existing 64-bit `interception.dll` from its own directory and call the DLL's exported C API. It will not link the MSVC-built `input_server.lib` from `agentexe` because that library exposes MSVC C++ ABI symbols and Sunshine is built with MSYS2/UCRT64. Dynamic loading keeps the diagnostic independent of compiler ABI and makes every required export and failure visible.

The program will reuse the same Interception kernel path as `agentexe`. It will not install, remove, restart, or reconfigure the `keyboard` and `mouse` upper-filter drivers. Those drivers have already been confirmed to be running on the target host.

## Program and deployment

The new executable will be named `interception-input-test.exe` and added under Sunshine's `tools` build area. It will be built using the repository's required MSYS2/UCRT64 toolchain and a `cmake-build-*` build directory.

Deployment consists of placing these files in the same directory on the target host:

```text
interception-input-test.exe
interception.dll
```

The DLL will be taken from the existing `agentexe/rslib/libs` deployment set. The test executable will resolve its own path and load only the sibling `interception.dll`; it will not rely on the current working directory or the global DLL search path.

## Device discovery

After creating an Interception context, the program will inspect device numbers 1 through 20. For each device it will print:

- the device number;
- whether Interception classifies it as a keyboard or mouse;
- its hardware ID when available.

Automatic selection will choose the first available keyboard and first available mouse that return usable hardware information. The program must never send the test sequence to every matching device, because that could duplicate input.

The user can override automatic selection with:

```powershell
.\interception-input-test.exe --keyboard 1 --mouse 11
```

An override must be validated against the expected Interception device type before any input is sent.

## Test sequence

Once both devices are selected, the program will print them and wait five seconds so the user can focus Notepad, an input-test page, or a game. It will then perform:

1. relative mouse movement right, down, left, and up;
2. one left-button press and release;
3. `W`, `A`, `S`, and `D` key presses and releases using keyboard scan codes.

Each send operation will print the action, the return value from `interception_send()`, and elapsed time. A return value indicating that one stroke was sent is success. The program will use short, visible hold and spacing intervals without enabling any mouse smoothing or tracking delay.

## Safety and cleanup

The executable will track the currently pressed key and mouse button. Scope-based cleanup will attempt to release any held input before context destruction, including early returns after a failed send. It will not send arbitrary HID reports or broadcast strokes to multiple devices.

This diagnostic intentionally has no `SendInput` fallback. Falling back would hide an Interception failure and invalidate the test result.

## Error handling

The program will return distinct nonzero exit codes for:

- failure to resolve its executable directory;
- missing or unloadable `interception.dll`;
- missing required DLL exports;
- failure to create an Interception context;
- failure to find or validate a keyboard or mouse device;
- failure of an input send operation.

Errors will include the relevant Windows error code where Windows supplies one. Interception send failures will include the selected device number and returned stroke count.

## Verification

Local build verification will include:

- formatting the C++ source according to `.clang-format`;
- configuring and building the tool with MSYS2/UCRT64;
- verifying that the executable imports no static Interception symbols and instead loads the DLL dynamically;
- verifying missing-DLL and invalid-device failure paths without changing installed drivers.

Cloud-host verification will be performed as administrator:

1. place the executable beside the known `agentexe` copy of `interception.dll`;
2. open Notepad and focus its edit area before the countdown ends;
3. run automatic device selection and record the full console output;
4. if necessary, rerun with explicit `--keyboard` and `--mouse` device numbers shown by enumeration;
5. confirm mouse movement, one click, and `wasd` input.

Only after this standalone test succeeds will Interception be designed as a Sunshine keyboard and mouse backend. ViGEm gamepad handling remains unchanged throughout this phase.

## Out of scope

- modifying Sunshine's production keyboard or mouse backend;
- optimizing Sunshine's input queue;
- changing ViGEm gamepad behavior;
- installing or reconfiguring Interception drivers;
- continuing the Buke or paid Virtual HID Driver integrations;
- guaranteeing compatibility with any specific anti-cheat system.
