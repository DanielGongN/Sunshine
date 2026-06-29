# Middleware Integration & Gamepad Pre-Initialization Design

**Date**: 2026-06-25
**Status**: Approved

## Overview

Add two features to Sunshine on Windows:

1. **Middleware connection**: WebSocket client connecting to a local middle-platform service (`0.0.0.0:40002`), subscribing to events for remote management.
2. **Gamepad pre-initialization**: Pre-create 2 virtual Xbox 360 gamepads via ViGEm at startup, so they are immediately available when Moonlight clients connect.

## Requirements

### Middleware Service Connection

- Connect to WebSocket at `0.0.0.0:40002` on startup
- Subscribe to 5 events: `force_disconnected_time`, `standby_disconnected_time`, `display_config`, `click_gamepad`, `disconnected`
- Handle received events per the `on_message_event` dispatch pattern
- Exponential backoff reconnection on disconnect (1s → 2s → 4s → ... → max 60s)
- Graceful shutdown on Sunshine exit

### Gamepad Pre-Initialization

- Create 2 virtual Xbox 360 gamepads at startup via ViGEm
- Wait for XInput to assign player slot indices (polling `vigem_target_x360_get_user_index`, max 10s per gamepad)
- Register rumble/notification callbacks
- Subsequent Moonlight client connections reuse these pre-created gamepads (gamepads 3+ fall through to dynamic allocation)

### Subscription Message Format

```json
{
    "message_id": "<millisecond-timestamp>",
    "event": "subscribe",
    "data": {
        "topic": "<event-name>"
    }
}
```

Recipient events use the same JSON envelope with `event` set to the topic name and `data` containing event-specific payload.

## Architecture

```
src/
├── main.cpp                          # Modified: start middleware thread
├── config.h / config.cpp             # Modified: add middleware_t config
├── middleware.h / middleware.cpp      # NEW: WebSocket client module
├── platform/
│   └── windows/
│       └── input.cpp                 # Modified: gamepad preinit in vigem_t::init()
```

### Module Responsibilities

| Module | Responsibility |
|--------|---------------|
| `config` | Parse/store middleware settings: enabled, address, port, gamepad_preinit |
| `middleware` | WebSocket connection lifecycle, event subscription, JSON parsing, event dispatch, exponential backoff reconnect |
| `input` (vigem_t) | Existing gamepad management + startup pre-creation of 2 Xbox 360 gamepads |
| `main` | Call `middleware::start()` after `input::init()`, store deinit guard for ordered shutdown |

## Data Flow

```
Middle-Platform Service (0.0.0.0:40002)
    │ WebSocket
    ▼
┌──────────────────────────────────────────┐
│  middleware module (own thread)           │
│                                          │
│  ┌────────────────────────────────────┐  │
│  │  WebSocket client (Boost.Beast)     │  │
│  │  - Connect / reconnect              │  │
│  │  - Send subscribe messages (5 topics)│  │
│  │  - Receive JSON events              │  │
│  └──────────────┬─────────────────────┘  │
│                 ▼                        │
│  ┌────────────────────────────────────┐  │
│  │  Event dispatcher                   │  │
│  │  on_message_event(Json)            │  │
│  │  - force_disconnected_time → config │  │
│  │  - standby_disconnected_time → cfg │  │
│  │  - display_config → video qp       │  │
│  │  - click_gamepad → virtual gamepad │  │
│  │  - disconnected → client quit      │  │
│  └────────────────────────────────────┘  │
└──────────────────────────────────────────┘
    │                   │
    ▼                   ▼
  Global state      Gamepad input
  (timeouts, qp)    (ViGEm virtual device)
```

## Configuration

### config.h — New struct

```cpp
struct middleware_t {
    bool enabled;              // default: false
    std::string address;       // default: "0.0.0.0"
    int port;                  // default: 40002
    bool gamepad_preinit;      // default: false
};
```

Added to `sunshine_t`.

### sunshine.conf — New entries

```ini
middleware_enabled = true
middleware_address = 0.0.0.0
middleware_port = 40002
middleware_gamepad_preinit = true
```

## Gamepad Pre-Initialization

### Implementation in `vigem_t::init()`

After existing ViGEm bus probe, if `config::sunshine.middleware.gamepad_preinit` is true:

1. Connect to ViGEm bus (if not already connected)
2. For slot 0 and 1:
   - `vigem_target_x360_alloc()` + `vigem_target_add()`
   - Poll `vigem_target_x360_get_user_index()` up to 40 retries × 250ms (10s)
   - `vigem_target_x360_register_notification()` with `x360_notify` callback
   - Mark `gamepadMask[slot] = true`
   - Initialize `XUSB_REPORT`

