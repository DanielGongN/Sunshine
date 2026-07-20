# Extreme Input Latency Optimization Design

## Goal

Reduce local input latency after network delay is excluded, with emphasis on mouse and gamepad input under high event rates.

## Approach

Use a two-tier change.

1. Apply safe hot-path fixes unconditionally:
   - Fix relative mouse and scroll batching so normal non-overflowing deltas are merged.
   - Avoid copying queued input packets while scanning for batch candidates.
   - Keep the existing single active drain task per input context.
2. Add an opt-in low-latency input mode:
   - Keep default behavior compatible.
   - When enabled, disable the 10 ms absolute-mouse left-button release guard.
   - Drain more queued input messages per worker activation.
   - Reschedule continued backlog work through the zero-delay timer path so other task-pool work is not starved.

## Behavior

Keyboard press/release ordering remains unchanged and is not batched.

Gamepad button edges remain unbatched. Stick, trigger, touch, motion, mouse move, and scroll updates may collapse to the latest safe state when multiple packets are already queued.

The low-latency mode trades a desktop/browser compatibility guard for lower mouse release latency. It is off by default.

## Testing

Build with the MSYS2 UCRT64 command prefix from `AGENTS.md`.

Run `test_sunshine` from the selected CMake build directory when practical. At minimum, compile the touched target and report any runtime-test limitation.
