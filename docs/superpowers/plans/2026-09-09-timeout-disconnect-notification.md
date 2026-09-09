# Timeout Disconnect Notification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect a stable keyboard, mouse-button, or gamepad hold for `force_disconnected_time`, detect true inactivity for `standby_disconnected_time`, and send one typed upstream notification without locally disconnecting Moonlight.

**Architecture:** Keep all hold and activity state inside each `input_t`. Input handlers update a synchronized timeout snapshot through a small, testable state machine; the stream control loop selects force only while a held signature is active and standby only while it is inactive. Existing middleware payload types and the later upstream-driven Moonlight termination path remain unchanged.

**Tech Stack:** C++23, Boost.Asio/Beast, nlohmann JSON, GoogleTest, CMake/Ninja, MSYS2 UCRT64.

**Spec:** `docs/superpowers/specs/2026-09-09-timeout-disconnect-notification-design.md`

## Global Constraints

- Prefix Windows build and test commands with `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.
- Use build directories prefixed with `cmake-build-`.
- The GoogleTest executable is `cmake-build-codex-tests/tests/test_sunshine.exe`.
- Format changed C/C++ code according to `.clang-format`.
- Do not update localization files other than `en`; this correction requires no localization changes.
- `force_disconnected_time` measures one unchanged, non-empty held-control signature; it is not session duration.
- A held signature pauses standby even when force timeout is disabled.
- Timeout notification does not call `send_termination_msg()` or `session::stop()`.
- `data.type` remains `0` for other reasons, `1` for force hold, and `2` for standby.
- Codex does not run compilation or tests for this implementation; the user runs the listed verification commands.

---

### Task 1: Force-Hold State Machine and Stable Gamepad Classification

**Files:**
- Modify: `src/input.h`
- Modify: `src/input.cpp`
- Modify: `tests/unit/test_input_activity.cpp`

**Interfaces:**
- Produces: `input::timeout_state_t { bool force_hold_active; steady_clock::time_point force_hold_started_at; steady_clock::time_point last_input_time; }`.
- Produces: `input::detail::force_axis_e { negative, neutral, positive }`.
- Produces: `input::detail::gamepad_force_state_t` containing digital buttons, trigger classifications, and four axis classifications.
- Produces: `input::detail::update_force_axis(force_axis_e, std::int16_t) -> force_axis_e`.
- Produces: `input::detail::update_force_trigger(bool, std::uint8_t) -> bool`.
- Produces: `input::detail::update_gamepad_force_state(gamepad_force_state_t&, bool, const platf::gamepad_state_t&) -> bool`.
- Produces: `input::detail::apply_force_signature_change(timeout_state_t&, bool, steady_clock::time_point)`.

- [ ] **Step 1: Replace the old gamepad-activity tests with failing force-state tests**

Add deterministic tests using fixed `steady_clock::time_point` values:

```cpp
TEST(InputForceStateTests, NonEmptySignatureStartsForceTiming) {
  input::timeout_state_t state {};
  auto now = std::chrono::steady_clock::time_point {10s};

  input::detail::apply_force_signature_change(state, true, now);

  EXPECT_TRUE(state.force_hold_active);
  EXPECT_EQ(state.force_hold_started_at, now);
  EXPECT_EQ(state.last_input_time, now);
}

TEST(InputForceStateTests, ChangedSignatureRestartsForceTiming) {
  input::timeout_state_t state {true, std::chrono::steady_clock::time_point {10s}, {}};
  auto changed_at = std::chrono::steady_clock::time_point {20s};

  input::detail::apply_force_signature_change(state, true, changed_at);

  EXPECT_EQ(state.force_hold_started_at, changed_at);
}