### Client Integration

Three changes needed in `src/input.cpp`:

**1. In `passthrough(PNV_MULTI_CONTROLLER_PACKET)` — reuse on connect:**
```cpp
// Before calling alloc_gamepad(), check if this is a pre-created slot
if (packet->controllerNumber < 2 && gamepadMask[packet->controllerNumber]) {
    // Reuse pre-created gamepad — no alloc_gamepad() needed
    input->gamepads[packet->controllerNumber].id = packet->controllerNumber;
}
```

**2. In `passthrough(PSS_CONTROLLER_ARRIVAL_PACKET)` — reuse on arrival:**
```cpp
// Before calling alloc_gamepad(), check if this is a pre-created slot
if (packet->controllerNumber < 2 && gamepadMask[packet->controllerNumber]) {
    input->gamepads[packet->controllerNumber].id = packet->controllerNumber;
    return;  // Pre-created gamepad already has metadata from vigem_t::init()
}
```

**3. In `free_gamepad()` — prevent destruction on client disconnect:**

This is critical. When a Moonlight client disconnects, `gamepad_t::~gamepad_t()` calls `free_gamepad()`, which calls `vigem_target_remove()`. Pre-created gamepads must survive client disconnects.

```cpp
void free_gamepad(platf::input_t &platf_input, int id) {
    platf::gamepad_update(platf_input, id, platf::gamepad_state_t {});  // Reset state to neutral

    // Pre-created gamepads are persistent — never remove them from ViGEm
    if (config::sunshine.middleware.gamepad_preinit && id < 2) {
        return;  // Skip vigem_target_remove() and free_id()
    }

    platf::free_gamepad(platf_input, id);
    free_id(gamepadMask, id);
}
```

**Result of client lifecycle:**

| Client Controller# | Slot | On Connect | On Disconnect |
|---|---|---|---|
| 0 | 0 | Reuse pre-created gamepad #0 | Reset state to neutral, keep ViGEm target alive |
| 1 | 1 | Reuse pre-created gamepad #1 | Reset state to neutral, keep ViGEm target alive |
| 2+ | 2+ | Dynamic `alloc_gamepad()` | Normal `vigem_target_remove()` + free |

Without check #3, a client disconnect would call `vigem_target_remove()` on the pre-created gamepad, destroying the ViGEm virtual device — and the next client connection would fail to find it.

## Startup Sequence

```
main()
 ├── config::parse()           ← parses middleware_* config
 ├── logging::init()
 ├── display_device::init()
 ├── nvprefs (Windows)
 ├── task_pool.start()
 ├── platf::init()
 ├── proc::init()
 ├── input::init()             ← includes gamepad preinit (2× X360)
 ├── input::probe_gamepads()
 ├── http::init()
 ├── middleware::start()        ← NEW: WebSocket connect + subscribe
 ├── mDNS / upnp (async)
 ├── httpThread, configThread, rtspThread
 ├── system_tray
 └── mainThreadLoop(shutdown_event)
```

## Shutdown Sequence

Deinit guards destruct in reverse declaration order:

1. `middleware_deinit_guard` → send disconnect ack → close WebSocket → join thread
2. `input_deinit_guard` → `~vigem_t()` → remove all gamepads → disconnect ViGEm bus

## Reconnection Strategy

| Retry | Delay |
|-------|-------|
| 1 | 1s |
| 2 | 2s |
| 3 | 4s |
| 4 | 8s |
| 5 | 16s |
| 6+ | 60s (cap) |

Counter resets on successful connection.

## WebSocket Implementation

- **Library**: Boost.Beast (Sunshine already depends on Boost)
- **Thread**: Dedicated `std::thread` running `boost::asio::io_context` event loop
- **JSON**: Use existing `jsoncpp` (already used in Sunshine for config/nvhttp)

## Build Changes

- `CMakeLists.txt`: Add `src/middleware.cpp` to sources
- Link Boost.Beast (header-only, no new library linkage required)

## Error Handling

| Scenario | Behavior |
|----------|----------|
| Middleware address unreachable | Log error, retry with backoff |
| WebSocket connection dropped | Retry with backoff |
| ViGEm not installed | Log fatal error, skip preinit, continue startup |
| Preinit gamepad add fails | Log error, clean up created gamepads, disconnect ViGEm, continue without preinit |
| click_gamepad received before gamepad ready | Log warning, drop event |

## Memory Safety

### Ownership Model

