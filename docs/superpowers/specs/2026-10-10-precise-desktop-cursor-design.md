# Precise desktop cursor on Windows

## Goal

Provide an opt-in desktop cursor mode for the Windows SendInput backend that maps incoming relative Moonlight mouse deltas to absolute virtual-desktop positions. The default relative mode and the Interception backend retain their existing behavior.

## Behavior

- Expose `desktop_mouse_absolute` as a Windows input option, disabled by default. Enable it in this checkout's `engine.conf` for the requested SendInput trial.
- On the first relative packet, after a stream reset, or when virtual-desktop bounds change, seed the logical cursor position from `GetCursorPos`.
- Accumulate each relative delta in a floating-point logical position, clamp to the virtual desktop, normalize to `0..65535`, and inject `MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK` through the existing `send_inputs` function.
- Preserve event order in the existing input queue. Batched SendInput movement remains a sequence of distinct absolute events; the deltas are never summed.
- Re-sync from `GetCursorPos` after an idle gap so local mouse movement between remote inputs is respected. An incoming absolute movement invalidates the accumulated relative target.
- On cursor-query or injection failure, reset the logical target and use the existing relative SendInput path for that movement where safe. Retry the absolute path after a one-second cooldown to avoid repeated system calls and log spam.
- Do not change keyboard, button, wheel, Interception, or non-Windows paths.

## Validation

Unit-test the pure coordinate mapper for accumulation, virtual-desktop offsets, clamping, normalization, idle re-sync, and display changes. Build `test_sunshine` with MSYS2 UCRT64 and run the focused mapper tests. Manually compare host cursor displacement for slow and fast movement with the option off and on; evaluate stream image latency separately.