TEST(InputForceStateTests, EmptySignatureStopsForceAndRestartsStandby) {
  input::timeout_state_t state {true, std::chrono::steady_clock::time_point {10s}, {}};
  auto released_at = std::chrono::steady_clock::time_point {20s};

  input::detail::apply_force_signature_change(state, false, released_at);

  EXPECT_FALSE(state.force_hold_active);
  EXPECT_EQ(state.last_input_time, released_at);
}
```

Add gamepad hysteresis tests:

```cpp
TEST(InputForceStateTests, StickHysteresisIgnoresSmallFluctuations) {
  using input::detail::force_axis_e;
  EXPECT_EQ(input::detail::update_force_axis(force_axis_e::neutral, 7999), force_axis_e::neutral);
  EXPECT_EQ(input::detail::update_force_axis(force_axis_e::neutral, 8000), force_axis_e::positive);
  EXPECT_EQ(input::detail::update_force_axis(force_axis_e::positive, 7000), force_axis_e::positive);
  EXPECT_EQ(input::detail::update_force_axis(force_axis_e::positive, 6000), force_axis_e::neutral);
  EXPECT_EQ(input::detail::update_force_axis(force_axis_e::positive, -8000), force_axis_e::negative);
}

TEST(InputForceStateTests, TriggerHysteresisIgnoresSmallFluctuations) {
  EXPECT_FALSE(input::detail::update_force_trigger(false, 29));
  EXPECT_TRUE(input::detail::update_force_trigger(false, 30));
  EXPECT_TRUE(input::detail::update_force_trigger(true, 21));
  EXPECT_FALSE(input::detail::update_force_trigger(true, 20));
}
```

Cover digital button changes, inactive-controller clearing, unchanged stable states returning `false`, and changed direction returning `true` through `update_gamepad_force_state()`.

- [ ] **Step 2: User runs the focused test to observe the expected failure**

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-codex-tests --target test_sunshine -j2 && ./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_color=no --gtest_filter=InputForceStateTests.*"
```

Expected: compilation fails because the new timeout-state and classifier interfaces do not exist.

- [ ] **Step 3: Add the timeout-state and classifier types**

Declare in `input.h`:

```cpp
struct timeout_state_t {
  bool force_hold_active {};
  std::chrono::steady_clock::time_point force_hold_started_at {};
  std::chrono::steady_clock::time_point last_input_time {};
};

namespace detail {
  enum class force_axis_e : std::int8_t {
    negative = -1,
    neutral = 0,
    positive = 1,
  };

  struct gamepad_force_state_t {
    std::uint32_t buttons {};
    bool left_trigger {};
    bool right_trigger {};
    force_axis_e left_x {force_axis_e::neutral};
    force_axis_e left_y {force_axis_e::neutral};
    force_axis_e right_x {force_axis_e::neutral};
    force_axis_e right_y {force_axis_e::neutral};

    bool has_held_control() const;
  };
}
```

Use constants `STICK_ACTIVATE_THRESHOLD=8000`, `STICK_RELEASE_THRESHOLD=6000`, `TRIGGER_ACTIVATE_THRESHOLD=30`, and `TRIGGER_RELEASE_THRESHOLD=20`. Implement axis and trigger hysteresis exactly as specified, including direct positive-to-negative transitions at the activation threshold.

- [ ] **Step 4: Implement force signature transitions**

Implement:

```cpp
void detail::apply_force_signature_change(
  timeout_state_t &state,
  bool force_hold_active,
  std::chrono::steady_clock::time_point now
) {
  state.force_hold_active = force_hold_active;
  state.last_input_time = now;
  if (force_hold_active) {
    state.force_hold_started_at = now;
  }
}
```

`update_gamepad_force_state()` must build a classified next state, compare every field with the prior classified state, assign it, and return whether the stable signature changed. An inactive controller produces an empty classified state.

- [ ] **Step 5: User runs the focused tests and expects them to pass**

Use the Step 2 command. Expected: all `InputForceStateTests` pass.

### Task 2: Wire Per-Session Keyboard, Mouse, and Gamepad Hold State

**Files:**
- Modify: `src/input.h`
- Modify: `src/input.cpp`
- Modify: `tests/unit/test_input_activity.cpp`

