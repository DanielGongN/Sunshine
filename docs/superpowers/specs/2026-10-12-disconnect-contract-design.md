# Disconnect reporting and upstream termination

The user requested this contract on 2026-10-12 and clarified that upstream-triggered disconnections must not be echoed upstream, except that types 1/2 must retain normal disconnected reporting. Private termination codes belong to the client control protocol.

## Outgoing contract

- Keep internal detailed disconnect reasons for first-cause ownership, encoder recovery decisions, and diagnostics. Serialize explicit client stop as `data.type=0`, force/standby reminders as `1/2`, and all other Sunshine-originated stop reasons as `99`.
- Preserve the existing detailed message and one outgoing notification per incoming `client_connect` cycle. Unknown peer disconnects and timeouts are abnormal (`99`), since they do not prove an intentional client exit.

## Incoming termination

- Re-enable the middleware `disconnected` handler. Parse the incoming command and queue a typed request for the control thread. Incoming types 1/2 attempt the usual outgoing disconnected notification with the same type and message before queuing; the existing cycle latch suppresses duplicates when a local timeout reminder already reported. Other incoming types consume the latch without reporting. Stop the session with `notify_upstream=false` so even an intervening client-connect reset cannot make session cleanup echo the command upstream.
- Numeric `data.type=0` selects graceful termination code `0x80030023`; `1` selects private force-timeout code `0x8004E001`; `2` selects private standby-timeout code `0x8004E002`; `3` selects private game-process-exit code `0x8004E003`. Other or omitted numeric types retain the existing graceful fallback. Reject malformed data/type fields without consuming the notification latch.
- Preserve the upstream message where present; otherwise provide a meaningful default. Process requests in arrival order. The first request stops all running sessions; later commands cannot replace an already selected termination code.
- Store the selected code in each session's control state. Initial termination, retry on send failure, and shutdown cleanup all use that stored code and the existing send-once guard. Never replace a private termination code with the graceful code during cleanup.
- Reset notification suppression only on a new `client_connect`, as before.

## Agent evidence and limits

At the initial inspection, `E:/work/rust/agent/src/logics/events/listen_process.rs` sent `disconnected` with `type=3` only through `upstream_tx` after game-process exit. This path is not a report of the agent service itself crashing, and it can include ordinary game exits. The follow-up agent fix on 2026-10-12 adds direct local broadcast of the same type-3 envelope for terminal game exits and failed recovery; upstream reporting remains independent. Successful recovery retains the connection. See the agent repository's `docs/superpowers/specs/2026-10-12-local-process-exit-design.md`.

If the remote platform sends a `disconnected` command back, `upstream_client.rs` dispatches it to `handlers/events/disconnected.rs`; the handler posts it to the main consumer, which broadcasts it through `SessionManager`. `disconnected` is broadcast to all local sessions, and Sunshine subscribes to it. The remote platform's implementation is not present here, so its decision to send the command back cannot be established from these repositories. Direct agent delivery now uses the same consumer and removes that dependency for local game-process termination.

The bundled Moonlight control reader passes unrecognized 32-bit termination codes through unchanged. `0x8004E001`, `0x8004E002`, and `0x8004E003` are private extensions, distinct from the existing graceful code and its success normalization. A client-specific friendly message still depends on that client's UI.

## Validation

Test grouped outgoing types, retained messages, incoming type-0/1/2/3/generic requests, timeout reporting with and without a preceding local reminder, suppression of other command echoes, malformed requests preserving the latch, repeated-command ordering, and next-cycle reset. Retain stream first-cause, encoder recovery, route, and control regressions. Build both executables with MSYS2 UCRT64 and follow `.clang-format`. No agent files or live upstream state are modified.
