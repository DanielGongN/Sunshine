# Disconnected notification cycle

The user specified the behavior on 2026-10-10: after the first outgoing `disconnected` notification, suppress further notifications until a new upstream `client_connect` event arrives.

## Design

Use one process-wide atomic latch in middleware's common `notify_client_disconnected()` path. Both string overloads and every disconnect type, including force/standby reminders, share the latch. An atomic claim permits exactly one notification between resets even when multiple stream workers report concurrently. Keep the existing JSON payload and session-local first-cause tracking.

Reset at the beginning of the incoming `client_connect` handler, before validating the connection data, because receipt of this event defines the new cycle. Outgoing `connected` notifications, acknowledgments, stream cleanup, and middleware reconnection do not reset the latch. The first notification attempt consumes the latch even if middleware is unavailable; retain the existing drop/queue delivery behavior.

This supersedes the earlier advisory-event behavior: if type 1/2 is the first outgoing notification, the later terminal notification is suppressed until the next `client_connect`. Local stop handling and diagnostic logging continue normally.

## Validation

GTests inspect the real outgoing queue without running network I/O. Cover repeated disconnects, repeated client-connect resets, concurrent notifications, both public notification overloads sharing the latch, an outgoing `connected` notification not resetting it, and middleware availability not resetting it. Build `sunshine` and `test_sunshine` with MSYS2 UCRT64, then run middleware and existing stream/encoder regressions. Format the affected C++ files using `.clang-format` and preserve the preceding encoder fixes.