**Interfaces:**
- Consumes: the Task 1 force-state helpers.
- Produces: `input::reset_timeout_state(const std::shared_ptr<input_t>&)`.
- Produces: `input::get_timeout_state(const std::shared_ptr<input_t>&) -> timeout_state_t`.
- Retains: `input::update_input_time(const std::shared_ptr<input_t>&)` for accepted non-hold activity.
- Produces: `input::detail::update_digital_force_state(std::unordered_set<std::uint32_t>&, std::uint32_t, bool) -> bool`.

- [ ] **Step 1: Add failing digital held-set tests**

Test press, repeated press, release, and repeated release without platform injection:

```cpp
TEST(InputForceStateTests, DigitalHeldSetChangesOnlyOnTransitions) {
  std::unordered_set<std::uint32_t> held;

  EXPECT_TRUE(input::detail::update_digital_force_state(held, 42, true));
  EXPECT_FALSE(input::detail::update_digital_force_state(held, 42, true));
  EXPECT_TRUE(input::detail::update_digital_force_state(held, 42, false));
  EXPECT_FALSE(input::detail::update_digital_force_state(held, 42, false));
  EXPECT_TRUE(held.empty());
}
```

Expected before implementation: compilation fails because
`update_digital_force_state()` does not exist.

- [ ] **Step 2: User runs the digital held-set test to observe the expected failure**

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-codex-tests --target test_sunshine -j2 && ./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_color=no --gtest_filter=InputForceStateTests.DigitalHeldSetChangesOnlyOnTransitions"
```

- [ ] **Step 3: Add synchronized state and held-control collections to `input_t`**

Add these per-session members and initialize their time points together:

```cpp
std::mutex timeout_state_mutex;
timeout_state_t timeout_state;
std::unordered_set<key_press_id_t> force_held_keys;
std::unordered_set<std::uint32_t> force_held_mouse_buttons;
std::vector<detail::gamepad_force_state_t> force_gamepads;
```

Add `<unordered_set>` to `input.h` because the detail test seam uses it, and
use the same type in `input.cpp`. Do not use the existing process-global
`key_press` or `mouse_press` collections for timeout decisions.

- [ ] **Step 4: Add synchronized timeout-state accessors**

Implement `reset_timeout_state()`, `update_input_time()`, and `get_timeout_state()` under `timeout_state_mutex`. `reset_timeout_state()` sets force inactive and both timestamps to one captured `now`. `update_input_time()` only updates `last_input_time`. `get_timeout_state()` returns a value snapshot.

Treat `timeout_state` and every `force_held_*` collection as one synchronized
unit protected by `timeout_state_mutex`. Add internal helpers that require this
mutex to be held:

```cpp
bool has_force_held_control_locked(const input_t &input);
void apply_current_force_signature_locked(input_t &input) {
  auto now = std::chrono::steady_clock::now();
  detail::apply_force_signature_change(input.timeout_state, has_force_held_control_locked(input), now);
}
```

`reset_timeout_state()` captures one `now`, determines whether a signature is
already held under the same mutex, and initializes force from `now` when it is
non-empty; otherwise it initializes standby from `now`. This safely handles
input received immediately before the session publishes `RUNNING`.

Call `apply_current_force_signature_locked()` only after a stable held signature
actually changes.

- [ ] **Step 5: Wire keyboard and mouse-button changes**

First implement the tested collection transition helper:

```cpp
bool detail::update_digital_force_state(
  std::unordered_set<std::uint32_t> &held,
  std::uint32_t id,
  bool pressed
) {
  return pressed ? held.emplace(id).second : held.erase(id) != 0;
}
```

After configuration and packet validation, lock `timeout_state_mutex` and update
the per-session held set before process-global platform duplicate suppression:

```cpp
auto changed = detail::update_digital_force_state(input->force_held_keys, key_id, !release);
if (changed) {
  apply_current_force_signature_locked(*input);
}
```

Use equivalent explicit insert/erase logic for mouse buttons greater than zero. Repeated down/down or up/up packets do not change the signature. Keyboard shortcuts still count as real held input even when Sunshine consumes the shortcut rather than injecting it into the OS.

- [ ] **Step 6: Wire gamepad classified state changes and controller removal**

Construct the raw client gamepad state before Sunshine's synthetic BACK/HOME
transformations. Under `timeout_state_mutex`, pass it with the packet's active
bit to `update_gamepad_force_state()` for that controller. If the stable
classified state changes, call `apply_current_force_signature_locked()`.

When a controller is removed, clear its classified state before returning and apply the resulting signature transition. Arrival, battery, motion, controller touch, and middleware synthetic clicks do not create held state.

- [ ] **Step 7: Preserve standby real-activity rules**

Keep non-zero mouse motion, valid absolute mouse motion, non-zero scroll, text, touch, pen, and controller touch calling `update_input_time()`. Digital press/release and stable gamepad-signature changes update last activity through `apply_force_signature_change()`.

Do not update either timeout state for periodic ping, loss, repeated input states, gamepad motion, battery, arrival, or synthetic clicks. Standby pausing is implemented by the stream selector in Task 3, so `update_input_time()` does not need to inspect force state.

- [ ] **Step 8: User runs input tests**

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-codex-tests --target test_sunshine -j2 && ./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_color=no --gtest_filter=InputForceStateTests.*:InputActivityTests.*"
```

