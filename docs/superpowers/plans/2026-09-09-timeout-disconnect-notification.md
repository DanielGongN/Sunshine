# Timeout Disconnect Notification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Separate the absolute force timeout from per-session idle detection and report exactly one typed `disconnected` event to the upstream service without stopping Moonlight locally.

**Architecture:** Keep middleware payload construction in `middleware`, per-session user-activity timestamps in each `input_t`, and timeout selection in a small pure helper under `stream::detail`. The control loop asks the helper for one notification reason, latches the session after the first report, and leaves the session running until the existing upstream `disconnected` command terminates it.

**Tech Stack:** C++23, Boost.Asio/Beast, nlohmann JSON, GoogleTest, CMake/Ninja, MSYS2 UCRT64.

**Spec:** `docs/superpowers/specs/2026-09-09-timeout-disconnect-notification-design.md`

## Global Constraints

- Prefix Windows build and test commands with `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.
- Use build directories prefixed with `cmake-build-`.
- Run `cmake-build-codex-tests/tests/test_sunshine.exe` for GoogleTest verification.
- Format all changed C/C++ code according to `.clang-format`.
- Do not update localization files other than `en`; this change requires no localization edits.
- Preserve all pre-existing user modifications in the dirty working tree.
- Do not commit implementation files automatically because every target source file already contains user changes that predate this plan.
- Interpret both timeout values as seconds; a numeric zero disables the corresponding timeout.
- Timeout notification does not call `send_termination_msg()` or `session::stop()`.

---

### Task 1: Typed Middleware Disconnect Payloads

**Files:**
- Modify: `src/middleware.h`
- Modify: `src/middleware.cpp:232-242`
- Modify: `tests/unit/test_middleware.cpp`

**Interfaces:**
- Produces: `middleware::disconnect_notification_type_e` with `other=0`, `force_timeout=1`, and `standby_timeout=2`.
- Produces: `middleware::detail::make_disconnected_payload(std::string_view, disconnect_notification_type_e) -> nlohmann::json`.
- Produces: typed overloads of `notify_client_disconnected()` whose default type is `other`.

- [ ] **Step 1: Write failing payload tests**

Add declarations and tests that exercise all fixed protocol values:

```cpp
namespace middleware {
  enum class disconnect_notification_type_e : int;

  namespace detail {
    nlohmann::json make_disconnected_payload(std::string_view message, disconnect_notification_type_e type);
  }
}

TEST(MiddlewareDisconnectTests, BuildsOtherDisconnectPayload) {
  auto msg = middleware::detail::make_disconnected_payload(
    "network disconnect",
    middleware::disconnect_notification_type_e::other
  );
  EXPECT_EQ(msg.at("event"), "disconnected");
  EXPECT_EQ(msg.at("message"), "network disconnect");
  EXPECT_EQ(msg.at("type"), 0);
}

TEST(MiddlewareDisconnectTests, BuildsForceTimeoutPayload) {
  auto msg = middleware::detail::make_disconnected_payload(
    "force timeout",
    middleware::disconnect_notification_type_e::force_timeout
  );
  EXPECT_EQ(msg.at("type"), 1);
}

TEST(MiddlewareDisconnectTests, BuildsStandbyTimeoutPayload) {
  auto msg = middleware::detail::make_disconnected_payload(
    "standby timeout",
    middleware::disconnect_notification_type_e::standby_timeout
  );
  EXPECT_EQ(msg.at("type"), 2);
}
```

- [ ] **Step 2: Run the focused tests and verify failure**

Run:

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-codex-tests --target test_sunshine -j2 && ./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_color=no --gtest_filter=MiddlewareDisconnectTests.*"
```

Expected: compilation fails because the enum and payload helper do not exist.

- [ ] **Step 3: Add the enum and payload helper**

Declare the enum and typed public API in `middleware.h`:

```cpp
enum class disconnect_notification_type_e : int {
  other = 0,
  force_timeout = 1,
  standby_timeout = 2,
};

void notify_client_disconnected(
  std::string_view message,
  disconnect_notification_type_e type = disconnect_notification_type_e::other
);
void notify_client_disconnected(
  std::u8string_view message,
  disconnect_notification_type_e type = disconnect_notification_type_e::other
);
```

Implement payload construction once in `middleware.cpp`:

