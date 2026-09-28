# Interception Input Backend Design

## Goal

On Windows, Sunshine prefers Interception for streamed keyboard and mouse input when `interception.dll` is available beside the Sunshine executable and its driver can be initialized with usable keyboard and mouse devices. If Interception cannot start, Sunshine tries the existing Buke HID backend, then the existing Windows `SendInput` path.

This follows the input representation used by `E:\work\cplusplus\agentexe`: scan-code keyboard strokes, Interception mouse strokes for relative motion, buttons, and wheels, and `SetCursorPos` for absolute positioning.

## Scope and Backend Selection

- Apply to Sunshine's Windows streamed keyboard and mouse input. Gamepads, touch, pen, capture, other platforms, and configuration UI stay outside this change.
- Search only the Sunshine executable directory for `interception.dll`. Resolve its C exports dynamically. Do not link the MSVC-built `input_server.lib` from `agentexe` into the MSYS2/UCRT64 build.
- At platform input context creation, attempt Interception first. Require the DLL, all needed exports, a non-null Interception context, and one usable keyboard device and one usable mouse device. Identify devices through `interception_get_hardware_id`; use the first responding device of each kind. Do not install or modify a driver.
- On any initialization failure, clean up partial Interception state and try `buke_km_hid64.dll` through the existing adapter. If Buke is unavailable or fails initialization, keep the existing `SendInput` path.
- Select one backend for the lifetime of the input context. Log the chosen backend or a concise reason for fallback once at initialization. Do not initialize Buke when Interception succeeds.
- Do not bundle or commit either vendor DLL or a driver. A DLL in the repository root is not automatically deployed beside a build output executable.

## Interception Adapter

A small Windows-only adapter owns the loaded DLL, Interception context, chosen device IDs, and a mutex. It resolves `interception_create_context`, `interception_destroy_context`, `interception_get_hardware_id`, and `interception_send`. It validates the expected stroke layouts with compile-time size checks. The adapter does not set capture filters or consume local input; injection needs only `interception_send`.

The adapter serializes sends to preserve event order. `interception_send` reports the number of strokes written. A result other than one records a rate-limited error. A runtime send failure does not switch backends midstream, because a press and its release must not go through different devices. The context and DLL are released when Sunshine's platform input context is destroyed.

## Event Mapping

| Sunshine event | Interception path |
| --- | --- |
| Keyboard press/release | Translate the virtual key with Sunshine's existing US scan-code map, or its current layout mapping for non-normalized keys when configured. Send a key stroke with scan code plus `KEY_UP` and `E0`/`E1` flags as needed. Use the existing `SendInput` path for a key that has no valid scan-code mapping, consistently for both press and release. |
| Relative mouse movement | Send one relative mouse stroke with the original signed `dx` and `dy`; do not apply Buke's signed-byte splitting. |
| Absolute mouse movement | Follow `agentexe` by converting Sunshine's target position to virtual-desktop pixels and calling `SetCursorPos`. Preserve the existing `SendInput` fallback if that call fails. |
| Left, middle, right, X1, X2 buttons | Send the matching Interception down/up state with zero movement. |
| Vertical and horizontal wheel | Send the matching wheel state and signed rolling value. Split a value that exceeds the stroke's signed 16-bit field without changing the total distance. |

The existing Buke and `SendInput` event mapping remains the fallback behavior. Backend selection does not change Sunshine's network input protocol, batching, or release tracking.

## Verification

- Test missing DLL, missing export, failed context creation, missing keyboard or mouse device, backend priority, and cleanup using a fake Interception DLL. Tests must not send real device input.
- Test scan-code and extended-key mapping, relative movement, five buttons, both wheel directions, horizontal wheel, and large wheel values through the fake DLL.
- Verify the existing Buke fallback and `SendInput` behavior remain available when Interception does not initialize.
- Format changed C++ with the repository `.clang-format` rules. Build under MSYS2/UCRT64 in a `cmake-build-*` directory and run `tests/test_sunshine`.
- On the target host, manually verify keyboard, relative and absolute movement, buttons, and wheels with the Interception driver installed. A successful DLL load alone does not prove the driver or target application accepts input.

## References

- Local reference: `E:\work\cplusplus\agentexe\server\source\input_mgr.cpp`, `E:\work\cplusplus\agentexe\rslib\include\input_server\input_define.h`, and `iinput_server.h`.
- [Interception public header](https://github.com/oblitum/Interception/blob/master/library/interception.h) and [library implementation](https://github.com/oblitum/Interception/blob/master/library/interception.c) define the ABI, device ranges, and send result.
- The [Interception README](https://github.com/oblitum/Interception/blob/master/README.md) describes the separate driver installation and licensing terms.
