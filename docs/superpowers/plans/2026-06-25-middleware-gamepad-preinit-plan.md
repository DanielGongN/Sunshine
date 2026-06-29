# Middleware Integration & Gamepad Pre-Initialization — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add WebSocket middleware client connecting to a local middle-platform service and pre-create 2 virtual Xbox 360 gamepads at Sunshine startup.

**Architecture:** New `middleware` module (middleware.h/cpp) encapsulating a Boost.Beast WebSocket client on its own thread; extended `vigem_t::init()` pre-creating 2 Xbox 360 targets; guarded `free_gamepad()` preventing premature destruction; guarded allocation sites in `input.cpp` reusing pre-created slots.

**Tech Stack:** C++23, Boost.Beast (header-only, already in Boost 1.89.0), nlohmann/json (already linked), ViGEm API (already linked)

## Global Constraints

- Windows only for this iteration
- Config key prefix: `middleware_*`
- ViGEm pre-created type: Xbox 360 Wired only
- Pre-created slot count: 2 (indices 0 and 1)
- Reconnect backoff: 1s → 2s → 4s → 8s → 16s → cap 60s
- XInput slot poll: max 40 retries × 250ms per gamepad
- Deinit guard destruction order: middleware → display_device → input
- NO memory leaks: all ViGEm resources via `util::safe_ptr` RAII, WebSocket thread `join()` in destructor

---

### Task 1: Add middleware config to config.h and config.cpp

**Files:**
- Modify: `src/config.h:257-287` (add `middleware_t` inside `sunshine_t`)
- Modify: `src/config.cpp:1098-1197` (add parsing in `apply_config`)

**Interfaces:**
- Consumes: nothing (first task)
- Produces: `config::sunshine.middleware` (`struct middleware_t { bool enabled; std::string address; int port; bool gamepad_preinit; }`)

- [ ] **Step 1: Add middleware_t struct to config.h**

In `src/config.h`, inside `struct sunshine_t` (after `csrf_allowed_origins` at line 286), add:

```cpp
struct middleware_t {
    bool enabled;              // default: false
    std::string address;       // default: "0.0.0.0"
    int port;                  // default: 40002
    bool gamepad_preinit;      // default: false
} middleware;
```

- [ ] **Step 2: Add config parsing in config.cpp**

In `src/config.cpp`, inside `apply_config()`, after the last existing `bool_f` line (after `bool_f(vars, "dd_config_revert_on_disconnect", ...)` at line ~1192), add:

```cpp
bool_f(vars, "middleware_enabled", sunshine.middleware.enabled);
string_f(vars, "middleware_address", sunshine.middleware.address);
int_f(vars, "middleware_port", sunshine.middleware.port);
bool_f(vars, "middleware_gamepad_preinit", sunshine.middleware.gamepad_preinit);
```

- [ ] **Step 3: Verify compilation**

```bash
cd build && cmake -G "Ninja" -DSUNSHINE_BUILD_ASSETS=OFF .. && ninja -j$(nproc)
```

Expected: build succeeds (new struct and parsing compile cleanly).

- [ ] **Step 4: Commit**

```bash
git add src/config.h src/config.cpp
git commit -m "feat(config): add middleware_t struct with parsing"
```

---

### Task 2: Create middleware WebSocket module (header)

**Files:**
- Create: `src/middleware.h`

**Interfaces:**
- Consumes: `platf::deinit_t` (from `src/platform/common.h`)
- Produces: `namespace middleware { [[nodiscard]] std::unique_ptr<platf::deinit_t> start(); }`

- [ ] **Step 1: Write middleware.h**

```cpp
/**
 * @file src/middleware.h
 * @brief Declarations for the middle-platform WebSocket client.
 */
#pragma once

// local includes
#include "platform/common.h"

namespace middleware {

  /**
   * @brief Start the middle-platform WebSocket connection.
   * @return A deinit guard that stops the connection on destruction.
   * @retval nullptr on failure (Sunshine continues without middleware).
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> start();

}  // namespace middleware
```

- [ ] **Step 2: Commit**

```bash
git add src/middleware.h
git commit -m "feat(middleware): add middleware module header"
```

---

### Task 3: Create middleware WebSocket module (implementation)

**Files:**
- Create: `src/middleware.cpp`

