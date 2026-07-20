# Gamepad Input Latency Optimization Design

## Scope

Optimize Sunshine's gamepad input receive path without changing the GameStream protocol or adding a dedicated input thread. The change targets two latency sources found in `src/input.cpp`:

- blocking sleeps on the global single-worker `task_pool`
- one task being queued for every input packet

## Chosen Approach

Use a focused two-part change:

1. Convert synthetic gamepad actions to non-blocking delayed release tasks. Back-to-Home emulation and `click_gamepad()` should submit the press immediately, then schedule the release with `task_pool.pushDelayed()` instead of sleeping inside a task.
2. Keep one active input drain task per input context. Incoming packets append to `input_queue`; if no drain is active, schedule one drain task. The drain task processes queued packets in order and reschedules itself only when more work remains.

This keeps the existing single-worker ordering model, avoids platform API concurrency changes, and removes obvious 100-200 ms stalls from the input task path.

## Data Flow

Control stream input data continues to call `input::passthrough(input, data)`.

`passthrough()` appends the data to `input_queue`, records stats, and schedules a drain task only when one is not already scheduled or running.

The drain task pops and batches one queued packet at a time, sends it to the platform backend, and loops while work remains. When the queue is empty, it clears the active-drain flag.

## Behavior

Normal gamepad packets retain the existing same-state filtering before `platf::gamepad_update()`.

`click_gamepad()` still emits one press and one release, with the same hold duration, but the hold no longer blocks unrelated input processing.

Back-to-Home emulation still forces Back up, sends Home down, and sends Home up after the existing hold duration. The delay is represented as a timer task instead of a blocking sleep.

## Error Handling

If a controller is disconnected before a delayed release fires, the delayed task should check that the controller slot and gamepad id are still valid before sending a release.

Existing warnings for unallocated controller slots remain unchanged.

## Testing

Build with the MSYS2 UCRT64 command prefix from `AGENTS.md`.

Run the existing `test_sunshine` executable from the CMake build directory. If a full test run is not practical, at minimum compile the touched target and report why runtime tests were not executed.

Manual verification should inspect logs for lower `queue_peak`, absence of 100-200 ms stalls from synthetic actions, and no repeated gamepad press events for unchanged button state.
