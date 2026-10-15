# Local timeout disconnect implementation plan

> Execute inline in the current checkout, preserving existing user changes. The user explicitly requested the direct-disconnect design.

**Goal:** Stop expired local sessions immediately with distinct client codes while reporting matching timeout types to the platform.

**Architecture:** The control thread selects the existing timeout, stores and sends its client termination code, then invokes `session::stop` with the corresponding timeout reason. Existing cleanup and middleware deduplication remain responsible for retries and exactly-once reporting.

**Tech Stack:** C++23, Boost/ENet, gtest, CMake/Ninja, MSYS2 UCRT64.

**Spec:** `docs/superpowers/specs/2026-10-13-local-timeout-disconnect-design.md`.

## Global constraints

- Prefix shell commands with `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.
- Use `cmake-build-codex-tests`; the test executable is `tests/test_sunshine.exe` within that directory.
- Follow `.clang-format`; preserve unrelated changes and existing first-cause/report deduplication.
- Timeout type 1 uses `0x8004E001`; timeout type 2 uses `0x8004E002`.

## Task 1: Terminate on local timeout and verify the protocol contract

**Files:** Modify `src/stream.cpp`, `docs/stream-disconnect-notifications.md`, and `disconnect-types.md`.

**Interfaces:** Consume `select_timeout_notification`, `send_termination_msg(session_t *, std::string_view)`, and `session::stop(session_t &, disconnect_notification_type_e, std::string_view, bool)`. Preserve all signatures.

- [x] Set `session->control.termination_code` to `CONTROL_TERMINATION_FORCE_TIMEOUT` or `CONTROL_TERMINATION_STANDBY_TIMEOUT` in the matching timeout branch.
- [x] Replace reminder-only notification with `send_termination_msg(session, reason)` followed by `session::stop(*session, matching_type, message)`, using `长时间挂机，断开连接` or `长时间无操作，断开连接`. Log `action=terminate_session` and the selected code.
- [x] Update protocol docs so local expiry directly terminates and reports; describe late replies and retained retry codes.
- [x] Format changed C++; check whitespace and inspect the semantic diff.
- [x] Build `test_sunshine` and `sunshine` with `cmake --build cmake-build-codex-tests --target test_sunshine sunshine -j 2`.
- [x] Run `StreamTimeoutNotificationTests.*:StreamDisconnectTests.*:MiddlewareDisconnectTests.*:MiddlewareDisconnectCycleTests.*`; inspect all failures and record results.
- [x] Run the full suite once; distinguish any known pre-existing configuration consistency failure from new regressions.

## Validation results

- Both UCRT64 targets built successfully; the service output is `cmake-build-codex-tests/ws_engine.exe`.
- All 32 focused tests passed; XML: `cmake-build-codex-tests/local-timeout-focused-tests.xml`.
- Full suite: 484 tests, 475 passed, 8 skipped, 1 failed. `ConfigConsistencyTest.AllConfigOptionsExistInAllFiles` reports the same six missing middleware options in UI/documentation/localization as the pre-existing `optional-audio-full-tests.log` run on October 10. This change does not touch those files/options.
- Full results: `cmake-build-codex-tests/local-timeout-full-tests.log` and `.xml`.
- `clang-format --dry-run --Werror` and focused `git diff --check` passed. Code review confirms both timeout branches send before stopping, stop reports the matching reason, failed sends retain the chosen code for cleanup, and upstream commands skip stopped sessions.
- No new production abstraction or test hook was added solely to mirror the two existing timeout branches. Existing tests cover timeout selection and report/code/deduplication contracts; Android packet delivery has not been exercised with a live client.
