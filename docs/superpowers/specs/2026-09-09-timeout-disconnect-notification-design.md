# Timeout Disconnect Notification Design

## Goal

Detect two different inactivity scenarios per streaming session and notify the
upstream middleware without immediately disconnecting Moonlight:

- `force_disconnected_time`: a keyboard key, mouse button, or stable gamepad
  control combination has remained held for too long.
- `standby_disconnected_time`: no real user operation is active or received for
  too long.

The upstream service remains responsible for sending the existing
`disconnected` command that causes Sunshine to terminate the client session.

## Timeout Semantics

### Force disconnect time

`force_disconnected_time` is a continuous-hold limit, expressed in seconds:

- Start measuring when the current held-control combination changes from empty
  to non-empty.
- Restart the timer whenever that combination changes.
- Stop the timer when every tracked control returns to its released or neutral
  state.
- A value of `0` disables the timeout.
- When the limit is reached, report a `disconnected` event with `type=1`.

The held-control combination includes real client input from:

- Keyboard keys.
- Mouse buttons.
- Gamepad digital buttons, triggers, and stick directions, separated by client
  controller number.

Keyboard auto-repeat packets and repeated unchanged controller packets do not
change the combination or restart the timer. Synthetic server-generated input,
including middleware gamepad clicks, is excluded.

### Standby disconnect time

`standby_disconnected_time` is a per-session idle limit, expressed in seconds:

- Initialize the last-activity time when the stream session enters `RUNNING`.
- Reset it when a real user action is processed for that session.
- Pause standby detection while any tracked control is held, even when force
  detection is disabled.
- When all tracked controls are released, restart standby timing from the
  release time.
- A value of `0` disables the timeout.
- When the idle limit is reached, report a `disconnected` event with `type=2`.

Force and standby are mutually exclusive at any instant: a non-empty held
combination is eligible only for force detection, while an empty combination is
eligible only for standby detection.

## Stable Gamepad Classification

Raw gamepad analog values are not compared exactly because hardware noise would
continually restart the force timer. Each session instead maintains a stable
classification for every controller:

- Digital button flags are tracked exactly.
- A trigger enters the held state at value `30` and returns to released at value
  `20` or below. Values in between retain the previous classification.
- Each stick axis enters a positive or negative held direction at absolute value
  `8000` and returns to neutral at absolute value `6000` or below. Values in
  between retain the previous classification.
- Crossing between positive, neutral, and negative changes the held-control
  combination and restarts force timing.

This hysteresis makes small stick and trigger fluctuations irrelevant while
still treating a deliberate direction change or release as a new combination.

## Per-Session State and Data Flow

Each `input_t` owns:

- Its current keyboard-key set.
- Its current mouse-button set.
- Stable control classifications for its gamepads.
- A held-control signature derived from those sets and classifications.
- Whether force hold detection is active and when the current signature began.
- The timestamp of the last real user activity used by standby detection.

The per-session input queue processes accepted input changes in order. After
packet validation it updates the corresponding held-control state, rebuilds the
signature, and performs one of these transitions:

1. Empty to non-empty: activate force timing at the current time.
2. Non-empty to a different non-empty signature: restart force timing.
3. Non-empty to empty: deactivate force timing and restart standby timing.
4. Unchanged signature: preserve the original force start time.

Mouse movement, scrolling, text, touch, pen, and controller touch update the
standby activity timestamp but do not enter the held-control signature. While a
held signature exists, these events do not restart force timing.

The control thread reads a consistent `active + held_since + last_activity`
snapshot under synchronization. State must not use process-global key or mouse
collections for timeout decisions, because activity from one session must not
affect another session.

## Real User Activity

The following reset standby when no tracked control is held:

- Accepted keyboard press and release changes.
- Text input.
- Accepted mouse button press and release changes.
- Non-zero relative mouse movement and valid absolute mouse movement.
- Non-zero vertical or horizontal scrolling.
- Touchscreen and pen interaction.
- Controller touch interaction.
- Changed controller button, trigger, or stick state while the controller is
  active.

The following do not reset standby or create a force-held signature:

- Periodic control pings and loss statistics.
- Repeated keyboard, mouse-button, or controller state packets.
- Controller arrival by itself. Controller removal clears that controller's
  tracked held state and follows the normal combination-change transition.
- Controller battery reports.
- Controller accelerometer and gyroscope reports.
- Synthetic server-generated input.

## Notification Types

The inner `data.type` field describes the reason:

| Value | Meaning |
| --- | --- |
| `0` | Other disconnect reason |
| `1` | `force_disconnected_time` continuous-hold limit reached |
| `2` | `standby_disconnected_time` idle limit reached |

All existing non-timeout reports use `type=0`, including client-initiated
disconnects, network failures, ping timeouts, and upstream-requested
disconnects.

The force notification is wrapped using the existing middleware envelope:

```json
{
  "event": "send_to_upstream",
  "message_id": 123456789,
  "data": {
    "event": "disconnected",
    "message": "长时间挂机，等待上游断开连接",
    "data": {
      "type": 1
    }
  }
}
```

The standby form uses `type=2` and the message
`长时间无操作，等待上游断开连接`.

## Notification Lifecycle

Each session stores whether a timeout notification has already been sent.
After the first force or standby notification:

- Do not send another timeout notification for that session.
- Do not send a Moonlight termination control message.
- Do not call `session::stop()`.
- Leave the stream running while waiting for the upstream response.

When the upstream service subsequently sends the existing `disconnected`
command, Sunshine follows the current force-disconnect path:

1. Raise `mail::force_disconnect`.
2. Send the standard Moonlight graceful termination code `0x80030023`.
3. Stop and remove the running session.

## Runtime Configuration and Failure Behavior

Timeout values remain atomically readable because middleware events can update
them while sessions are running. A numeric zero disables the corresponding
timeout; malformed or negative values leave the current value unchanged.

When a runtime update lowers a timeout below the current eligible elapsed time,
the next control-loop iteration reports it. Increasing or disabling a timeout
affects subsequent checks unless the session has already sent its one timeout
notification.

The session latch is set when Sunshine attempts the first notification. The
notification is not retried if the middleware is disconnected; improving
middleware delivery guarantees remains outside this change.

## Testing

Automated tests cover:

- Keyboard keys, mouse buttons, gamepad buttons, triggers, and stick directions
  can each form a force-held signature.
- An unchanged non-empty signature reaches the force timeout and reports
  `type=1`.
- Adding, removing, or changing a held control restarts force timing.
- Releasing every held control stops force timing and restarts standby timing.
- Standby cannot fire while a force-held signature is active.
- Analog noise inside the hysteresis bands does not restart force timing.
- Deliberate analog direction changes and releases do restart timing.
- Ordinary real input restarts standby without being accumulated as force.
- Periodic, sensor, battery, arrival, synthetic, and repeated unchanged events
  do not count as real activity.
- Timeout state is isolated between sessions.
- A timeout notification leaves Moonlight in `RUNNING` and is emitted only once.
- A later upstream `disconnected` command still terminates Moonlight.
- Non-timeout disconnect reports use `type=0`.
- Zero timeout values disable their respective checks.

## Out of Scope

- Changing the upstream service's response behavior.
- Adding custom Moonlight termination codes.
- Displaying the force or standby reason in the Moonlight UI.
- Retrying or persisting middleware notifications across WebSocket failures.