```cpp
nlohmann::json detail::make_disconnected_payload(
  std::string_view message,
  disconnect_notification_type_e type
) {
  json msg;
  msg["event"] = "disconnected";
  msg["message"] = std::string {message};
  msg["type"] = static_cast<int>(type);
  return msg;
}
```

Make both notification overloads pass the selected type to this helper. Existing call sites omit the argument and therefore become `type=0`.

- [ ] **Step 4: Format and run the focused tests**

Run:

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "clang-format -i src/middleware.h src/middleware.cpp tests/unit/test_middleware.cpp"
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-codex-tests --target test_sunshine -j2 && ./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_color=no --gtest_filter=MiddlewareDisconnectTests.*"
```

Expected: all `MiddlewareDisconnectTests` pass.

### Task 2: Per-Session Real User Activity Time

**Files:**
- Modify: `src/input.h:65-74`
- Modify: `src/input.cpp:41-44, 360-457, 1818-1897, 2132-2137`
- Create: `tests/unit/test_input_activity.cpp`

**Interfaces:**
- Produces: `input::update_input_time(const std::shared_ptr<input_t>&)`.
- Produces: `input::get_last_input_time(const std::shared_ptr<input_t>&) -> std::chrono::steady_clock::time_point`.
- Produces: `input::detail::is_direct_user_activity_packet(void*) -> bool` for deterministic packet classification tests.
- Produces: `input::detail::is_changed_active_gamepad_state(bool, const platf::gamepad_state_t&, const platf::gamepad_state_t&) -> bool`.

- [ ] **Step 1: Write failing input classification tests**

Create packet-based tests using `third-party/moonlight-common-c/src/Input.h`:

```cpp
TEST(InputActivityTests, ZeroRelativeMouseMovementIsNotActivity) {
  NV_REL_MOUSE_MOVE_PACKET packet {};
  packet.header.magic = util::endian::big<std::uint32_t>(MOUSE_MOVE_REL_MAGIC_GEN5);
  EXPECT_FALSE(input::detail::is_direct_user_activity_packet(&packet));
}

TEST(InputActivityTests, NonZeroRelativeMouseMovementIsActivity) {
  NV_REL_MOUSE_MOVE_PACKET packet {};
  packet.header.magic = util::endian::big<std::uint32_t>(MOUSE_MOVE_REL_MAGIC_GEN5);
  packet.deltaX = util::endian::big<std::int16_t>(1);
  EXPECT_TRUE(input::detail::is_direct_user_activity_packet(&packet));
}

TEST(InputActivityTests, ControllerMotionIsNotActivity) {
  SS_CONTROLLER_MOTION_PACKET packet {};
  packet.header.magic = util::endian::big<std::uint32_t>(SS_CONTROLLER_MOTION_MAGIC);
  EXPECT_FALSE(input::detail::is_direct_user_activity_packet(&packet));
}

TEST(InputActivityTests, RepeatedGamepadStateIsNotActivity) {
  platf::gamepad_state_t state {platf::A, 0, 0, 0, 0, 0, 0};
  EXPECT_FALSE(input::detail::is_changed_active_gamepad_state(true, state, state));
}

TEST(InputActivityTests, ChangedActiveGamepadStateIsActivity) {
  platf::gamepad_state_t old_state {};
  platf::gamepad_state_t new_state {platf::A, 0, 0, 0, 0, 0, 0};
  EXPECT_TRUE(input::detail::is_changed_active_gamepad_state(true, old_state, new_state));
  EXPECT_FALSE(input::detail::is_changed_active_gamepad_state(false, old_state, new_state));
}
```

Also cover zero/non-zero vertical and horizontal scrolling, keyboard, unicode, mouse buttons, touch, pen, controller touch, controller arrival, and controller battery packets.

- [ ] **Step 2: Run the focused tests and verify failure**

Run:

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-codex-tests --target test_sunshine -j2 && ./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_color=no --gtest_filter=InputActivityTests.*"
```

Expected: compilation fails because the detail helpers do not exist.

- [ ] **Step 3: Move the timestamp into `input_t`**

Delete the process-global `last_input_time` and add this member to `input_t`, initialized in its constructor:

```cpp
std::atomic<std::chrono::steady_clock::time_point> last_input_time;
```

Change the public functions to require an input context:

```cpp
void update_input_time(const std::shared_ptr<input_t> &input) {
  input->last_input_time.store(std::chrono::steady_clock::now(), std::memory_order_release);
}

std::chrono::steady_clock::time_point get_last_input_time(const std::shared_ptr<input_t> &input) {
  return input->last_input_time.load(std::memory_order_acquire);
}
```

- [ ] **Step 4: Implement direct input classification**

Replace the magic-only activity helper with `is_direct_user_activity_packet(void*)`. It returns true for keyboard, non-empty unicode, mouse buttons, non-zero relative mouse movement, absolute mouse movement, non-zero scroll, touch, pen, and controller touch. It returns false for controller state, controller motion, controller arrival, controller battery, and unknown packets.

Keep controller state separate so it can compare against the session's previous state:

```cpp
bool detail::is_changed_active_gamepad_state(
  bool active,
  const platf::gamepad_state_t &old_state,
  const platf::gamepad_state_t &new_state
) {
  return active && !same_gamepad_state(old_state, new_state);
}
```

Remove the timestamp update from `passthrough_next_message()`. Invoke the
classifier from the corresponding typed `passthrough()` overload only after
that overload's configuration and packet validation checks have succeeded.
For keyboard input, update only after duplicate key-down/key-up suppression.
Change the unicode overload to accept the session input context so it can
update the correct timestamp. In the multi-controller overload, update only
after validation and only when the active gamepad state changed. Do not update
for controller arrival/removal or synthetic middleware clicks.

- [ ] **Step 5: Format and run focused tests**

Run:

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "clang-format -i src/input.h src/input.cpp tests/unit/test_input_activity.cpp"
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-codex-tests --target test_sunshine -j2 && ./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_color=no --gtest_filter=InputActivityTests.*"
```

Expected: all `InputActivityTests` pass.

### Task 3: Independent Timeout Decision and Notification-Only Control Flow

**Files:**
- Modify: `src/stream.h`
- Modify: `src/stream.cpp:431-442, 1278-1327, 2433-2468`
- Modify: `tests/unit/test_stream.cpp`

**Interfaces:**
- Consumes: typed middleware disconnect notifications from Task 1.
- Consumes: per-session `input::get_last_input_time(session.input)` from Task 2.
- Produces: `stream::detail::timeout_notification_e {none, force, standby}`.
- Produces: `stream::detail::select_timeout_notification(std::int64_t session_seconds, std::int64_t idle_seconds, int force_timeout, int standby_timeout, bool already_notified) -> timeout_notification_e`.

- [ ] **Step 1: Write failing timeout decision tests**

Add table-driven tests:

```cpp
TEST(TimeoutNotificationTests, ForceUsesSessionElapsedTime) {
  EXPECT_EQ(
    stream::detail::select_timeout_notification(900, 1, 900, 900, false),
    stream::detail::timeout_notification_e::force
  );
}

TEST(TimeoutNotificationTests, StandbyUsesIdleElapsedTime) {
  EXPECT_EQ(
    stream::detail::select_timeout_notification(100, 300, 900, 300, false),
    stream::detail::timeout_notification_e::standby
  );
}

TEST(TimeoutNotificationTests, ForceWinsEqualExpiredDeadlines) {
  EXPECT_EQ(
    stream::detail::select_timeout_notification(900, 900, 900, 900, false),
    stream::detail::timeout_notification_e::force
  );
}

TEST(TimeoutNotificationTests, AlreadyNotifiedSuppressesAllLaterReports) {
  EXPECT_EQ(
    stream::detail::select_timeout_notification(1800, 1800, 900, 900, true),
    stream::detail::timeout_notification_e::none
  );
}

TEST(TimeoutNotificationTests, ZeroDisablesEachTimeout) {
  EXPECT_EQ(
    stream::detail::select_timeout_notification(900, 900, 0, 0, false),
    stream::detail::timeout_notification_e::none
  );
}
```

Also cover non-expired values, a shorter force value, a shorter standby value, and negative values being treated as disabled by the selector.

- [ ] **Step 2: Run the focused tests and verify failure**

Run:

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-codex-tests --target test_sunshine -j2 && ./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_color=no --gtest_filter=TimeoutNotificationTests.*"
```

Expected: compilation fails because the selector does not exist.

- [ ] **Step 3: Implement the pure selector**

Add the detail enum and function declaration to `stream.h` and implement this priority in `stream.cpp`:

