/**
 * @file tests/unit/test_input_activity.cpp
 * @brief Test user activity and force-hold classification helpers.
 */

#include "../tests_common.h"
#include "src/input.h"

#include <chrono>
#include <cstdint>
#include <unordered_set>

TEST(InputMouseBatchTests, AccumulatesRelativeMotionWithoutOverflow) {
  input::detail::relative_mouse_delta_t dest {100, -50};
  const input::detail::relative_mouse_delta_t src {25, 20};

  EXPECT_TRUE(input::detail::batch_relative_mouse(dest, src));
  EXPECT_EQ(dest, (input::detail::relative_mouse_delta_t {125, -30}));
}

TEST(InputMouseBatchTests, RejectsRelativeMotionOverflow) {
  input::detail::relative_mouse_delta_t dest {INT16_MAX, 0};
  const input::detail::relative_mouse_delta_t src {1, 0};

  EXPECT_FALSE(input::detail::batch_relative_mouse(dest, src));
  EXPECT_EQ(dest, (input::detail::relative_mouse_delta_t {INT16_MAX, 0}));
}

TEST(InputForceStateTests, NonEmptySignatureStartsForceTiming) {
  input::timeout_state_t state {};
  auto now = std::chrono::steady_clock::time_point {std::chrono::seconds {10}};

  input::detail::apply_force_signature_change(state, true, now);

  EXPECT_TRUE(state.force_hold_active);
  EXPECT_EQ(state.force_hold_started_at, now);
  EXPECT_EQ(state.last_input_time, now);
}

TEST(InputForceStateTests, ChangedSignatureRestartsForceTiming) {
  input::timeout_state_t state {
    true,
    std::chrono::steady_clock::time_point {std::chrono::seconds {10}},
    {}
  };
  auto changed_at = std::chrono::steady_clock::time_point {std::chrono::seconds {20}};

  input::detail::apply_force_signature_change(state, true, changed_at);

  EXPECT_EQ(state.force_hold_started_at, changed_at);
  EXPECT_EQ(state.last_input_time, changed_at);
}

TEST(InputForceStateTests, EmptySignatureStopsForceAndRestartsStandby) {
  input::timeout_state_t state {
    true,
    std::chrono::steady_clock::time_point {std::chrono::seconds {10}},
    {}
  };
  auto released_at = std::chrono::steady_clock::time_point {std::chrono::seconds {20}};

  input::detail::apply_force_signature_change(state, false, released_at);

  EXPECT_FALSE(state.force_hold_active);
  EXPECT_EQ(state.last_input_time, released_at);
}

TEST(InputForceStateTests, TimeoutStatesAreIndependent) {
  input::timeout_state_t first {};
  input::timeout_state_t second {};
  auto first_started_at = std::chrono::steady_clock::time_point {std::chrono::seconds {10}};
  auto second_started_at = std::chrono::steady_clock::time_point {std::chrono::seconds {20}};

  input::detail::apply_force_signature_change(first, true, first_started_at);
  input::detail::apply_force_signature_change(second, true, second_started_at);

  EXPECT_EQ(first.force_hold_started_at, first_started_at);
  EXPECT_EQ(second.force_hold_started_at, second_started_at);
}

TEST(InputForceStateTests, DigitalHeldSetChangesOnlyOnTransitions) {
  std::unordered_set<std::uint32_t> held;

  EXPECT_TRUE(input::detail::update_digital_force_state(held, 42, true));
  EXPECT_FALSE(input::detail::update_digital_force_state(held, 42, true));
  EXPECT_TRUE(input::detail::update_digital_force_state(held, 42, false));
  EXPECT_FALSE(input::detail::update_digital_force_state(held, 42, false));
  EXPECT_TRUE(held.empty());
}

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

TEST(InputForceStateTests, GamepadDigitalButtonChangesStableSignature) {
  input::detail::gamepad_force_state_t state {};
  platf::gamepad_state_t pressed {platf::A, 0, 0, 0, 0, 0, 0};

  EXPECT_TRUE(input::detail::update_gamepad_force_state(state, true, pressed));
  EXPECT_TRUE(state.has_held_control());
  EXPECT_FALSE(input::detail::update_gamepad_force_state(state, true, pressed));
}

TEST(InputForceStateTests, GamepadAnalogNoiseDoesNotChangeStableSignature) {
  input::detail::gamepad_force_state_t state {};
  platf::gamepad_state_t active {0, 31, 0, 9000, 0, 0, 0};
  platf::gamepad_state_t noisy {0, 25, 0, 7000, 0, 0, 0};

  ASSERT_TRUE(input::detail::update_gamepad_force_state(state, true, active));
  EXPECT_FALSE(input::detail::update_gamepad_force_state(state, true, noisy));
  EXPECT_TRUE(state.has_held_control());
}

TEST(InputForceStateTests, GamepadDirectionChangeChangesStableSignature) {
  input::detail::gamepad_force_state_t state {};
  platf::gamepad_state_t positive {0, 0, 0, 9000, 0, 0, 0};
  platf::gamepad_state_t negative {0, 0, 0, -9000, 0, 0, 0};

  ASSERT_TRUE(input::detail::update_gamepad_force_state(state, true, positive));
  EXPECT_TRUE(input::detail::update_gamepad_force_state(state, true, negative));
  EXPECT_EQ(state.left_x, input::detail::force_axis_e::negative);
}

TEST(InputForceStateTests, InactiveGamepadClearsStableSignature) {
  input::detail::gamepad_force_state_t state {};
  platf::gamepad_state_t pressed {platf::A, 0, 0, 0, 0, 0, 0};

  ASSERT_TRUE(input::detail::update_gamepad_force_state(state, true, pressed));
  EXPECT_TRUE(input::detail::update_gamepad_force_state(state, false, pressed));
  EXPECT_FALSE(state.has_held_control());
}