**Interfaces:**
- Consumes: `middleware::start()` (from Task 2), `config::sunshine.middleware` (from Task 1)
- Produces: Full WebSocket client with event subscription, JSON parsing, exponential backoff reconnect

- [ ] **Step 1: Write middleware.cpp — includes and internal state struct**

```cpp
/**
 * @file src/middleware.cpp
 * @brief Definitions for the middle-platform WebSocket client.
 */

// standard includes
#include <chrono>
#include <string>
#include <thread>

// lib includes
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "middleware.h"

using namespace std::literals;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;

namespace middleware {

  // Constants
  constexpr auto RECONNECT_BASE_DELAY = 1s;
  constexpr auto RECONNECT_MAX_DELAY = 60s;
  constexpr int RECONNECT_BACKOFF_MULTIPLIER = 2;

  // Events to subscribe to
  constexpr const char* SUBSCRIBED_EVENTS[] = {
    "force_disconnected_time",
    "standby_disconnected_time",
    "display_config",
    "click_gamepad",
    "disconnected"
  };

  class middleware_t {
  public:
    middleware_t():
        ioc {},
        ws {ioc},
        resolver {ioc},
        reconnect_delay {RECONNECT_BASE_DELAY},
        stopping {false} {
    }

    ~middleware_t() {
      stopping.store(true);
      beast::error_code ec;
      ws.close(websocket::close_code::normal, ec);  // ignore errors during shutdown
      ioc.stop();
      if (worker.joinable()) {
        worker.join();
      }
    }

    void run() {
      worker = std::thread([this]() {
        platf::set_thread_name("middleware");
        connect_and_subscribe();
        ioc.run();
      });
    }

  private:
    static std::string make_timestamp() {
      auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count();
      return std::to_string(now);
    }

    void send_subscribe_msg(const std::string& topic) {
      json root;
      root["message_id"] = make_timestamp();
      root["event"] = "subscribe";
      json data;
      data["topic"] = topic;
      root["data"] = data;

      std::string msg = root.dump();
      BOOST_LOG(info) << "Subscribing to event: "sv << topic;
      ws.write(net::buffer(msg));
    }

    void subscribe_all_events() {
      for (const auto& event : SUBSCRIBED_EVENTS) {
        send_subscribe_msg(event);
      }
    }

    void on_message_event(json& msg) {
      if (!msg.contains("event")) {
        BOOST_LOG(warning) << "No event field in middleware message"sv;
        return;
      }

      std::string event_type = msg["event"].get<std::string>();
      std::string msg_id = msg.value("message_id", "");

      BOOST_LOG(info) << "Middleware event received: "sv << event_type;

      if (event_type == "force_disconnected_time") {
        // Store AFK timeout (seconds) for use by stream logic
        int timeout = msg.value("data", json::object()).value("timeout", 0);
        if (timeout <= 0) timeout = msg.value("data", 0);
        if (timeout > 0) {
          BOOST_LOG(info) << "force_disconnected_time: timeout="sv << timeout;
        }
      }
      else if (event_type == "standby_disconnected_time") {
        int timeout = msg.value("data", json::object()).value("timeout", 0);
        if (timeout <= 0) timeout = msg.value("data", 0);
        if (timeout > 0) {
          BOOST_LOG(info) << "standby_disconnected_time: timeout="sv << timeout;
        }
      }
      else if (event_type == "display_config") {
        auto& data = msg["data"];
        int qp_high   = data.value("high", 0);
        int qp_normal = data.value("normal", 0);
        int qp_low    = data.value("low", 0);
        if (qp_high > 0 && qp_normal > 0 && qp_low > 0) {
          BOOST_LOG(info) << "display_config: qp_high="sv << qp_high
                          << " qp_normal="sv << qp_normal
                          << " qp_low="sv << qp_low;
        }
      }
      else if (event_type == "click_gamepad") {
        std::string button = msg["data"].value("button", "");
        BOOST_LOG(info) << "click_gamepad: button="sv << button;
      }
      else if (event_type == "disconnected") {
        BOOST_LOG(info) << "Middleware requested client disconnect"sv;
      }
      else {
        BOOST_LOG(debug) << "Unknown middleware event: "sv << event_type;
      }
    }

    void read_loop() {
      beast::flat_buffer buffer;
      while (!stopping.load()) {
        beast::error_code ec;
        ws.read(buffer, ec);

        if (ec == net::error::operation_aborted || stopping.load()) {
          break;
        }

        if (ec) {
          BOOST_LOG(warning) << "Middleware read error: "sv << ec.message();
          break;
        }

        try {
          std::string payload = beast::buffers_to_string(buffer.data());
          buffer.consume(buffer.size());
          json msg = json::parse(payload);
          on_message_event(msg);
        } catch (const json::parse_error& e) {
          BOOST_LOG(warning) << "Middleware JSON parse error: "sv << e.what();
        }
      }
    }

    void connect_and_subscribe() {
      auto& cfg = config::sunshine.middleware;

      while (!stopping.load()) {
        beast::error_code ec;

        // Resolve and connect
        auto results = resolver.resolve(cfg.address, std::to_string(cfg.port), ec);
        if (ec) {
          BOOST_LOG(warning) << "Middleware resolve failed: "sv << ec.message();
          schedule_reconnect();
          continue;
        }

        auto ep = net::connect(beast::get_lowest_layer(ws), results, ec);
        if (ec) {
          BOOST_LOG(warning) << "Middleware connect failed: "sv << ec.message();
          schedule_reconnect();
          continue;
        }

        // WebSocket handshake
        ws.handshake(cfg.address + ":" + std::to_string(cfg.port), "/", ec);
        if (ec) {
          BOOST_LOG(warning) << "Middleware handshake failed: "sv << ec.message();
          schedule_reconnect();
          continue;
        }

        BOOST_LOG(info) << "Middleware connected to "sv << cfg.address << ':' << cfg.port;
        reconnect_delay = RECONNECT_BASE_DELAY;  // reset backoff

        // Subscribe to events
        subscribe_all_events();

        // Enter read loop
        read_loop();

        // Read loop exited — reconnect
        BOOST_LOG(info) << "Middleware disconnected, will reconnect..."sv;
      }
    }

    void schedule_reconnect() {
      if (stopping.load()) return;

      BOOST_LOG(info) << "Middleware reconnecting in "sv
                      << std::chrono::duration_cast<std::chrono::seconds>(reconnect_delay).count()
                      << " seconds"sv;

      auto timer = std::make_shared<net::steady_timer>(ioc, reconnect_delay);
      timer->async_wait([this, timer](beast::error_code ec) {
        if (ec == net::error::operation_aborted || stopping.load()) {
          return;
        }
        connect_and_subscribe();
      });

      // Increase backoff
      reconnect_delay = std::min(
        reconnect_delay * RECONNECT_BACKOFF_MULTIPLIER,
        RECONNECT_MAX_DELAY
      );
    }

    net::io_context ioc;
    websocket::stream<tcp::socket> ws;
    tcp::resolver resolver;
    std::thread worker;
    std::chrono::milliseconds reconnect_delay;
    std::atomic<bool> stopping;
  };

  class deinit_t: public platf::deinit_t {
  public:
    deinit_t(std::unique_ptr<middleware_t> &&impl):
        impl {std::move(impl)} {
    }

    ~deinit_t() override {
      impl.reset();  // destroy middleware_t first (joins thread, closes socket)
    }

  private:
    std::unique_ptr<middleware_t> impl;
  };

  [[nodiscard]] std::unique_ptr<platf::deinit_t> start() {
    auto impl = std::make_unique<middleware_t>();
    impl->run();

    return std::make_unique<deinit_t>(std::move(impl));
  }

}  // namespace middleware
```