Expected: all input timeout tests pass.

### Task 3: Select Force Hold or Standby in the Stream Control Loop

**Files:**
- Modify: `src/stream.h`
- Modify: `src/stream.cpp`
- Modify: `tests/unit/test_stream.cpp`

**Interfaces:**
- Consumes: `input::get_timeout_state(session.input)` from Task 2.
- Produces: `stream::detail::select_timeout_notification(bool force_hold_active, std::int64_t force_hold_seconds, std::int64_t idle_seconds, int force_timeout, int standby_timeout, bool already_notified) -> timeout_notification_e`.

- [ ] **Step 1: Replace session-duration tests with failing mutual-exclusion tests**

```cpp
TEST(StreamTimeoutNotificationTests, ActiveHoldSelectsForceAtDeadline) {
  EXPECT_EQ(
    stream::detail::select_timeout_notification(true, 1800, 9999, 1800, 1, false),
    stream::detail::timeout_notification_e::force
  );
}

TEST(StreamTimeoutNotificationTests, ActiveHoldPausesStandby) {
  EXPECT_EQ(
    stream::detail::select_timeout_notification(true, 10, 9999, 1800, 1, false),
    stream::detail::timeout_notification_e::none
  );
}

TEST(StreamTimeoutNotificationTests, EmptyHoldSelectsStandbyAtDeadline) {
  EXPECT_EQ(
    stream::detail::select_timeout_notification(false, 9999, 1800, 1, 1800, false),
    stream::detail::timeout_notification_e::standby
  );
}

TEST(StreamTimeoutNotificationTests, EmptyHoldCannotSelectForce) {
  EXPECT_EQ(
    stream::detail::select_timeout_notification(false, 9999, 1, 1, 1800, false),
    stream::detail::timeout_notification_e::none
  );
}
```

Retain tests for `already_notified` and zero timeout values. Add the case where force is disabled but an active hold still pauses standby.

- [ ] **Step 2: User runs the focused test to observe the expected failure**

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-codex-tests --target test_sunshine -j2 && ./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_color=no --gtest_filter=StreamTimeoutNotificationTests.*"
```

Expected: tests fail because the existing selector treats force as session duration.

- [ ] **Step 3: Implement the mutually exclusive selector**

Replace the selector signature and logic:

```cpp
if (already_notified) {
  return timeout_notification_e::none;
}
if (force_hold_active) {
  return force_timeout > 0 && force_hold_seconds >= force_timeout ? timeout_notification_e::force
                                                                  : timeout_notification_e::none;
}
return standby_timeout > 0 && idle_seconds >= standby_timeout ? timeout_notification_e::standby
                                                               : timeout_notification_e::none;
