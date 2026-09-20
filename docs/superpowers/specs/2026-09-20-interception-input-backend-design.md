# Interception Keyboard and Mouse Backend Design

## Goal

Integrate the Interception keyboard and mouse injection path already proven by `agentexe` directly into Sunshine on Windows. Interception becomes the preferred keyboard and mouse backend when its runtime and devices are available. Sunshine retains its current `SendInput` implementation as an automatic startup fallback, while all existing ViGEm gamepad behavior remains unchanged.

This change targets cloud gaming hosts where paid per-machine Virtual HID licensing is unsuitable and games may ignore or reject Windows `SendInput` events.

## Selected approach

Sunshine will dynamically load the existing 64-bit `interception.dll` from the directory containing `sunshine.exe` and call its exported C API. It will not link the MSVC-built `input_server.lib` from `agentexe`, because that library exposes MSVC C++ ABI symbols while Sunshine is built with MSYS2/UCRT64.

Dynamic loading avoids compiler ABI coupling, makes missing runtime components diagnosable, and allows Sunshine to retain `SendInput` without making Interception a mandatory build or runtime dependency.

The DLL and kernel driver are deployment prerequisites, not repository artifacts. `interception.dll` will be placed manually beside `sunshine.exe`; neither the DLL nor Interception driver binaries will be committed or packaged by this change.

## Backend selection

Sunshine will select one keyboard and mouse backend when the Windows platform input context is initialized:

```text
Load sibling interception.dll
        |
        +-- load or export resolution fails --> SendInput
        |
Create Interception context
        |
        +-- context creation fails ----------> SendInput
        |
Find one usable keyboard and one usable mouse
        |
        +-- either device is unavailable ----> SendInput
        |
        `-- both devices are available ------> Interception
```

Both devices must be usable before Interception is selected. Sunshine will not mix an Interception keyboard with a `SendInput` mouse, or the reverse, during normal key and mouse event processing.

The selected backend remains fixed for the lifetime of the platform input context. A runtime send failure is logged but does not trigger an immediate backend switch, because switching after a key or button press could route its release through a different backend and leave input stuck. Sunshine reevaluates backend availability the next time its platform input context is created.

## Components

The Windows implementation will add a focused Interception module:

```text
src/platform/windows/interception_input.h
src/platform/windows/interception_input.cpp
```

The module owns:

- the dynamically loaded DLL module;
- resolved Interception function pointers;
- the Interception context;
- selected keyboard and mouse device numbers;
- printable hardware IDs for diagnostics;
- conversion and send operations for keyboard and mouse strokes.

The existing Windows platform input context in `src/platform/windows/input.cpp` will own the module. Existing platform entry points such as `move_mouse()`, `button_mouse()`, `scroll()`, and `keyboard_update()` will route ordinary keyboard and mouse events through the selected backend.

The implementation will be registered in the existing Windows CMake source list and built using the repository-required MSYS2/UCRT64 toolchain and a `cmake-build-*` directory.

## Runtime loading

The backend will resolve the directory containing `sunshine.exe` and load only the sibling file named `interception.dll`. It will not rely on the process current working directory, `PATH`, or the default DLL search order.

The required DLL exports include context creation and destruction, device classification, hardware ID lookup, and stroke sending. If any required export is missing, the partial runtime is unloaded and Sunshine selects `SendInput`.

No installation, removal, restart, or configuration of the `keyboard` and `mouse` upper-filter drivers is performed by Sunshine.

## Device discovery

After context creation, the module will inspect Interception device numbers 1 through 20. For every available device it will determine whether Interception classifies it as a keyboard or mouse and request its hardware ID.

Automatic selection chooses the first usable keyboard and first usable mouse with a valid hardware ID. Sunshine never broadcasts a stroke to every matching device, which would risk duplicated input.

Startup logging will include:

- whether `interception.dll` was found and loaded;
- why initialization fell back when it did;
- the selected keyboard and mouse device numbers;
- the selected device hardware IDs;
- the final keyboard and mouse backend name.

## Input mapping

Ordinary streamed keyboard transitions will use the scan-code mapping Sunshine already maintains. Interception strokes will preserve key-up state and extended-key information, including E0/E1 handling where required.

The Interception mouse path will support:

- relative movement;
- absolute movement and virtual-desktop flags where Sunshine supplies absolute coordinates;
- left, middle, right, fourth, and fifth mouse buttons;
- vertical and horizontal scrolling.

Unicode text input remains on the existing Windows injection path because Interception keyboard strokes represent hardware scan codes rather than arbitrary Unicode text. Touch and pen continue to use their current Windows synthetic pointer implementation.

## Data flow

The network parsing and input queue remain unchanged in this phase:

```text
Moonlight input packet
        |