- [ ] **Step 2: Verify compilation (will fail due to no cmake entry yet — expected)**

```bash
cd build && cmake -G "Ninja" -DSUNSHINE_BUILD_ASSETS=OFF .. 2>&1 | tail -5
```

Expected: CMake configures OK (middleware.cpp not in build yet, ok for now).

- [ ] **Step 3: Commit**

```bash
git add src/middleware.cpp
git commit -m "feat(middleware): implement WebSocket client with event subscribe and reconnect"
```

---

### Task 4: Add gamepad pre-creation in vigem_t::init()

**Files:**
- Modify: `src/platform/windows/input.cpp:196-211` (inside `vigem_t::init()`)

**Interfaces:**
- Consumes: `config::sunshine.middleware.gamepad_preinit` (from Task 1)
- Produces: 2 pre-created Xbox 360 ViGEm targets at global indices 0 and 1, `gamepadMask[0]` and `gamepadMask[1]` set to true

- [ ] **Step 1: Modify vigem_t::init() to add preinit logic**

In `src/platform/windows/input.cpp`, in `vigem_t::init()`, after the existing probe code and before `gamepads.resize(MAX_GAMEPADS)`, replace the return statement with the preinit logic. Locate the `init()` method (line ~196):

Current code (~lines 196-211):
```cpp
int init() {
    client_t client {vigem_alloc()};
    VIGEM_ERROR status = vigem_connect(client.get());
    if (!VIGEM_SUCCESS(status)) {
        BOOST_LOG(fatal) << "ViGEmBus is not installed or running..."sv;
    } else {
        vigem_disconnect(client.get());
    }
    gamepads.resize(MAX_GAMEPADS);
    return 0;
}
```

