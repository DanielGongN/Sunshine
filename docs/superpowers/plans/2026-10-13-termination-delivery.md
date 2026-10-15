# Reliable termination implementation plan

Execute inline in the shared checkout, preserving existing changes.

**Goal:** Give queued client termination codes a bounded opportunity for reliable delivery before releasing ENet and gateway ownership.

**Architecture:** A small per-session ENet termination state tracks a fixed 1000 ms drain deadline and requests deferred disconnect. The control thread keeps STOPPING sessions registered until the peer closes or that deadline expires; media shutdown and platform notification remain immediate.

**Tech Stack:** C++23, ENet, gtest, CMake/Ninja, MSYS2 UCRT64.

**Spec:** `docs/superpowers/specs/2026-10-13-termination-delivery-design.md`.

## Constraints

- Prefix commands with the repository's MSYS2 UCRT64 shell command.
- Use `cmake-build-codex-tests`; preserve first-cause, selected client code, and report deduplication.
- Apply `.clang-format`; no watchdog or agent changes.

## Task 1: Reproduce and fix reliable termination cleanup

**Files:** Create `src/stream_termination.h` and `tests/unit/test_stream_termination.cpp`; modify `src/stream.cpp` and the two current disconnect protocol documents.

- [x] Add a real ENet loopback fixture, with a client receive interceptor that can drop a chosen datagram. Verify `enet_peer_send`, flush, and immediate disconnect lose a termination when that first datagram is dropped.
- [x] Add `control_termination_t::poll(ENetPeer *, clock::time_point)` returning waiting/complete/timed_out, with an immutable 1000 ms deadline and deferred disconnect.
- [x] Test lost-first-packet recovery, early completion, bounded blackhole shutdown, and repeated sessions with exact force/standby/game-process-exit codes.
- [x] In STOPPING, queue the termination once before beginning the drain, service ENet without blocking, and retain the session until complete/timeout. Release mappings and clear the peer pointer before signaling controlEnd.
- [x] Drop application RECEIVE messages for non-running sessions; preserve transport ACK service. Log queued/draining/completed/timed-out accurately.
- [x] Update protocol docs, format, build test/service targets, and run targeted tests including middleware deduplication.
- [x] Run the complete test suite once and compare failures with the existing configuration consistency baseline.

## Validation

- Before the fix, the real immediate-disconnect control reproduced loss of the terminal packet: one deliberately dropped datagram, a client disconnect, and no received code. The immediate-close implementation failed the three new drain contract tests. XML: `cmake-build-codex-tests/termination-before-fix.xml`.
- After the fix, all five ENet tests passed, including reuse of the same host/peer slots for three consecutive connections and a lost first datagram on the third. Exact `0x8004E001` and `0x8004E002` payloads arrived before disconnect.
- All 43 targeted tests passed; XML: `cmake-build-codex-tests/termination-focused-tests.xml`.
- Full suite: 489 tests, 480 passed, 8 skipped, 1 pre-existing `ConfigConsistencyTest.AllConfigOptionsExistInAllFiles` failure for the same six missing middleware configuration options. Results: `cmake-build-codex-tests/termination-full-tests.log` and `.xml`.
- Both UCRT64 targets built successfully; output: `cmake-build-codex-tests/ws_engine.exe`. Formatting and focused whitespace checks passed.
- The root `tests/` ignore rule hides newly created tests. The new test was registered with `git add -N -f`, leaving its content unstaged and visible for review without changing ignore rules.
- Android UI behavior and packet delivery through the deployed gateway still require client-side validation. The logs establish the third local timeout and continued server activity, but cannot prove which datagram or client handling step caused the reported generic error.
- Follow-up coverage confirms all three private codes use the common production cleanup path. The loss test now runs separately for `0x8004E001`, `0x8004E002`, and `0x8004E003`; the repeated-connection test uses those three codes in order and drops the third connection's first termination datagram. All seven ENet cases and all 45 focused cases passed. XML: `cmake-build-codex-tests/termination-all-codes-tests.xml`. This follow-up changes tests only, so the existing service binary already contains the shared fix; the full-suite result above precedes this test expansion.
