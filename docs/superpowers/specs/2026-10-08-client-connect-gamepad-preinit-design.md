# Client Connect Gamepad Preinitialization Design

**Date:** 2026-10-08  
**Status:** Approved

## Goal

When `middleware_gamepad_preinit` is true, create the persistent virtual gamepad after a successful middleware `client_connect` authorization, before acknowledging that event. Sunshine startup must not create the gamepad.

## Behavior

- Windows continues to probe ViGEm availability at startup, but does not attach a preinitialized gamepad then.
- A `client_connect` event with a valid UUID and certificate that `nvhttp::add_trusted_client` accepts triggers preinitialization when the option is enabled.
- The call is synchronous. The existing `operate_result` acknowledgment is sent after preinitialization finishes. Its `status` continues to report certificate authorization status; a ViGEm failure is logged separately.
- Invalid or rejected `client_connect` events do not trigger preinitialization. When the option is false, allocation stays on the normal input path.
- Repeated successful events reuse the one persistent slot and do not attach another target. If creation fails, a later successful event can retry.
- The preinitialized slot remains attached across client sessions and is removed during normal Sunshine input shutdown. Subsequent Moonlight input binds its feedback queue to that slot as before.

## Code boundaries

Keep event authorization and acknowledgment in `src/middleware.cpp`. Expose one input-layer entry point that requests preinitialization for the already initialized platform input context. Keep ViGEm target creation, state, and cleanup in `src/platform/windows/input.cpp`. Move the current startup allocation block into an idempotent Windows platform operation. Keep the existing startup probe and gamepad vector setup in `vigem_t::init()`.

The configured count remains `PREINITIALIZED_GAMEPADS` (currently 1 on Windows). Other platforms have no preinitialized slots.

## Failure and concurrency

Partial target creation must clean up the incomplete target and preserve correct allocation masks. A failed attempt leaves the slot eligible for retry. The event handler submits preinitialization to the existing single-worker input task pool and waits for its future before acknowledging the event. This keeps ViGEm target operations ordered with streamed input. The main shutdown path joins the middleware worker before stopping the task pool so the wait can finish. A repeated call after success is a no-op. If a normal session already owns the target slot, preinitialization leaves it intact and reports failure so a later event can retry.

## Verification

Build with MSYS2 UCRT64 in a `cmake-build-*` directory and run the relevant `tests/test_sunshine` suite. Inspect the event path and startup path to confirm the timing change. ViGEm hardware behavior requires a Windows host with the driver and a middleware event; record that limit if it cannot be exercised automatically.

## Scope

Do not change the user-modified `engine.conf`. Update current configuration and architecture documentation to describe the new timing. Do not modify localization files.
