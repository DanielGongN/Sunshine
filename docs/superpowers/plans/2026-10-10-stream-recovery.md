# Stream Recovery Implementation Plan

> Execute inline in this session, as requested by the user. Track each task below.

**Goal:** Make repeated short outages recover predictably and leave terminal disconnects diagnosable and ready for a fresh client handshake.

**Architecture:** A small recovery state object supplies atomic media gating and deterministic deadlines. A synchronized route object supplies endpoint snapshots and exact-token rebinding; gateway route generations are isolated per channel. Existing session stop/notification semantics remain terminal and deduplicated.

**Tech Stack:** C++23, Boost.Asio, ENet, GTest, MSYS2 UCRT64.

**Spec:** `docs/superpowers/specs/2026-10-10-stream-recovery-design.md`

## Global Constraints

- Default application recovery deadline: 10 seconds; ENet margin: 2 seconds.
- Recovery wait begins after 1 second without valid control.
- No revival of an ENet peer after disconnect.
- `.clang-format`; MSYS2 UCRT64; build directory `cmake-build-codex-tests`; tests `tests/test_sunshine.exe`.
- Preserve unrelated working-tree changes; localization only `en`.

## Task 1: Recovery state and transport diagnostics

Files: create `src/stream_recovery.h`, `tests/unit/test_stream_recovery.cpp`; modify `src/stream.cpp`, `cmake/compile_definitions/common.cmake`.

- [ ] Add deterministic tests: `start(t)`, `receive(t)`, `poll(t, timeout)` transition through healthy/waiting/expired; repeat 50 five-second outages; no receive revives a stopped state. Test ENet timeout budget is 12000 ms for a 10000 ms application deadline.
- [ ] Implement the state with a mutex for timestamps and an atomic media gate; use `enet_peer_timeout(peer, ENET_PEER_TIMEOUT_LIMIT, budget, budget)` on connection establishment.
- [ ] Refresh control liveness only after successful processing, bound control packet reads, gate outgoing media, reset input on entering wait, request IDR on recovery, and snapshot ENet diagnostics before servicing events.
- [ ] Record observed wire disconnect commands without changing packet handling or inferring user intent.

## Task 2: Media routes and gateway queues

Files: create `src/stream_route.h`, `tests/unit/test_stream_route.cpp`; modify `src/stream.cpp`, `src/gateway.h`, `src/gateway.cpp`, `tests/unit/test_gateway.cpp`.

- [ ] Test exact token plus same-IP port rebinding, wrong token/address rejection, terminal stop rejection, and gateway routes retaining their internal destination.
- [ ] Retain weak route registrations in the receiver and publish endpoint/source snapshots to the media sender. Unregister by identity so old cleanup cannot erase a replacement route.
- [ ] Replace shared gateway generation with per-channel generations and a global clear epoch. Test changing audio leaves control current; clear invalidates both; returning to an old port does not revive old packets.
- [ ] Suppress and clear queued gateway media while waiting; restore dispatch only after recovery.

## Task 3: Microphone and session handoff

Files: modify `src/client_mic.h`, `src/client_mic.cpp`, `src/stream.cpp`, `src/rtsp.h`, `src/rtsp.cpp`, `src/nvhttp.cpp`, `tests/unit/test_client_mic.cpp`.

- [ ] Inject a transient suspension predicate into microphone receive; while suspended silence the sink, clear decoder/sequence state, and discard packets without terminating the worker. Test resume with a reset sequence.
- [ ] Serialize last-session callbacks against startup before publishing RUNNING.
- [ ] Make pending-launch admission return failure for a conflict and propagate it through NVHTTP. Consume cached middleware configuration only after launch preparation succeeds.

## Task 4: Build, regression, and handoff

Files: modify `docs/stream-disconnect-notifications.md`; create ignored verification script in `cmake-build-codex-tests`.

- [ ] Format changed C++ and build `test_sunshine sunshine --parallel 4` through the required MSYS2 UCRT64 launcher.
- [ ] Run `StreamRecovery*:StreamRoute*:StreamPing*:StreamDisconnect*:Gateway*:ClientMic*:Middleware*` and relevant config/locale checks; compare known baseline documentation failures.
- [ ] Review diff and source lifetimes; document exact validation and client/hardware limits; provide the built `ws_engine.exe`.
