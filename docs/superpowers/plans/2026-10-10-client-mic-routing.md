# Client Microphone Routing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Select a virtual microphone cable automatically and silence its recording endpoint when client microphone audio stops.

**Architecture:** Extract endpoint matching and active-writer accounting into small testable helpers. Windows WASAPI owns a paired recording-endpoint mute gate and stops/resets its render buffer on silence. The receiver applies a 500 ms valid-audio idle timeout and checks cancellation before writes.

**Tech Stack:** C++23, Windows Core Audio/WASAPI, Opus, GTest, MSYS2 UCRT64, CMake/Ninja.

**Spec:** `docs/superpowers/specs/2026-10-10-client-mic-routing-design.md`

## Global Constraints

- Build through `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.
- Build directory names start with `cmake-build-`; tests are `tests/test_sunshine`.
- Follow `.clang-format`; localization changes are limited to `en`.
- Preserve unrelated working-tree changes.

### Task 1: Endpoint selection and gate ownership

**Files:** `src/client_mic_device.h`, `tests/unit/test_client_mic.cpp`.

**Interfaces:** Endpoint metadata contains ID, friendly name, description, and adapter. Selection returns playback/recording pairs. Gate ownership tracks active writers by recording-device ID.

- [x] Test VAC line matching, VB cable matching, ambiguous pairs, ordinary speakers, and stable ID ordering.
- [x] Implement the matching policy and active-writer counter.
- [x] Verify that releasing one writer does not close another writer's gate.

### Task 2: Windows discovery and mute lifecycle

**Files:** `src/platform/common.h`, `src/platform/windows/audio.cpp`, `src/rtsp.cpp`, `src/gateway.cpp`, `src/stream.cpp`.

**Interfaces:** The microphone sink gains `silence()` and a cancellation callback for `write()`. The platform factory retains its configured-name/ID argument; an empty value enables discovery.

- [x] Enumerate active render/capture endpoints and apply Task 1 matching.
- [x] For automatic mode, try candidate pairs in ID order until initialization succeeds; explicit failures retain clear errors.
- [x] Activate recording-endpoint volume control. Initialize muted, unmute on valid writes, mute on the final active writer's silence, and stop/reset the render client.
- [x] Remove nonempty-sink gates from capability advertisement and socket setup.

### Task 3: Receiver shutdown, idle, and resume

**Files:** `src/client_mic.h`, `src/client_mic.cpp`, `tests/unit/test_client_mic.cpp`.

**Interfaces:** Add a receiver overload accepting a sink factory for deterministic tests; normal callers keep the existing signature.

- [x] Recheck shutdown and packet-queue closure after waits and before writes.
- [x] Authenticate before PLC; only successfully decoded audio keeps the channel active.
- [x] Silence after 500 ms with no valid audio, reset sequence/decoder state, and resume on new valid packets.
- [x] Use a fake sink to verify idle, resume, queued-packet shutdown, queue closure, and failed authentication.

### Task 4: Documentation and verification

**Files:** `docs/configuration.md`, `src/config.h`, `src_assets/common/assets/web/public/assets/locale/en.json`.

- [x] Explain automatic selection, dedicated virtual recording endpoint, mute-on-close, and separate QQ playback routing.
- [x] Format changed C++ with `.clang-format`.
- [x] Build `test_sunshine` and Sunshine with the existing UCRT64 build configuration.
- [x] Run focused microphone, stream-ping, gateway, and configuration tests; inspect the final diff.

## Validation result

MSYS2/UCRT64 built ws_engine.exe and tests/test_sunshine.exe successfully. The focused run passed 46 of 47 tests, including all 16 ClientMic tests. ConfigConsistencyTest.AllConfigOptionsExistInAllFiles also fails against HEAD versions of its four input files with the identical 18 missing-documentation entries for six unrelated existing options. Driver/QQ behavior still requires a real cloud-host check.
