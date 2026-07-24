# Low-Latency Mouse Batching Design

## Goal

Reduce mouse motion latency in low-latency input mode. The current input queue can merge multiple mouse move packets into one host input event, which can make movement feel like short jumps when packets accumulate.

## Chosen Approach

When `low_latency_input` is enabled, do not batch relative or absolute mouse movement packets. Each queued mouse move packet should be processed as its own host input event.

Default mode keeps the existing batching behavior for compatibility and lower system-call volume. Scroll, touch, pen, controller touch, controller motion, and gamepad batching remain unchanged.

## Data Flow

Control stream input continues to enqueue packets through `input::passthrough()`.

The input drain task still pops packets in order. During the batching scan, mouse movement packets return `terminate_batch` when low-latency mode is active, so later mouse movement packets remain queued and are processed individually by the same drain loop.

## Error Handling

No new error paths are introduced. The existing input queue, logging, and platform dispatch behavior remain unchanged.

## Testing

Build with the MSYS2 UCRT64 command prefix from `AGENTS.md`.

Run the focused `test_sunshine` target when practical. Manual verification should enable `low_latency_input`, move the mouse in a streamed session, and confirm motion feels continuous rather than batched into short jumps.
