# Virtual HID Integration Design

## Summary

Port the complete upstream `libvirtualhid` integration into the customized
Sunshine branch without merging unrelated upstream changes. On Windows,
Virtual HID becomes the preferred keyboard, mouse, and gamepad backend when the
driver, broker, and machine license are available. The existing `SendInput`
and ViGEm implementations remain available as automatic fallbacks.

This first version is intended for functional validation in a diskless cloud
host deployment. A Virtual HID license key is read from an ignored local file
at build time and embedded in `sunshine.exe` so that a newly provisioned host
can activate itself without a system tray or interactive Web UI.

## Goals

- Deliver Raw Input-visible keyboard and relative mouse devices on Windows
  through the installed Virtual HID Driver.
- Prefer Virtual HID gamepads while preserving the current ViGEm X360/DS4
  implementation and its customized behavior.
- Preserve Middleware input, preinitialized gamepads, input activity tracking,
  timeout handling, feedback, and reconnect behavior.
- Support unattended first-start license activation on the final cloud host.
- Retain usable input when Virtual HID is unavailable or activation fails.
- Port the upstream driver diagnostics, license API, Web UI, tray integration,
  and tests needed to maintain the feature, without making the tray a runtime
  dependency.

## Non-goals

- Sunshine will not install, update, uninstall, or redistribute the Virtual
  HID Driver.
- The integration will not attempt to conceal virtual devices or bypass
  anti-cheat systems. Raw Input compatibility does not guarantee acceptance by
  any particular game or anti-cheat product.
- This work will not merge the approximately 252 unrelated commits currently
  separating the customized branch from upstream.
- The first version will not provide a fleet license server or bypass the
  activation limits of the purchased LizardByte license.

## Supported Environment

- Windows AMD64 hosts.
- Virtual HID Driver version `2026.914.1218.10` or newer.
- The `libvirtualhid Broker` Windows service must be installed and running.
- The final cloud host must have network access when first activating.
- Every cloud host consumes a machine activation according to the purchased
  license. The diskless image may contain the driver and Sunshine, but it must
  not contain a cloned activated machine state.
- The machine identity and broker license store should persist across reboots.
  If the diskless environment discards them, the host may need to activate
  again and consume or exhaust activation capacity.

## Architecture

### Dependency and shared adapter

Add upstream `third-party/libvirtualhid` as a submodule and integrate it with
the existing CMake build. Port the upstream shared `virtualhid_input` adapter,
which owns portable conversion between Sunshine input state and
`libvirtualhid` keyboard, mouse, touch, pen, and gamepad reports.

The adapter exposes small operations for device allocation, updates, feedback,
and device refresh. Platform-specific Windows code owns backend selection and
the ViGEm fallback rather than duplicating input conversion throughout
`src/platform/windows/input.cpp`.

### Windows input context

The Windows global input context owns both:

- a `libvirtualhid` runtime and its shared keyboard/mouse devices; and
- the existing ViGEm context used as an X360/DS4 fallback.

The per-client context owns Virtual HID touch and pen state. The shared
keyboard and mouse are recreated after a license-state transition so an
activation can take effect without restarting Sunshine.

### Backend policy

Add the `gamepad_driver` configuration with the following values:

- `all`: prefer Virtual HID when the driver is usable and licensed; otherwise
  fall back to ViGEm for X360/DS4.
- `virtualhid`: require Virtual HID and disable ViGEm fallback.
- `vigembus`: force the current ViGEm implementation.

The customized build defaults to `all`.

Keyboard and mouse backend behavior is independent of whether controller input
is enabled:

- ordinary key transitions use the Virtual HID keyboard when available;
- relative movement, buttons, and scrolling use the Virtual HID mouse;
- Unicode text, unsupported keyboard usages, and absolute mouse positioning
  continue to use the existing Windows injection path;
- when the Virtual HID runtime is unavailable, keyboard and mouse input falls
  back to `SendInput()`.

### Customized ViGEm behavior

The ViGEm implementation is retained rather than replaced. Existing
preinitialization, state holding, repeated reports, feedback forwarding,
logging, and controller cleanup remain active whenever ViGEm is the selected
backend.

`middleware_gamepad_preinit` is routed through the unified gamepad allocation
interface. With a licensed Virtual HID runtime it precreates a Virtual HID
gamepad; when the policy falls back, it precreates the current ViGEm gamepad.
Cleanup must remember which backend owns each slot and release it through the
same backend.

## Build-time License Injection

The repository root may contain `Virtual_HID_KEY.txt`. Add the exact root path
`/Virtual_HID_KEY.txt` to `.gitignore`.

During CMake configuration on Windows:

1. If the file exists, read it without printing its contents.
2. Remove surrounding line-ending and whitespace characters.
3. Reject a present file that is empty or contains multiple non-empty lines.
4. Generate a private header under the `cmake-build-*` output tree.
5. Compile the key into `sunshine.exe` without adding it to CMakeCache or a
   compiler command-line definition.

