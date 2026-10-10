# Moonlight display mode authority

## Goal and approval

The host display resolution and refresh rate follow the mode requested by Moonlight, rather than the `width`, `height`, and `fps` in the upstream `client_connect` event. The user approved extending the change to both host display settings in the conversation on 2026-10-10.

## Existing behavior

RTSP ANNOUNCE already supplies the video encoder dimensions and frame rate. However, the middleware launch path copies upstream dimensions and FPS into `launch_session_t` and configures the display before receiving ANNOUNCE. Automatic display configuration can therefore select a different mode from the eventual stream.

## Design

1. Middleware launches do not consume upstream width, height, or FPS. Mark their display mode as pending and leave those values unknown until ANNOUNCE.
2. Keep early display preparation and encoder probing. While the mode is pending, display configuration may activate the selected device and prepare its topology/HDR, but skips resolution, refresh rate, and mode remapping. This keeps an inactive VDD usable for encoder probing.
3. Parse and validate Moonlight's positive dimensions and frame rate in ANNOUNCE. Retain the existing check that accepts `clientRefreshRateX100` only when it agrees with the requested stream FPS within 1%.
4. After a valid ANNOUNCE is admitted, fill the pending launch mode from the negotiated video configuration. Apply the completed display configuration before starting the first stream. A joining stream must not change another running stream's display mode.
5. Use an optional hundredths-of-Hz field on the launch session to retain accepted rates such as 59.94 Hz. Integer-only launch paths retain their existing behavior.
6. Log the negotiated stream resolution and rate at INFO level, clearly identifying these as client-requested values.

## Configuration and failure behavior

- Existing display-configuration enable/disable, automatic/manual settings, SOPS, device selection, and mode-remapping policies still apply. The change supplies authoritative inputs rather than overriding user configuration.
- Disabled automatic resolution or refresh-rate settings leave that aspect of the host display unchanged; manual settings retain their configured values.
- Unsupported physical/VDD modes continue through the existing display-setting error/retry behavior. Streaming retains its requested encoding mode even if display switching fails.
- Missing or nonpositive ANNOUNCE dimensions or FPS are rejected before changing display state. There is no fallback to upstream dimensions.
- Ordinary Moonlight HTTP `/launch` and `/resume` still prepare the display from their client-originated parameters.
- Connection cleanup and display restoration continue using the existing machinery.

## Alternatives considered

- Removing all early display preparation is smaller, but can leave an inactive VDD unavailable during encoder probing.
- Leaving early upstream mode changes and correcting them after ANNOUNCE causes an unnecessary mode switch and still temporarily treats the platform as authoritative.
- The selected approach keeps early device preparation and defers only the mode-dependent work.

## Validation

Add GTest coverage through the real display-configuration parser for pending modes, conflicting platform/client values, automatic 4K/120 Hz, fractional rates, disabled/manual policies, remapping, and regular HTTP launches. Build Sunshine and `test_sunshine` with MSYS2 UCRT64, run the RTSP/display suites, and check formatting and patch whitespace. Live monitor switching requires a Moonlight connection and a supported display mode; unit tests must not change the workstation's current mode.

## Repository constraints

- Build through `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.
- Build directory names start with `cmake-build-`; tests are `tests/test_sunshine`.
- Follow `.clang-format`; localization changes are limited to `en`.
- Preserve unrelated working-tree changes.