Sunshine input queue and task pool
        |
Windows platform input entry point
        |
        +-- Interception selected --> interception_send()
        |
        `-- fallback selected -----> SendInput()
```

ViGEm gamepad allocation, updates, feedback, and cleanup are not routed through the new module and are not modified.

Mouse queue batching or coalescing is explicitly deferred. Keeping the queue unchanged makes it possible to attribute the first test results to the injection backend rather than combining two latency-sensitive changes.

## Failure behavior and cleanup

Initialization failures are nonfatal. Sunshine logs the reason, releases all partially created Interception resources, and uses `SendInput` for ordinary keyboard and mouse events.

An Interception send succeeds only when the DLL reports that one stroke was sent. Runtime failures log the operation, selected device number, and returned stroke count. Repeated logging will be rate-limited if necessary to avoid worsening an input failure under sustained mouse traffic.

The module will track any key or mouse button state it needs for safe teardown. Context destruction performs best-effort release through the currently selected Interception devices before closing them. Sunshine's existing stream reset path continues to generate releases through the selected platform backend.

The backend will use ownership types and scope-based cleanup for the DLL, context, and any intermediate resources. A failed partial initialization must leave no live context and must not affect the `SendInput` fallback.

## Automated verification

The implementation will expose a narrow API boundary that can be exercised without installing or sending input through the real kernel driver. GoogleTest coverage will include:

- selection of `SendInput` when the DLL is absent;
- fallback when required exports are missing;
- fallback when context creation fails;
- fallback when either keyboard or mouse discovery fails;
- successful selection when both devices are available;
- keyboard scan-code, key-up, and extended-key stroke conversion;
- relative and absolute mouse conversion;
- mouse button and scroll conversion;
- send failure propagation and diagnostic state;
- complete cleanup after partial initialization and normal destruction.

Existing Sunshine tests will be run from the required `tests/test_sunshine` location in the selected `cmake-build-*` directory when the repository's existing build dependencies permit it. The Windows build itself must succeed using the required MSYS2/UCRT64 prefix.

## Manual verification

The cloud host will have the known `agentexe` copy of `interception.dll` placed beside `sunshine.exe`, with the already installed `keyboard` and `mouse` upper-filter services running.

Manual verification will cover:

1. Sunshine logs Interception as the selected backend and prints the chosen device IDs.
2. Mouse movement, buttons, wheel input, and keyboard input work in Notepad or an input test page.
3. The same inputs work in the target Tencent game.
4. ViGEm gamepad input and feedback show no regression.
5. Removing or renaming the sibling DLL causes a clean startup fallback to `SendInput`.
6. Ending a stream or stopping Sunshine does not leave keys or mouse buttons held.

Latency testing in this phase compares the Interception build against the current `SendInput` build while leaving the Sunshine input queue unchanged. Queue-depth or mouse-coalescing work will be designed separately if input backlog remains visible.

## Out of scope

- packaging or installing `interception.dll` or its kernel drivers;
- linking or modifying the closed `agentexe` `input_server.lib` implementation;
- changing ViGEm gamepad behavior;
- modifying Sunshine's input queue or mouse batching policy;
- continuing the Buke or paid Virtual HID Driver integrations;
- promising compatibility with or bypass of a particular anti-cheat implementation.