```
main.cpp (deinit guards — stack)
  │
  ├── middleware_deinit_guard (unique_ptr<deinit_t>)
  │     └── ~deinit_t() → io_context.stop() → thread.join() → socket.close()
  │
  └── input_deinit_guard (unique_ptr<deinit_t>)
        └── ~deinit_t() → platf_input.reset() → ~input_raw_t() → delete vigem
              └── ~vigem_t()
                    ├── for each gamepad with attached target:
                    │     └── vigem_target_remove() + gamepad.gp.reset() (safe_ptr RAII)
                    ├── vigem_disconnect()
                    └── client.reset() (safe_ptr RAII)
```

### Lifecycle Rules

| Object | Owner | Acquire | Release |
|--------|-------|---------|---------|
| `vigem_t` | `input_raw_t` (raw pointer) | `new` in `platf::input()` | `delete` in `freeInput()` |
| `vigem_t::client` | `vigem_t` (`client_t` = `safe_ptr`) | `vigem_alloc()` in `init()` | `safe_ptr` destructor calls `vigem_free()` |
| `gamepad_context_t::gp` | `gamepad_context_t` (`target_t` = `safe_ptr`) | `vigem_target_x360_alloc()` | `safe_ptr` destructor calls `vigem_target_free()` |
| `middleware_t` (impl) | `deinit_t` returned by `start()` | `new` in `start()` | `delete` in `~deinit_t()` |
| WebSocket thread | `middleware_t` | `std::thread(...)` in `start()` | `join()` in `~deinit_t()` |
| `io_context` | `middleware_t` | stack member | automatic destructor |
| JSON objects | stack (value semantics) | — | automatic destructor |

### Double-Free Prevention

All ViGEm resources use `util::safe_ptr<T, Deleter>`:
- `client_t` = `safe_ptr<_VIGEM_CLIENT_T, vigem_free>` — ensures `vigem_free()` called exactly once
- `target_t` = `safe_ptr<_VIGEM_TARGET_T, vigem_target_free>` — ensures `vigem_target_free()` called exactly once

Destruction ordering in `~vigem_t()` is critical:
1. First remove all targets from the bus (`vigem_target_remove`) — the bus holds internal references
2. Then reset each `target_t` (calls `vigem_target_free`) — safe since bus no longer references them
3. Then disconnect (`vigem_disconnect`) — no targets left on bus
4. Then reset `client_t` (calls `vigem_free`) — safe since bus is disconnected

### Partial Initialization Safety

Preinit failure handling (in `vigem_t::init()`):

```cpp
for (int slot = 0; slot < 2; ++slot) {
    // Step 1: alloc target
    gamepads[slot].gp.reset(vigem_target_x360_alloc());
    if (!gamepads[slot].gp) goto fail;

    // Step 2: add to bus
    err = vigem_target_add(client.get(), gamepads[slot].gp.get());
    if (VIGEM_FAILED(err)) goto fail;

    // Step 3: wait XInput
    // ... polling ...

    // Step 4: register notification (non-fatal, log warning on fail)
    vigem_target_x360_register_notification(...);

    gamepadMask[slot] = true;  // Mark occupied only after full success
}

fail:
    // slot failed — clean up all previously successful slots
    for (int j = 0; j < slot; ++j) {
        vigem_target_remove(client.get(), gamepads[j].gp.get());
        gamepads[j].gp.reset();  // calls vigem_target_free via safe_ptr
        gamepadMask[j] = false;
    }
    vigem_disconnect(client.get());
    client.reset();  // calls vigem_free via safe_ptr
    // Don't return error — allow Sunshine to continue without preinit
```

`gamepadMask[slot]` is only set to `true` after all steps succeed. This guarantees that if a slot partially initializes, it won't be treated as "occupied", and the destructor won't double-free because `safe_ptr` handles the intermediate state.

### Thread Safety

| Resource | Access Pattern | Synchronization |
|----------|---------------|-----------------|
| `gamepadMask` | Written in `init()` (before any readers), read-only during streaming | Happens-before via `main()` sequencing |
| `vigem_t::gamepads[]` | Created in `init()`, client input via `task_pool` threads | Existing `task_pool` serialization |
| `vigem_t::client` | Created in `init()`, used from `task_pool` callbacks | ViGEm API is thread-safe per documentation |
| Middleware WebSocket | Dedicated `io_context` thread | Single-threaded event loop |

### Shutdown Ordering

Deinit guards must destruct in this exact order:

```
1. middleware_deinit_guard  (stop WebSocket first — prevent new click_gamepad events)
2. display_device_deinit_guard
3. input_deinit_guard       (ViGEm cleanup — no more gamepad events possible)
```

Each guard is a stack variable in `main()`, so C++ guarantees reverse declaration order destruction.

## Non-Goals

- Linux/macOS middleware support (Windows only for this iteration)
- DS4 pre-creation (Xbox 360 only for preinit; DS4 still available via dynamic allocation)
- Middleware authentication/encryption