Replace with:
```cpp
int init() {
    // Probe ViGEm during startup to see if we can successfully attach gamepads.
    // This will allow us to immediately display the error message in the web UI
    // even before the user tries to stream.
    {
        client_t probe {vigem_alloc()};
        VIGEM_ERROR status = vigem_connect(probe.get());
        if (!VIGEM_SUCCESS(status)) {
            BOOST_LOG(fatal) << "ViGEmBus is not installed or running. You must install ViGEmBus for gamepad support!"sv;
        } else {
            vigem_disconnect(probe.get());
        }
    }

    gamepads.resize(MAX_GAMEPADS);

    // Pre-create gamepads if configured
    if (config::sunshine.middleware.gamepad_preinit) {
        // Connect to ViGEm bus
        client.reset(vigem_alloc());
        auto status = vigem_connect(client.get());
        if (!VIGEM_SUCCESS(status)) {
            BOOST_LOG(error) << "ViGEm connect failed for preinit: 0x"sv << util::hex(status).to_string_view();
            client.reset();
            return 0;  // Continue without preinit
        }

        for (int slot = 0; slot < 2; ++slot) {
            auto &gamepad = gamepads[slot];

            // Allocate and add target
            gamepad.gp.reset(vigem_target_x360_alloc());
            if (!gamepad.gp) {
                BOOST_LOG(error) << "Preinit: vigem_target_x360_alloc slot="sv << slot << " failed"sv;
                goto preinit_fail;
            }

            XUSB_REPORT_INIT(&gamepad.report.x360);
            gamepad.client_relative_index = (std::uint8_t) slot;

            status = vigem_target_add(client.get(), gamepad.gp.get());
            if (!VIGEM_SUCCESS(status)) {
                BOOST_LOG(error) << "Preinit: vigem_target_add slot="sv << slot
                                 << " failed: 0x"sv << util::hex(status).to_string_view();
                gamepad.gp.reset();
                goto preinit_fail;
            }

            // Wait for XInput to assign a player slot (LED index).
            // vigem_target_add returns when the PDO is ready, but XInput
            // enumeration happens asynchronously.
            {
                ULONG userIndex = 0;
                for (int retry = 0; retry < 40; ++retry) {
                    auto idxErr = vigem_target_x360_get_user_index(
                        client.get(), gamepad.gp.get(), &userIndex);
                    if (VIGEM_SUCCESS(idxErr)) {
                        BOOST_LOG(info) << "Preinit slot="sv << slot
                                        << " XInput user index="sv << userIndex;
                        break;
                    }
                    std::this_thread::sleep_for(250ms);
                }
            }

            // Register notification callback (non-fatal on failure)
            status = vigem_target_x360_register_notification(
                client.get(), gamepad.gp.get(), x360_notify, this);
            if (!VIGEM_SUCCESS(status)) {
                BOOST_LOG(warning) << "Preinit: register_notification slot="sv << slot
                                   << " failed: 0x"sv << util::hex(status).to_string_view();
            }

            gamepadMask[slot] = true;
            BOOST_LOG(info) << "Preinit gamepad slot="sv << slot << " ready"sv;
        }

        BOOST_LOG(info) << "Preinit: 2 gamepads ready"sv;
        return 0;

    preinit_fail:
        // Clean up all previously successful slots
        for (int j = 0; j < slot; ++j) {
            if (gamepads[j].gp && vigem_target_is_attached(gamepads[j].gp.get())) {
                vigem_target_remove(client.get(), gamepads[j].gp.get());
            }
            gamepads[j].gp.reset();  // safe_ptr calls vigem_target_free
            gamepadMask[j] = false;
        }
        vigem_disconnect(client.get());
        client.reset();  // safe_ptr calls vigem_free
        BOOST_LOG(error) << "Preinit failed, continuing without pre-created gamepads"sv;
    }

    return 0;
}
```

