# Buke HID Input Integration Design

## Purpose

On Windows, prefer the Buke HID keyboard and mouse API when
`buke_km_hid64.dll` is available beside the Sunshine executable and
`Init(0, 0)` succeeds. Preserve the existing Windows input path when the DLL
cannot be used. This builds on the earlier Buke HID smoke test design; the
diagnostic tool is not part of the current working tree.

## Scope

- Apply only to Sunshine's Windows keyboard and mouse injection. Leave
  gamepad, touch, pen, and other platforms unchanged.
- Do not bundle, download, install, or commit the vendor DLL or driver.
- Do not add a user-facing backend selector. DLL presence and successful
  initialization select the backend for the lifetime of the platform input
  context.
- Keep the unrelated existing `engine.conf` modification untouched.

## Backend and Loading

A small Windows-only Buke adapter owns the module, resolves exports, and
serializes calls into the vendor library. It obtains the Sunshine executable
directory with `GetModuleFileNameW` and loads only its sibling
`buke_km_hid64.dll` by absolute path. It validates every export required for
the supported event types before calling `Init(0, 0)`. The adapter is owned by
the existing Windows platform input context and unloaded with that context.

Missing DLL, failed load, absent export, or failed initialization selects the
existing `SendInput` backend for the whole context. Sunshine records a concise
backend selection or failure message once. The vendor event functions return
no status, so Sunshine cannot detect or retry a failed individual event.
Keeping the selected backend stable prevents a press and its release from
being sent through different devices.

## Event Mapping

| Sunshine event | Preferred Buke HID call | Existing path |
| --- | --- | --- |
| Key press and release | `KeyboardDown(vkey)`, `KeyboardUp(vkey)` | `SendInput` |
| Relative mouse motion | `MouseMoveR(dx, dy)` | `SendInput` |
| Absolute mouse motion | `MouseMove(x, y)` with virtual-desktop pixel coordinates | `SendInput` |
| Left, right, middle, X1, X2 buttons | Matching Buke down/up export | `SendInput` |
| Vertical wheel | `MouseWheelUp()` or `MouseWheelDown()` per whole wheel detent | `SendInput` |
| Horizontal wheel | No matching vendor export | `SendInput` |

Keyboard events pass Sunshine's virtual key code directly to Buke, as the
vendor API specifies VK codes. The adapter does not use Sunshine's scan-code
conversion, which remains in the `SendInput` path. Absolute mouse coordinates
are derived from Sunshine's target touch port and Windows virtual desktop
bounds. Relative motion uses the original deltas. Vertical wheel conversion
accumulates partial `WHEEL_DELTA` values so repeated small scroll packets are
not lost; it calls the vendor API once per complete detent. Horizontal scroll
continues through `SendInput` because the vendor API has no horizontal wheel
call. Synthetic touch and pen events keep their current Windows APIs.

## Error Handling and Lifecycle

The adapter is active only after all required exports and `Init(0, 0)` succeed.
Partial initialization leaves no active Buke backend. Module ownership uses
RAII. Calls into the vendor library are serialized with a mutex because the
vendor does not document thread safety. The selected backend stays fixed until
the platform input context is destroyed. Existing release handling in
Sunshine continues to emit releases through the same selected backend.

## Verification

- Use a test DLL or isolated adapter tests to verify export validation,
  initialization failure, and event-to-export mapping without injecting real
  keyboard or mouse input during automated tests.
- Verify the DLL-absent path still uses the current Windows behavior.
- Format all changed C++ according to `.clang-format`.
- Configure and build using MSYS2/UCRT64 in a `cmake-build-*` directory, then
  run that build's `tests/test_sunshine` executable.
- On a Windows host with the Buke driver and DLL installed, manually verify
  relative and absolute movement, five buttons, keys, and vertical scrolling.
  The vendor API cannot report whether a target application accepts an event.

## Source

The API signatures and VK behavior follow the vendor's
[Buke KM library documentation](https://github.com/bukepro/buke_km_lib) and
the C++ example in the local vendor package. The earlier smoke test design is
`docs/superpowers/specs/2026-09-18-buke-hid-smoke-test-design.md`.
