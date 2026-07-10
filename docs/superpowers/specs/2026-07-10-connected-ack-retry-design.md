# Connected Ack Retry Design

## Context

When a streaming client reaches the running state, Sunshine reports a `connected` event to the upstream middleware through `middleware::send_to_upstream()`. The current implementation sends an inner message shaped like:

```json
{
  "event": "connected",
  "type": 1
}
```

`send_to_upstream()` wraps that inner event in the upstream envelope:

```json
{
  "event": "send_to_upstream",
  "message_id": 1783665405,
  "data": {
    "event": "connected",
    "type": 1
  }
}
```

The upstream platform expects the inner `connected` event to carry its own `message_id` and `data` object:

```json
{
  "event": "connected",
  "message_id": 1783665405,
  "data": {
    "type": 1
  }
}
```

The upstream platform confirms successful consumption with:

```json
{
  "event": "consume_record_create_success",
  "message_id": 1783665405,
  "data": {
    "code": 0,
    "message": "success",
    "result": {
      "time": "2026-07-10T14:36:44.52+08:00"
    }
  }
}
```

Sunshine must keep sending the `connected` event until it receives a successful confirmation whose `message_id` matches the inner `connected.message_id`.

## Goals

- Send `connected` as an inner event with `event`, `message_id`, and `data.type`.
- Retry the same `connected` event every 30 seconds while it remains unconfirmed.
- Stop retrying only when `consume_record_create_success` has matching `message_id` and `data.code == 0`.
- Keep retry logic scoped to `connected`; do not change the behavior of `disconnected`, `operate_result`, or `client_heartbeat`.
- Preserve the existing outer `send_to_upstream` envelope.

## Non-Goals

- General-purpose acknowledgement handling for every upstream event.
- Changing heartbeat timing or payload shape.
- Changing upstream subscribe behavior.
- Changing localization files.

## Design

Add a small pending-confirmation state inside `src/middleware.cpp` for the `connected` event:

- `pending_connected_message_id`: the inner `connected.message_id` currently waiting for confirmation.
- `pending_connected_payload`: the full inner `connected` JSON object to resend.
- `connected_retry_timer`: an Asio steady timer on the middleware io_context.

`notify_client_connected()` will create the inner payload:

```json
{
  "event": "connected",
  "message_id": 1783665405,
  "data": {
    "type": 1
  }
}
```

It will queue the payload through `send_to_upstream()` and schedule a retry after 30 seconds. Retries resend the same inner payload, including the same inner `message_id`.

`on_message_event()` will handle `consume_record_create_success`:

- Ignore messages without a numeric `message_id`.
- Ignore messages where `message_id` does not match `pending_connected_message_id`.
- Ignore messages where `data.code` is absent or not `0`.
- On a match with `code == 0`, clear the pending state and cancel the retry timer.

If another client connection creates a new `connected` event before the previous one is confirmed, the newest connection replaces the pending state. This matches the existing one-global-middleware model and avoids retrying stale connection records after a newer session starts.

## Data Flow

1. Stream reaches running state and calls `middleware::notify_client_connected()`.
2. Middleware builds the inner `connected` event with its own `message_id`.
3. Middleware stores that inner event as pending and sends it via `send_to_upstream()`.
4. `send_to_upstream()` wraps it in the existing `send_to_upstream` envelope.
5. If no success confirmation arrives after 30 seconds, middleware resends the same inner event.
6. Upstream sends `consume_record_create_success`.
7. Middleware matches `message_id` and requires `data.code == 0`.
8. Middleware clears the pending state and stops retrying.

## Error Handling

- Malformed JSON is already handled by the read loop and logged.
- Unknown events continue through the existing unknown-event logging path.
- `consume_record_create_success` with unmatched `message_id` is ignored.
- `consume_record_create_success` with nonzero or missing `code` is ignored and retry continues.
- WebSocket write failures keep the existing logging behavior.
- Middleware shutdown cancels the retry timer as part of object destruction.

## Testing

Add focused unit coverage where practical around pure helper logic:

- Creating a `connected` payload produces `event=connected`, numeric `message_id`, and `data.type=1`.
- Matching `consume_record_create_success` with `code=0` clears pending confirmation.
- Mismatched `message_id` does not clear pending confirmation.
- Matching `message_id` with nonzero `code` does not clear pending confirmation.

If direct unit coverage requires too much refactoring around the private middleware implementation, verify with the existing build/test executable and keep the production change narrowly scoped.