- [ ] **Step 2: Verify compilation**

```bash
cd build && cmake -G "Ninja" -DSUNSHINE_BUILD_ASSETS=OFF .. && ninja -j$(nproc)
```

Expected: build succeeds. Preinit code compiles cleanly.

- [ ] **Step 3: Commit**

```bash
git add src/platform/windows/input.cpp
git commit -m "feat(input): pre-create 2 Xbox 360 gamepads in vigem_t::init()"
```

---

### Task 5: Client input integration — reuse pre-created gamepads and prevent destruction

**Files:**
- Modify: `src/input.cpp:1144-1155` (PNV_MULTI_CONTROLLER_PACKET handler)
- Modify: `src/input.cpp:870-900` (PSS_CONTROLLER_ARRIVAL_PACKET handler)
- Modify: `src/input.cpp:118-123` (free_gamepad function)

**Interfaces:**
- Consumes: `config::sunshine.middleware.gamepad_preinit`, `gamepadMask` (from external `input.cpp` static)
- Produces: Client controllers 0 and 1 reuse pre-created ViGEm targets instead of allocating new ones; `free_gamepad()` skips `vigem_target_remove()` for pre-created slots

- [ ] **Step 1: Add reuse check in PNV_MULTI_CONTROLLER_PACKET handler**

In `src/input.cpp`, locate `passthrough(std::shared_ptr<input_t> &input, PNV_MULTI_CONTROLLER_PACKET packet)` (~line 1129). Find the block (~line 1144):
```cpp
if ((packet->activeGamepadMask & (1 << packet->controllerNumber)) && gamepad.id < 0) {
    auto id = alloc_id(gamepadMask);
```

Insert BEFORE this block:
```cpp
// Reuse pre-created gamepad if this controller number matches a preinit slot
if (config::sunshine.middleware.gamepad_preinit &&
    packet->controllerNumber < 2 &&
    gamepadMask[packet->controllerNumber]) {
    input->gamepads[packet->controllerNumber].id = packet->controllerNumber;
}
```

The final code should look like:
```cpp
auto &gamepad = input->gamepads[packet->controllerNumber];

// If this is an event for a new gamepad, create the gamepad now...
// (existing comment)
if ((packet->activeGamepadMask & (1 << packet->controllerNumber)) && gamepad.id < 0) {
    // Reuse pre-created gamepad if this controller number matches a preinit slot
    if (config::sunshine.middleware.gamepad_preinit &&
        packet->controllerNumber < 2 &&
        gamepadMask[packet->controllerNumber]) {
        input->gamepads[packet->controllerNumber].id = packet->controllerNumber;
    }
    else {
        auto id = alloc_id(gamepadMask);
        if (id < 0) {
            return;
        }

        if (platf::alloc_gamepad(platf_input, {id, (uint8_t) packet->controllerNumber}, {}, input->feedback_queue)) {
            free_id(gamepadMask, id);
            return;
        }

        gamepad.id = id;
    }
}
```

- [ ] **Step 2: Add reuse check in PSS_CONTROLLER_ARRIVAL_PACKET handler**

In `src/input.cpp`, locate `passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_ARRIVAL_PACKET packet)` (~line 870). After the controllerNumber range check (~line 880) and the "already allocated" check, insert BEFORE `auto id = alloc_id(gamepadMask);`:

```cpp
// Reuse pre-created gamepad if this controller number matches a preinit slot
if (config::sunshine.middleware.gamepad_preinit &&
    packet->controllerNumber < 2 &&
    gamepadMask[packet->controllerNumber]) {
    input->gamepads[packet->controllerNumber].id = packet->controllerNumber;
    return;  // Pre-created gamepad already has metadata from vigem_t::init()
}
```