```

- [ ] **Step 4: Replace session-duration calculation with the input timeout snapshot**

Remove `session_t::started_at`. Initialize the input snapshot immediately before publishing `RUNNING`:

```cpp
input::reset_timeout_state(session.input);
session.timeout_notification_sent = false;
session.state.store(state_e::RUNNING, std::memory_order_release);
```

In the control loop, get one snapshot and calculate eligible elapsed values:

```cpp
auto timeout_state = input::get_timeout_state(session->input);
auto force_hold_sec = timeout_state.force_hold_active
                        ? std::chrono::duration_cast<std::chrono::seconds>(now - timeout_state.force_hold_started_at).count()
                        : 0;
auto idle_sec = std::chrono::duration_cast<std::chrono::seconds>(now - timeout_state.last_input_time).count();
```

Pass these values and `force_hold_active` to the selector. Log `force_hold_sec` for force and `idle_sec` for standby.

- [ ] **Step 5: Preserve notification-only behavior**

Force sends message `长时间挂机，等待上游断开连接` with `force_timeout` type 1. Standby sends `长时间无操作，等待上游断开连接` with `standby_timeout` type 2. Both set `timeout_notification_sent` before enqueueing and neither sends a termination message nor stops the session.

Do not modify the existing upstream `mail::force_disconnect` path; it still sends Moonlight graceful termination `0x80030023` and stops running sessions.

- [ ] **Step 6: User runs stream timeout tests**

Use the Step 2 command. Expected: all `StreamTimeoutNotificationTests` pass.

### Task 4: Formatting, Static Review, and User Verification

**Files:**
- Verify: `src/input.h`
- Verify: `src/input.cpp`
- Verify: `src/stream.h`
- Verify: `src/stream.cpp`
- Verify: `src/middleware.h`
- Verify: `src/middleware.cpp`
- Verify: `tests/unit/test_input_activity.cpp`
- Verify: `tests/unit/test_stream.cpp`
- Verify: `tests/unit/test_middleware.cpp`

**Interfaces:**
- Consumes: all interfaces from Tasks 1-3.
- Produces: formatted source plus a user-executable verification sequence.

- [ ] **Step 1: Format changed C++ files**

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "/e/WorkComp/LLVM/bin/clang-format.exe -i src/input.h src/input.cpp src/stream.h src/stream.cpp tests/unit/test_input_activity.cpp tests/unit/test_stream.cpp"
```

- [ ] **Step 2: Codex performs non-building static checks**

```powershell
git diff --check -- src/input.h src/input.cpp src/stream.h src/stream.cpp tests/unit/test_input_activity.cpp tests/unit/test_stream.cpp
```

Confirm directly from the diff:

```text
force elapsed source       = current per-session held signature start
standby eligibility        = no held signature active
standby elapsed source     = current session last real input/release time
session-duration force     = absent
timeout local stop         = absent
timeout termination packet = absent
timeout notification latch = one per session
force payload type         = 1
standby payload type       = 2
all other payload types    = 0
upstream disconnect path   = unchanged and still stops Moonlight
```

- [ ] **Step 3: User builds the complete test executable**

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-codex-tests --target test_sunshine -j2"
```

Expected: `cmake-build-codex-tests/tests/test_sunshine.exe` links successfully.

- [ ] **Step 4: User runs all feature tests**

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_color=no --gtest_filter=MiddlewareDisconnectTests.*:InputForceStateTests.*:InputActivityTests.*:StreamTimeoutNotificationTests.*"
```

Expected: every selected test passes.

- [ ] **Step 5: User runs the full suite**

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_color=no"
```

Expected: no new failures. Report any baseline failure separately from this feature.

- [ ] **Step 6: Leave implementation changes for user review**

Do not automatically commit production code. Report changed files, the static-review result, and the exact tests the user still needs to run.
