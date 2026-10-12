# Disconnect Contract Implementation Plan

> Execute inline under the user's specified contract and clarification. Preserve the preceding encoder and notification-cycle changes.

**Goal:** Report Sunshine stops as 0/99, retain timeout reports as 1/2, and select distinct client termination codes for upstream commands.

**Architecture:** Map internal reasons when building JSON. Parse upstream disconnects into a typed control-thread queue, report timeout types or consume notification suppression before queuing, and retain a per-session client termination code through all send paths.

**Tech Stack:** C++23, GTest, Boost.Asio/Beast, ENet, MSYS2 UCRT64, CMake/Ninja.

**Spec:** `docs/superpowers/specs/2026-10-12-disconnect-contract-design.md`

## Global Constraints

- Explicit client stop serializes as 0; reminders retain 1/2; other Sunshine stop reasons serialize as 99.
- Incoming types 1/2 retain ordinary outgoing disconnected reporting, subject to the cycle latch. Other upstream commands produce no outgoing disconnected notification until the next client_connect.
- Incoming types 0/1/2/3 select client termination codes 0x80030023/0x8004E001/0x8004E002/0x8004E003 respectively. Other valid commands retain the graceful fallback.
- Keep internal reason distinctions and existing recovery/first-cause behavior.
- Keep agent source and live upstream connections unchanged.

## Task 1: Grouped reports and upstream command handling

Files: `src/middleware.cpp`, `src/middleware.h`, `src/stream_disconnect.h`, `tests/unit/test_middleware.cpp`.

- [x] Update outgoing payload expectations to 0/99 and add six tests for type-3/default parsing, malformed commands, no echo, queued command ordering, and reset.
- [x] Confirm the test-first failure before implementing the new command handler.
- [x] Add shared termination constants and `stream::detail::upstream_disconnect_t` containing a message and termination code.
- [x] Implement `detail::handle_upstream_disconnected(const json &)`: validate, select the code, claim the notification latch, and raise a typed queue item. Use it from the incoming event branch.
- [x] Map outgoing internal reasons to the new external type contract while preserving messages and enum distinctions.

## Task 2: Client termination and documentation

Files: `src/stream.cpp`, `src/stream.h`, `disconnect-types.md`, `docs/stream-disconnect-notifications.md`.

- [x] Consume `mail::force_disconnect` as a typed queue in controlBroadcast; select the per-session code before stopping running sessions.
- [x] Add the defaulted `notify_upstream` parameter to `session::stop`; upstream commands pass false so a concurrent client-connect reset cannot re-enable their report.
- [x] Make send_termination_msg use the stored session code; use the same path for stopping and broadcast shutdown so cleanup cannot change or duplicate a sent code.
- [x] Update type tables, upstream/no-echo behavior, private client code, and agent routing limitations.

## Task 3: Validation

- [x] Format affected C++ files and build sunshine and test_sunshine in cmake-build-codex-tests through MSYS2 UCRT64.
- [x] Run the grouped-report/upstream-command and existing middleware, stream, encoder, and adjacent regressions.
- [x] Review the final diff and report executable, results, and the unverified remote-platform return behavior.

## Initial contract validation results

- Test-first compilation failed as expected because the typed upstream request and client termination constants did not yet exist.
- Final MSYS2 UCRT64 builds of `sunshine` and `test_sunshine` completed successfully after adding the explicit no-report stop parameter. `clang-format --dry-run --Werror` passed for all six affected C++ files; the focused `git diff --check` passed.
- Final regression run on 2026-10-12 at 13:59:54: **91 tests passed, 0 failures, 0 errors**, including 12 notification-cycle/upstream-command cases. Results: `cmake-build-codex-tests/tests/disconnect-contract-tests.xml` and `disconnect-contract-tests.log` in the same directory.
- Executable: `cmake-build-codex-tests/ws_engine.exe` (68,437,192 bytes). SHA-256: `0F4A0D1FD02E5DBAA416F34A47EA6C3F2EB7A7655C3A5DF96EA611A9BA915331`.
- Existing profiling data emitted libgcov checksum warnings for rebuilt objects. No coverage measurements are claimed.
- Agent inspection was read-only. Its outbound process-exit notification does not directly broadcast locally; a returned platform command is forwarded to Sunshine. The remote platform's actual return behavior and live client display of the private error code were not verified by these queue-based tests.

## Task 4: Timeout termination codes and retained reporting

Files: `src/stream_disconnect.h`, `src/middleware.cpp`, `src/middleware.h`, `tests/unit/test_middleware.cpp`, and the two disconnect reference documents.

- [x] Add regression cases for explicit normal exit, independent force/standby client codes, outgoing timeout reports, and no duplicate after a preceding timeout reminder.
- [x] Verify the new cases fail against the previous command handler.
- [x] Add `CONTROL_TERMINATION_FORCE_TIMEOUT=0x8004E001` and `CONTROL_TERMINATION_STANDBY_TIMEOUT=0x8004E002`. Replace the incoming type-3 ternary with a 0/1/2/3 switch, retaining the existing default fallback and message precedence.
- [x] Before queuing, types 1/2 call `notify_client_disconnected` with their matching internal timeout type; all other types only consume the latch. Keep the control-thread stop's explicit no-report parameter.
- [x] Update reference documents, format affected C++ files, build both executables, and rerun the focused regression suite.

## Timeout extension validation results

- The test-first run caught the previous handler's generic type-0 message, graceful type-1 code, and suppressed timeout notification. The implemented switch now selects all four specified codes and preserves timeout reporting through the existing cycle gate.
- MSYS2 UCRT64 builds of `sunshine` and `test_sunshine` passed. `clang-format --dry-run --Werror` and the focused `git diff --check` passed.
- Final regression run on 2026-10-12 at 14:24:34: **95 tests passed, 0 failures, 0 errors**, including four new normal-exit/timeout-code/reporting cases and all earlier focused regressions. Results: `cmake-build-codex-tests/tests/disconnect-contract-tests.xml` and `disconnect-contract-tests.log`.
- Final executable: `cmake-build-codex-tests/ws_engine.exe` (68,437,740 bytes). SHA-256: `5B0CEDA75CC883C0EC8B4FFCDC35EE3D236DEAF5C7C8206E5B0323D2EAFF2BB5`.
- Existing libgcov checksum warnings remain limited to old profiling data; no coverage measurements or live platform/client verification are claimed.
