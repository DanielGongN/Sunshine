# Moonlight Display Mode Implementation Plan

> **For agentic workers:** Execute inline, task-by-task, preserving the current shared checkout and its unrelated changes.

**Goal:** Make middleware-launched host display resolution and refresh rate follow Moonlight's negotiated stream mode.

**Architecture:** Mark middleware display modes as pending. Keep device preparation but suppress mode changes until RTSP ANNOUNCE supplies dimensions and the validated frame rate, then configure the first stream's display using the existing policy parser.

**Tech Stack:** C++23, Windows display-device library, GTest, MSYS2 UCRT64, CMake/Ninja.

**Spec:** `docs/superpowers/specs/2026-10-10-moonlight-display-mode-design.md`

## Global Constraints

- Build through `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.
- Build directory names start with `cmake-build-`; tests are `tests/test_sunshine`.
- Follow `.clang-format`; localization changes are limited to `en`.
- Preserve unrelated working-tree changes.

### Task 1: Pending and negotiated display modes

**Files:** `src/rtsp.h`, `src/rtsp.cpp`, `src/nvhttp.cpp`, `src/nvhttp.h`, `src/display_device.cpp`, `tests/unit/test_rtsp_display.cpp`, `docs/architecture/sunshine-middleware-gateway-architecture.md`.

**Interfaces:** Add `launch_session_t::display_mode_pending` and `launch_session_t::fps_x100`, both default-initialized. Add `rtsp_stream::detail::apply_client_display_mode(launch_session_t &, const video::config_t &)` to complete a pending launch mode from ANNOUNCE. The existing `display_device::parse_configuration` interface stays unchanged.

- [x] Add a regression fixture using `configuration_option=verify_only`, automatic resolution/refresh rate, and SOPS enabled. Verify a pending session containing 1920x1080/60 produces no resolution or refresh-rate request. Complete it with a Moonlight configuration containing 3840x2160/120 and verify the parser produces both requested settings.

```cpp
video::config_t client {};
client.width = 3840;
client.height = 2160;
client.framerate = 120;
rtsp_stream::detail::apply_client_display_mode(session, client);
const auto result = display_device::parse_configuration(video_config, session);
const auto &display = std::get<display_device::SingleDisplayConfiguration>(result);
EXPECT_EQ(display.m_resolution, (display_device::Resolution {3840, 2160}));
EXPECT_EQ(display.m_refresh_rate, (display_device::Rational {120, 1}));
```

- [x] Cover fractional 59.94 Hz, disabled display configuration, disabled mode settings, manual overrides, pending remapping, and ordinary nonpending launch parameters with the same parser fixture.
- [x] Confirm the new tests fail to build before the new mode fields/helper exist.
- [x] Stop reading upstream width/height/FPS in `nvhttp::start_stream_session`; set the pending marker instead.
- [x] Skip mode parsing/remapping while pending. Use `fps_x100/100` for automatic refresh rate when a validated fractional value is available.
- [x] Reject nonpositive ANNOUNCE dimensions and FPS before fractional-rate validation. After the existing admission check, complete a pending mode and call `configure_display` only if no stream is already running. Log the negotiated video mode for all admitted clients.
- [x] Update middleware API and architecture documentation to identify RTSP as the authoritative mode source.
- [x] Format changed C++ files, build `sunshine` and `test_sunshine` in `cmake-build-codex-tests`, and run `RtspDisplayTests.*`, `RtspLaunchTests.*`, and all display parser tests.
- [x] Review the final patch against the spec, verify unrelated edits are preserved, and record the build/test outcome below.

## Execution results

- The pre-implementation test compilation failed on the missing pending-mode fields and completion helper, as expected.
- Changed C++ files/ranges pass LLVM clang-format verification with the repository configuration.
- MSYS2 UCRT64 build passed for `sunshine` (output `cmake-build-codex-tests/ws_engine.exe`) and `test_sunshine`.
- All 164 tests across eight suites passed, including the ten new mode-policy regressions. Log: `cmake-build-codex-tests/tests/moonlight-display-tests.log`.
- The clean verification run writes coverage under `cmake-build-codex-tests/tests/moonlight-display-coverage`, avoiding stale coverage files from older binaries.
- Final patch whitespace checks passed; existing unrelated changes remain intact.
- No live Moonlight connection or physical/VDD mode switch was performed during verification.
