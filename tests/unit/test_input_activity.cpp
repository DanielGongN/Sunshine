/**
 * @file tests/unit/test_input_activity.cpp
 * @brief Test user activity and force-hold classification helpers.
 */

#include "../tests_common.h"
#include "src/input.h"
#include "src/utility.h"

extern "C" {
#include <moonlight-common-c/src/Input.h>
}

#include <chrono>
#include <cstdint>
#include <cstring>
#include <list>
#include <unordered_set>
#include <vector>

namespace {
  std::vector<std::uint8_t> make_relative_mouse_packet(std::int16_t x, std::int16_t y) {
    NV_REL_MOUSE_MOVE_PACKET packet {};
    packet.header.magic = util::endian::little<std::uint32_t>(MOUSE_MOVE_REL_MAGIC_GEN5);
    packet.deltaX = util::endian::big(x);
    packet.deltaY = util::endian::big(y);
    std::vector<std::uint8_t> bytes(sizeof(packet));
    std::memcpy(bytes.data(), &packet, sizeof(packet));
    return bytes;
  }

  std::vector<std::uint8_t> make_mouse_button_packet() {
    NV_MOUSE_BUTTON_PACKET packet {};
    packet.header.magic = util::endian::little<std::uint32_t>(MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5);
    std::vector<std::uint8_t> bytes(sizeof(packet));
    std::memcpy(bytes.data(), &packet, sizeof(packet));
    return bytes;
  }

  std::vector<std::uint8_t> make_keyboard_packet() {
    NV_KEYBOARD_PACKET packet {};
    packet.header.magic = util::endian::little<std::uint32_t>(KEY_DOWN_EVENT_MAGIC);
    std::vector<std::uint8_t> bytes(sizeof(packet));
    std::memcpy(bytes.data(), &packet, sizeof(packet));
    return bytes;
  }
}  // namespace

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

TEST(InputMouseBatchTests, SendInputPreservesMouseMotion) {
  EXPECT_TRUE(input::detail::preserve_mouse_move(MOUSE_MOVE_REL_MAGIC_GEN5, true));
  EXPECT_TRUE(input::detail::preserve_mouse_move(MOUSE_MOVE_ABS_MAGIC, true));
  EXPECT_FALSE(input::detail::preserve_mouse_move(MOUSE_MOVE_REL_MAGIC_GEN5, false));
  EXPECT_FALSE(input::detail::preserve_mouse_move(MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5, true));
}

TEST(InputMouseBatchTests, SendInputBatchKeepsReversalsAndStopsAtButton) {
  std::list<std::vector<std::uint8_t>> queue;
  for (int index = 0; index < 8; ++index) {
    queue.push_back(make_relative_mouse_packet(index % 2 == 0 ? 8 : -8, 0));
  }
  queue.push_back(make_mouse_button_packet());
  queue.push_back(make_relative_mouse_packet(5, 0));

  const auto packets = input::detail::take_sendinput_relative_batch(queue);
  ASSERT_EQ(packets.size(), 8);
  for (std::size_t index = 0; index < packets.size(); ++index) {
    const auto *packet = reinterpret_cast<const NV_REL_MOUSE_MOVE_PACKET *>(packets[index].data());
    EXPECT_EQ(util::endian::big(packet->deltaX), index % 2 == 0 ? 8 : -8);
  }
  ASSERT_EQ(queue.size(), 2);
  const auto *barrier = reinterpret_cast<const NV_INPUT_HEADER *>(queue.front().data());
  EXPECT_EQ(util::endian::little(barrier->magic), MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5);
}

TEST(InputMouseBatchTests, SendInputBatchWaitsForEightAlreadyQueuedMoves) {
  std::list<std::vector<std::uint8_t>> queue;
  for (int index = 0; index < 7; ++index) {
    queue.push_back(make_relative_mouse_packet(1, 0));
  }
  queue.push_back(make_mouse_button_packet());

  EXPECT_TRUE(input::detail::take_sendinput_relative_batch(queue).empty());
  EXPECT_EQ(queue.size(), 8);
}

TEST(InputMouseBatchTests, SendInputBatchDoesNotCrossKeyboardEvent) {
  std::list<std::vector<std::uint8_t>> queue;
  for (int index = 0; index < 8; ++index) {
    queue.push_back(make_relative_mouse_packet(1, 0));
  }
  queue.push_back(make_keyboard_packet());
  queue.push_back(make_relative_mouse_packet(2, 0));

  EXPECT_EQ(input::detail::take_sendinput_relative_batch(queue).size(), 8);
  ASSERT_EQ(queue.size(), 2);
  const auto *barrier = reinterpret_cast<const NV_INPUT_HEADER *>(queue.front().data());
  EXPECT_EQ(util::endian::little(barrier->magic), KEY_DOWN_EVENT_MAGIC);
}

TEST(InputMouseBatchTests, SendInputBatchCapsAtThirtyTwoMoves) {
  std::list<std::vector<std::uint8_t>> queue;
  for (int index = 0; index < 40; ++index) {
    queue.push_back(make_relative_mouse_packet(index + 1, 0));
  }

  const auto packets = input::detail::take_sendinput_relative_batch(queue);
  ASSERT_EQ(packets.size(), 32);
  EXPECT_EQ(queue.size(), 8);
  const auto *next = reinterpret_cast<const NV_REL_MOUSE_MOVE_PACKET *>(queue.front().data());
  EXPECT_EQ(util::endian::big(next->deltaX), 33);
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
