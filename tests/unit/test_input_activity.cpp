/**
 * @file tests/unit/test_input_activity.cpp
 * @brief Test user activity classification helpers.
 */

#include "../tests_common.h"
#include "src/input.h"

TEST(InputActivityTests, RepeatedGamepadStateIsNotActivity) {
  platf::gamepad_state_t state {platf::A, 0, 0, 0, 0, 0, 0};

  ASSERT_FALSE(input::detail::is_changed_active_gamepad_state(true, state, state));
}

TEST(InputActivityTests, ChangedActiveGamepadStateIsActivity) {
  platf::gamepad_state_t old_state {};
  platf::gamepad_state_t new_state {platf::A, 0, 0, 0, 0, 0, 0};

  ASSERT_TRUE(input::detail::is_changed_active_gamepad_state(true, old_state, new_state));
}

TEST(InputActivityTests, InactiveGamepadStateIsNotActivity) {
  platf::gamepad_state_t old_state {};
  platf::gamepad_state_t new_state {platf::A, 0, 0, 0, 0, 0, 0};

  ASSERT_FALSE(input::detail::is_changed_active_gamepad_state(false, old_state, new_state));
}
