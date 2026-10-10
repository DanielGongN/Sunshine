# Optional Audio Handshake Implementation Plan

> **For agentic workers:** Execute this focused change inline in the existing shared checkout. Read the spec and preserve unrelated local changes.

**Goal:** Keep video streaming while audio or a negotiated microphone is absent, and accept their PINGs later.

**Architecture:** Factor the initial-PING wait into a transport-independent function with an optional deadline and cancellation callback. Retain routing registrations for optional channels and make their workers non-fatal to the session.

**Tech Stack:** C++23, Boost.Asio, gtest, CMake/Ninja, MSYS2 UCRT64.

**Spec:** `docs/superpowers/specs/2026-10-10-optional-audio-handshake-design.md`

## Global Constraints

- Missing microphone PING must not stop the streaming session.
- Ordinary audio initial PING is also optional.
- Video startup timeout and control connection health retain existing stop behavior.
- A pending optional handshake must use queue waits of at most 100 ms and check cancellation between waits.
- Keep existing microphone negotiation, PING validation, gateway framing, and encryption requirements.
- Use the repository `.clang-format`; do not edit localization.
- Prefix Windows shell commands with `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.

## Task 1: Cancellable media handshake and worker isolation

**Files:**
- Create `src/stream_ping.h`: declare the wait result and shared wait function.
- Modify `src/stream.cpp`: implement the wait, integrate queue validation, and isolate optional worker failures.
- Create `tests/unit/test_stream_ping.cpp`: exercise deadlines, late activation, invalid traffic, and shutdown.
- Modify `docs/stream-disconnect-notifications.md` and `disconnect-types.md`: document required video versus optional audio/microphone startup.

**Interface:**

```cpp
enum class initial_ping_result_e {
  received,
  timeout,
  cancelled,
};

initial_ping_result_e wait_for_initial_ping(
  std::optional<std::chrono::milliseconds> timeout,
  const std::function<bool()> &cancelled,
  const std::function<bool(std::chrono::milliseconds)> &receive_ping,
  const std::function<std::chrono::steady_clock::time_point()> &now = std::chrono::steady_clock::now
);
```

- [x] Implement the wait using `steady_clock`, a finite deadline when provided, and queue waits of at most `100ms`. Return `cancelled` when shutdown is requested, including when it races with a valid PING. Only return `timeout` for a provided deadline.
- [x] Refactor `recv_ping` to call the shared wait. Its receive callback pops one message, retains existing diagnostic logging and validation, and updates the peer only for a matching PING. Only a timeout result calls `session::stop(...stream_start_timeout...)`.
- [x] Keep the video call's configured timeout and change the audio and microphone calls to `std::nullopt`. Remove their fatal worker exit guards and replace exception stop calls with local error logs.
- [x] Add gtests with controlled elapsed time and receive callbacks: required timeout, valid required PING, late microphone/audio beyond a simulated 10-second boundary, invalid PING followed by valid PING, no-PING cancellation, continuous-invalid-traffic cancellation, and shutdown racing with a valid PING. Add a real queue wait cancellation test to cover the polling interval without sleeping for the former 10-second timeout.
- [x] Update type 21 descriptions to name video/control startup only and document optional-channel activation and teardown.
- [x] Format and build:

```sh
/e/WorkComp/LLVM/bin/clang-format.exe -i src/stream_ping.h src/stream.cpp tests/unit/test_stream_ping.cpp
cmake --build cmake-build-codex-tests --target test_sunshine sunshine -j 2
./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_filter=StreamPingTests.*:StreamDisconnectTests.*:StreamTimeoutNotificationTests.*:ClientMicConfigTests.*:VideoSourceAddressTests.*
```

- [x] Run the full suite once, distinguish any existing unrelated failures, inspect the focused diff, and report the verified behavior and remaining negotiation requirement.

## Validation results

- `sunshine` and `test_sunshine` built successfully in `cmake-build-codex-tests` with MSYS2 UCRT64.
- 34 focused tests passed, including 10 new initial-PING wait tests and the existing disconnect, timeout, microphone configuration, and video source-address tests.
- Full suite: 419 tests, 410 passed, 8 skipped, 1 failed. The existing `ConfigConsistencyTest.AllConfigOptionsExistInAllFiles` failure reports six middleware configuration options absent from UI/documentation/localization; this change does not edit those configuration input files.
- Clang-format and focused whitespace checks passed.
- Focused and full test reports are saved under `cmake-build-codex-tests/optional-audio-*-tests.xml`; the full output is in `cmake-build-codex-tests/optional-audio-full-tests.log`.
- Actual client hardware insertion was not exercised. A late microphone still requires the existing microphone negotiation and a matching PING.
