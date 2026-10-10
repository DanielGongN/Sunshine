# SendInput Mouse Motion Design

## Goal

Preserve the trajectory of streamed Windows desktop mouse movement when Sunshine uses `SendInput`, without adding a fixed wait to the input path.

## Scope

Only the Windows `SendInput` mouse backend changes. Other input backends, keyboard, buttons, wheels, touch, pen, and gamepad dispatch retain their current behavior. Cursor capture and video rendering are separate from this change.

## Design

The platform input context reports whether mouse movement uses `SendInput`. For that backend, the common input queue does not sum relative mouse deltas or discard intermediate absolute positions. A different packet type remains an ordering barrier.

The existing single drain worker sends movement packets immediately when fewer than eight consecutive relative movement packets are queued. If eight or more are already queued, it removes at most 32 consecutive relative movement packets and submits an `INPUT` array to `SendInput`. Every array element keeps the original `dx` and `dy`, and the call does not wait to fill the array. Absolute movement stays on its existing conversion path and is dispatched individually.

The batch sender handles partial insertion by continuing with the unsent suffix. On zero progress it retains the existing input-desktop retry behavior, then logs the inserted count and Windows error if no retry is possible. It never replays an event already reported as inserted.

Aggregate input statistics count each original packet as processed, and record the number of multi-event SendInput calls and failures. No per-packet info logging is added.

## Verification

Unit tests cover exact relative delta order, direction reversals, the eight-event threshold, the 32-event cap, and keyboard or mouse-button barriers. Build with MSYS2 UCRT64 and run `tests/test_sunshine`. A manual streamed-session comparison checks desktop pointer motion and input latency; client-only visual trails are evaluated separately in the capture and display path.