```cpp
if (already_notified) {
  return timeout_notification_e::none;
}
if (force_timeout > 0 && session_seconds >= force_timeout) {
  return timeout_notification_e::force;
}
if (standby_timeout > 0 && idle_seconds >= standby_timeout) {
  return timeout_notification_e::standby;
}
return timeout_notification_e::none;
```

- [ ] **Step 4: Add per-session timing and latch state**

Add to `session_t`:

```cpp
std::chrono::steady_clock::time_point started_at;
bool timeout_notification_sent {};
```

Immediately before setting the session state to `RUNNING`, initialize both clocks:

```cpp
session.started_at = std::chrono::steady_clock::now();
input::update_input_time(session.input);
session.timeout_notification_sent = false;
```

- [ ] **Step 5: Replace the idle-only stop block**

For each running session with a connected control peer, calculate both elapsed values and call the selector. On force, set the latch and send:

```cpp
middleware::notify_client_disconnected(
  u8"长时间挂机，等待上游断开连接",
  middleware::disconnect_notification_type_e::force_timeout
);
```

On standby, set the latch and send:

```cpp
middleware::notify_client_disconnected(
  u8"长时间无操作，等待上游断开连接",
  middleware::disconnect_notification_type_e::standby_timeout
);
```

Log `session_sec`, `idle_sec`, and the selected reason. Do not call `send_termination_msg()` or `session::stop()` in either timeout branch. Preserve the existing upstream `mail::force_disconnect` path, which still sends `0x80030023` and stops all running sessions.

- [ ] **Step 6: Format and run focused tests**

Run:

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "clang-format -i src/stream.h src/stream.cpp tests/unit/test_stream.cpp"
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-codex-tests --target test_sunshine -j2 && ./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_color=no --gtest_filter=TimeoutNotificationTests.*"
```

Expected: all `TimeoutNotificationTests` pass.

### Task 4: Integrated Verification

**Files:**
- Verify: `src/middleware.h`
- Verify: `src/middleware.cpp`
- Verify: `src/input.h`
- Verify: `src/input.cpp`
- Verify: `src/stream.h`
- Verify: `src/stream.cpp`
- Verify: `tests/unit/test_middleware.cpp`
- Verify: `tests/unit/test_input_activity.cpp`
- Verify: `tests/unit/test_stream.cpp`

**Interfaces:**
- Consumes: all interfaces from Tasks 1-3.
- Produces: a buildable implementation with regression coverage and no duplicate timeout reports.

- [ ] **Step 1: Run whitespace and formatting checks**

Run:

```powershell
git diff --check -- src/middleware.h src/middleware.cpp src/input.h src/input.cpp src/stream.h src/stream.cpp tests/unit/test_middleware.cpp tests/unit/test_input_activity.cpp tests/unit/test_stream.cpp
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "clang-format --dry-run --Werror src/middleware.h src/middleware.cpp src/input.h src/input.cpp src/stream.h src/stream.cpp tests/unit/test_middleware.cpp tests/unit/test_input_activity.cpp tests/unit/test_stream.cpp"
```

Expected: both commands succeed without diagnostics.

- [ ] **Step 2: Build the complete test executable**

Run:

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-codex-tests --target test_sunshine -j2"
```

Expected: `cmake-build-codex-tests/tests/test_sunshine.exe` links successfully.

- [ ] **Step 3: Run all feature tests together**

Run:

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_color=no --gtest_filter=MiddlewareDisconnectTests.*:InputActivityTests.*:TimeoutNotificationTests.*"
```

Expected: all feature tests pass.

- [ ] **Step 4: Run the full GoogleTest suite**

Run:

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_color=no"
```

Expected: no new failures. The pre-existing `ConfigConsistencyTest.AllConfigOptionsExistInAllFiles` failure may remain until the middleware configuration documentation/UI work is completed separately.

- [ ] **Step 5: Review the final diff against the specification**

Confirm all of the following in the diff and test output:

```text
force elapsed source       = session.started_at
standby elapsed source     = session.input.last_input_time
timeout local stop         = absent
timeout termination packet = absent
timeout notification latch = one per session
force payload type         = 1
standby payload type       = 2
all other payload types    = 0
upstream disconnect path   = unchanged and still stops Moonlight
```

Because target source files contained pre-existing user modifications before this implementation, leave the implementation uncommitted for user review.