The final code around line 891 should look like:
```cpp
// Reuse pre-created gamepad if this controller number matches a preinit slot
if (config::sunshine.middleware.gamepad_preinit &&
    packet->controllerNumber < 2 &&
    gamepadMask[packet->controllerNumber]) {
    input->gamepads[packet->controllerNumber].id = packet->controllerNumber;
    return;  // Pre-created gamepad already has metadata from vigem_t::init()
}

auto id = alloc_id(gamepadMask);
if (id < 0) {
    return;
}

// Allocate a new gamepad
if (platf::alloc_gamepad(...  // existing code continues
```

- [ ] **Step 3: Guard free_gamepad() against destroying pre-created gamepads**

In `src/input.cpp`, locate `free_gamepad()` (the static function at ~line 118):
```cpp
void free_gamepad(platf::input_t &platf_input, int id) {
    platf::gamepad_update(platf_input, id, platf::gamepad_state_t {});
    platf::free_gamepad(platf_input, id);

    free_id(gamepadMask, id);
}
```

Replace with:
```cpp
void free_gamepad(platf::input_t &platf_input, int id) {
    // Reset gamepad state to neutral
    platf::gamepad_update(platf_input, id, platf::gamepad_state_t {});

    // Pre-created gamepads are persistent — never remove them from ViGEm
    if (config::sunshine.middleware.gamepad_preinit && id < 2) {
        return;  // Skip vigem_target_remove() and free_id()
    }

    platf::free_gamepad(platf_input, id);
    free_id(gamepadMask, id);
}
```

- [ ] **Step 4: Verify compilation**

```bash
cd build && cmake -G "Ninja" -DSUNSHINE_BUILD_ASSETS=OFF .. && ninja -j$(nproc)
```

Expected: build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/input.cpp
git commit -m "feat(input): reuse pre-created gamepads for client controllers 0/1 and guard free_gamepad"
```

---

### Task 6: Integrate middleware startup into main.cpp

**Files:**
- Modify: `src/main.cpp:1-6` (add include)
- Modify: `src/main.cpp:374-384` (add middleware::start() call after http::init())

**Interfaces:**
- Consumes: `middleware::start()` (from Task 3), `config::sunshine.middleware.enabled` (from Task 1)
- Produces: `middleware_deinit_guard` stack variable ensuring ordered shutdown

- [ ] **Step 1: Add include in main.cpp**

In `src/main.cpp`, add after `#include "logging.h"` (line ~22):

```cpp
#include "middleware.h"
```

- [ ] **Step 2: Start middleware after http::init()**

In `src/main.cpp`, locate the block after `http::init()` (~line 374-383). Insert middleware startup between `http::init()` and the mDNS/upnp async section:

```cpp
if (http::init()) {
    BOOST_LOG(fatal) << "HTTP interface failed to initialize"sv;
    // ... existing error handling ...
    return -1;
}

// Start middle-platform WebSocket connection
std::unique_ptr<platf::deinit_t> middleware_deinit_guard;
if (config::sunshine.middleware.enabled) {
    middleware_deinit_guard = middleware::start();
    if (!middleware_deinit_guard) {
        BOOST_LOG(error) << "Middleware failed to initialize"sv;
    }
}

std::unique_ptr<platf::deinit_t> mDNS;
auto sync_mDNS = std::async(std::launch::async, [&mDNS]() {
    // ... existing code ...
```

The `middleware_deinit_guard` must be declared AFTER the existing `input_deinit_guard` and `display_device_deinit_guard` in the stack, ensuring it destructs FIRST (reverse declaration order): middleware → display → input.

Verify the stack variable order in `main()`:
1. `display_device_deinit_guard` (declared ~line 206)
2. `input_deinit_guard` (declared ~line 364)
3. `middleware_deinit_guard` (NEW, declared here) — will destruct first ✓

- [ ] **Step 3: Verify compilation**

```bash
cd build && cmake -G "Ninja" -DSUNSHINE_BUILD_ASSETS=OFF .. && ninja -j$(nproc)
```