The source file and generated header must never be installed or packaged. If
`Virtual_HID_KEY.txt` is absent, configuration emits a warning and builds a
functional executable without automatic activation so open-source development
and CI builds remain possible.

Embedding only keeps the key out of Git; it does not make the key secret. A
party with access to the executable can recover it through binary or memory
analysis. The deployment owner accepts this limitation for the initial
functional validation.

## Automatic Machine Activation

Normal service startup performs the following sequence:

1. Query the broker's machine license status.
2. If already licensed, do not access or submit the embedded key.
3. If unlicensed and an embedded key exists, submit it once to the local broker.
4. Clear temporary mutable buffers holding the key immediately after the
   synchronous broker request.
5. On success, select `gamepad_driver = all` behavior and create or refresh the
   shared Virtual HID keyboard and mouse.
6. On failure, record only a categorized status and continue with
   `SendInput()`/ViGEm fallbacks.

The key must never appear in logs, exceptions, configuration files, HTTP API
responses, process arguments, or test output. An explicit
`--activate-virtualhid-license` command is also provided for diagnostics. It
uses the embedded key when present and otherwise reads a key without echo from
the console.

Startup activation is machine activation, not image activation. The driver and
executable may be preinstalled in the diskless base image, but activation must
occur inside each final Windows cloud host after its machine identity exists.

## Driver and License Diagnostics

Port the upstream status model and endpoints for:

- installed driver presence and version;
- broker/service availability;
- license state, activation limit, activation usage, and active devices;
- activate, validate, and deactivate operations; and
- official purchase and account-management URLs returned by the broker.

The Web UI troubleshooting page displays these states and actions. The input
configuration page exposes the three gamepad driver policies. The system tray
integration may also be compiled, but no activation or input behavior may
depend on the tray because production runs with `system_tray = disabled`.

Only the `en` localization is updated. Do not modify `en_US`, other English
variants, or any other language.

## Failure Handling

- An unsupported driver version reports the installed and required versions
  without attempting device creation.
- A missing or stopped broker reports a distinct error from an unlicensed
  machine.
- A failed Virtual HID runtime or device allocation must not terminate
  Sunshine or the active network session.
- With `gamepad_driver = all`, eligible X360/DS4 gamepads fall back to ViGEm.
- With `gamepad_driver = virtualhid`, a missing or unlicensed driver produces a
  clear error and does not silently allocate a ViGEm device.
- Keyboard and mouse always retain the existing `SendInput()` fallback.
- Repeated startup checks do not reactivate an already licensed machine.
- No error path may format or log the embedded or submitted license key.

## Testing

### Automated tests

- Backend policy selection for `all`, `virtualhid`, and `vigembus`.
- Licensed, unlicensed, unavailable-broker, and unsupported-driver states.
- Keyboard report conversion, including the Windows fallback cases.
- Relative mouse, buttons, scrolling, and absolute-position fallback.
- Virtual HID gamepad allocation and ViGEm X360/DS4 fallback.
- Preinitialized gamepad ownership, reuse, cleanup, and feedback on both
  backends.
- Automatic activation occurs only for an unlicensed machine with an embedded
  test key.
- Activation success refreshes keyboard and mouse devices.
- Activation failure preserves fallback input.
- License keys are absent from logs and API responses.
- `Virtual_HID_KEY.txt` parsing accepts one trimmed line and rejects empty or
  multiline content without exposing it.
- Existing input activity, stream, middleware, mouse, keyboard, and gamepad
  tests continue to pass.

Tests use a fake license provider and dummy keys. They never read the real
repository-root key file.

### Build and test commands

All Windows commands use MSYS2 UCRT64 through:

```text
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c
```

Build directories use the `cmake-build-` prefix. The test executable is run
from `cmake-build-*/tests/test_sunshine`.

### Manual functional validation

On the current Windows host:

1. Confirm driver version `2026.914.1218.10` or newer and a running
   `libvirtualhid Broker` service.
2. Place the purchased key in the ignored `Virtual_HID_KEY.txt` file and build
   the production executable.
3. Start Sunshine as the same service configuration used in deployment.
4. Confirm automatic machine activation without a tray.
5. Confirm Virtual HID keyboard and mouse devices are created.
6. Verify ordinary desktop input, relative mouse movement, buttons, scrolling,
   and keyboard transitions.
7. Verify the target game receives Raw Input-visible keyboard and mouse events.
8. Disable or invalidate Virtual HID in a controlled test and verify
   `SendInput()` plus ViGEm fallback.
9. Verify the customized Middleware preinitialized gamepad and feedback paths.

## Migration Strategy

Port the upstream Virtual HID feature series and its directly required fixes
file by file. Do not merge the complete upstream branch. At each boundary,
compare the upstream implementation with the customized input, stream,
configuration, HTTP, Web UI, tray, packaging, and test changes. Preserve local
behavior unless this design explicitly replaces it.

Implementation proceeds in independently testable stages: dependency/build
integration, portable adapter, Windows backend and fallbacks, automatic
activation, diagnostics/UI, customized preinitialization, then full regression
and manual validation.
