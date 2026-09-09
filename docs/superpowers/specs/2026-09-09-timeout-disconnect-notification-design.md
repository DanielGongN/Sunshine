# Timeout Disconnect Notification Design

## Goal

Separate the maximum session duration from user-idle detection and report the
result to the upstream middleware without immediately disconnecting Moonlight.
The upstream service remains responsible for sending the existing
`disconnected` command that causes Sunshine to terminate the client session.

## Timeout Semantics

### Force disconnect time

`force_disconnected_time` is an absolute session-duration limit:

- Interpret the configured value as seconds.
- Start measuring when the stream session enters `RUNNING`.
- Do not reset the deadline when user input is received.
- A value of `0` disables the timeout.
- When the deadline is reached, report a `disconnected` event with `type=1`.

### Standby disconnect time

`standby_disconnected_time` is a per-session idle limit:

- Interpret the configured value as seconds.
- Initialize the last-activity time when the stream session enters `RUNNING`.
- Reset it only after a real user action is processed for that session.
- A value of `0` disables the timeout.
- When the idle limit is reached, report a `disconnected` event with `type=2`.

If both timeouts become due during the same control-loop iteration, the force
timeout takes priority and only the force notification is sent.

## Notification Types

The `type` field in the inner `disconnected` event describes the reason:

| Value | Meaning |
| --- | --- |
| `0` | Other disconnect reason |
| `1` | `force_disconnected_time` reached |
| `2` | `standby_disconnected_time` reached |

All existing non-timeout reports use `type=0`, including client-initiated
disconnects, network failures, ping timeouts, and upstream-requested
disconnects.

The timeout notification is wrapped using the existing middleware envelope:

```json
{
  "event": "send_to_upstream",
  "message_id": 123456789,
  "data": {
    "event": "disconnected",
    "message": "长时间挂机，等待上游断开连接",
    "type": 1
  }
}
```

The standby form uses `type=2` and the message
`长时间无操作，等待上游断开连接`. Existing messages for non-timeout
disconnect reports remain unchanged.

## Timeout Notification Lifecycle

Each session stores whether a timeout notification has already been sent.
After the first force or standby notification:

- Do not send another timeout notification for that session.
- Do not send a Moonlight termination control message.
- Do not call `session::stop()`.
- Leave the stream running while waiting for the upstream response.

This latch is required because the control loop runs approximately every
millisecond. Without it, an expired session would continuously enqueue the same
notification.

When the upstream service subsequently sends the existing `disconnected`
command, Sunshine follows the current force-disconnect path:

1. Raise `mail::force_disconnect`.
2. Send the standard Moonlight graceful termination code `0x80030023`.
3. Stop and remove the running session.

The previously considered custom Moonlight termination codes are outside this
design. The timeout reason is communicated only through the upstream
notification's `type` field. Moonlight receives a generic graceful termination
when the upstream service orders the disconnect.

## Per-Session Timing State

The session owns its start timestamp and timeout-notification latch. The input
context owns its last-real-user-activity timestamp. This avoids the current
process-global input timestamp, where activity from one client can keep another
client session alive.

Timeout values remain atomically readable because middleware events can update
them while sessions are running. If an update lowers a timeout below elapsed
time, the next control-loop iteration reports it immediately. Increasing or
disabling a timeout affects subsequent checks, unless the session has already
sent its one timeout notification.

## Real User Activity

Only input that represents an effective user action resets the standby timer:

- Keyboard presses and releases.
- Text input.
- Mouse button events.
- Non-zero mouse movement.
- Non-zero vertical or horizontal scrolling.
- Touchscreen and pen interaction.
- Controller touch interaction.
- Controller button, trigger, or stick state changes while the controller is
  active.

The following do not reset the standby timer:

- Periodic control pings and loss statistics.
- Repeated controller packets whose button, trigger, and stick state is
  unchanged.
- Controller arrival or removal by itself.
- Controller battery reports.
- Controller accelerometer and gyroscope reports, because sensor noise can
  otherwise keep a session active indefinitely.
- Synthetic server-generated input such as middleware gamepad clicks.

The real-activity check occurs in the per-session input pipeline after packet
validation and before updating that session's last-activity timestamp.

## Failure and Retry Behavior

The session latch is set when Sunshine attempts the first timeout notification,
and Sunshine does not retry it. The existing middleware queue and heartbeat
drain behavior remains responsible for delivering an accepted message. If the
middleware is disconnected and the existing send path drops the notification,
the session remains running and the notification is not retried. Improving
middleware delivery guarantees is a separate concern and is not part of this
change.

Malformed or negative timeout events leave the current value unchanged. A
numeric zero disables the corresponding timeout, matching the current event
parsing behavior.

## Testing

Automated tests cover:

- Force time is measured from session start and is not reset by input.
- Standby time is measured from the last real input for the same session.
- Input in one session does not reset another session's standby time.
- Equal deadlines produce one force notification with `type=1`.
- A shorter standby deadline produces one standby notification with `type=2`.
- Repeated control-loop checks do not emit duplicate timeout notifications.
- Timeout notification leaves the Moonlight session in `RUNNING` state.
- The later upstream `disconnected` command still terminates the Moonlight
  session through the standard graceful termination path.
- Non-timeout disconnect reports use `type=0`.
- Zero timeout values disable their checks.
- Runtime timeout reductions take effect on the next check.
- Repeated unchanged controller packets and controller motion reports do not
  reset standby time.
- Actual keyboard, mouse, touch, pen, and changed controller state do reset
  standby time.

## Out of Scope

- Changing the upstream service's response behavior.
- Adding custom Moonlight termination codes.
- Displaying the force or standby reason in the Moonlight UI.
- Retrying or persisting middleware notifications across WebSocket failures.