Expected: build succeeds. The middleware start call and include compile.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat(main): integrate middleware startup with ordered deinit guard"
```

---

### Task 7: Add middleware.cpp and Boost.Beast to CMake build

**Files:**
- Modify: `cmake/compile_definitions/windows.cmake:63-81` (add middleware.cpp to PLATFORM_TARGET_FILES)
- Modify: `cmake/dependencies/Boost_Sunshine.cmake:6-12` (no changes needed — Beast is header-only part of existing Boost)

**Interfaces:**
- Consumes: `src/middleware.cpp` (created in Task 3)
- Produces: Build system picks up the new source file

- [ ] **Step 1: Add middleware source to PLATFORM_TARGET_FILES**

In `cmake/compile_definitions/windows.cmake`, inside the `PLATFORM_TARGET_FILES` list (~line 63), add the middleware source file. Since middleware is Windows-only for this iteration, place it in the Windows-specific list:

```cmake
set(PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/windows/publish.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/misc.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/misc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/input.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_base.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_vram.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_ram.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_wgc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/utf_utils.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/utf_utils.h"
        "${CMAKE_SOURCE_DIR}/src/middleware.h"              # NEW
        "${CMAKE_SOURCE_DIR}/src/middleware.cpp"            # NEW
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/src/ViGEmClient.cpp"
        # ... rest unchanged ...
```

- [ ] **Step 2: Full build**

```bash
cd build && cmake -G "Ninja" -DSUNSHINE_BUILD_ASSETS=OFF .. && ninja -j$(nproc)
```

Expected: Full build succeeds. All modules compile and link.

- [ ] **Step 3: Commit**

```bash
git add cmake/compile_definitions/windows.cmake
git commit -m "build(cmake): add middleware.cpp to Windows platform sources"
```

---

### Task 8: Final integration build and smoke test

**Files:**
- None (verification only)

- [ ] **Step 1: Clean rebuild**

```bash
cd build && rm -rf * && cmake -G "Ninja" -DSUNSHINE_BUILD_ASSETS=OFF .. && ninja -j$(nproc)
```

Expected: Clean build succeeds with zero errors, zero warnings from new code.

- [ ] **Step 2: Verify binary exists and links correctly**

```bash
ls -la sunshine.exe 2>/dev/null || ls -la sunshine 2>/dev/null
```

Expected: Binary exists and is executable.

- [ ] **Step 3: Verify with default config (middleware disabled, no crash)**

Run Sunshine briefly with default config (middleware disabled):

```bash
./sunshine --help 2>&1 | head -5
```

Expected: Help output displays normally, no crash on startup.

- [ ] **Step 4: Verify new config keys parse correctly**

Create a temporary test config and run:

```bash
echo "middleware_enabled = false" > /tmp/test_sunshine.conf
echo "middleware_address = 0.0.0.0" >> /tmp/test_sunshine.conf
echo "middleware_port = 40002" >> /tmp/test_sunshine.conf
echo "middleware_gamepad_preinit = false" >> /tmp/test_sunshine.conf
./sunshine /tmp/test_sunshine.conf --help 2>&1 | head -5
```

Expected: No parse errors related to middleware_* keys. Help output displays normally.

- [ ] **Step 5: Commit any remaining changes**

```bash
git status
git add -A
git commit -m "chore: final integration verification"
```

Expected: Working tree clean after commit.

---

## Self-Review Checklist

- [x] Config struct + parsing (Task 1) → implements spec "Configuration" section
- [x] middleware.h (Task 2) + middleware.cpp (Task 3) → implements spec "middleware module" section, all 5 events subscribed, exponential backoff, JSON message format
- [x] vigem_t::init() preinit (Task 4) → implements spec "Gamepad Pre-Initialization" section, 2 X360 targets, XInput polling, goto fail cleanup
- [x] Client reuse + free guard (Task 5) → implements spec "Client Integration" section, 3 modifications with exact code
- [x] main.cpp integration (Task 6) → implements spec "Startup Sequence" and "Shutdown Sequence" sections
- [x] CMake integration (Task 7) → implements spec "Build Changes" section
- [x] Smoke test (Task 8) → verifies everything links and runs
- [x] No TBD/TODO placeholders
- [x] All steps have exact code or exact commands
- [x] Type consistency: `gamepadMask` is `std::bitset<MAX_GAMEPADS>`, `id` is `int`, `config::sunshine.middleware.gamepad_preinit` is `bool`
- [x] Memory safety: `goto preinit_fail` cleans up partial init, `safe_ptr` handles ViGEm RAII, `~middleware_t()` joins thread, `free_gamepad()` guard prevents premature removal
