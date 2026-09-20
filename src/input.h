/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_set>

// local includes
#include "platform/common.h"
#include "thread_safe.h"

namespace input {
  struct input_t;

  struct timeout_state_t {
    bool force_hold_active {};
    std::chrono::steady_clock::time_point force_hold_started_at {};
    std::chrono::steady_clock::time_point last_input_time {};
  };

  namespace detail {
    struct relative_mouse_delta_t {
      std::int16_t x {};
      std::int16_t y {};

      bool operator==(const relative_mouse_delta_t &) const = default;
    };

    bool batch_relative_mouse(relative_mouse_delta_t &dest, const relative_mouse_delta_t &src);

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
      bool operator==(const gamepad_force_state_t &) const = default;
    };

    force_axis_e update_force_axis(force_axis_e previous, std::int16_t value);
    bool update_force_trigger(bool previous, std::uint8_t value);
    bool update_gamepad_force_state(
      gamepad_force_state_t &state,
      bool active,
      const platf::gamepad_state_t &gamepad_state
    );
    bool update_digital_force_state(
      std::unordered_set<std::uint32_t> &held,
      std::uint32_t id,
      bool pressed
    );
    void apply_force_signature_change(
      timeout_state_t &state,
      bool force_hold_active,
      std::chrono::steady_clock::time_point now
    );
  }  // namespace detail

  void print(void *input);
  void reset(std::shared_ptr<input_t> &input);
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data);

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  bool probe_gamepads();

  extern std::bitset<platf::MAX_GAMEPADS> gamepadMask;
  extern std::bitset<platf::MAX_GAMEPADS> preinitializedGamepadMask;

  std::shared_ptr<input_t> alloc(safe::mail_t mail);

  /**
   * @brief Inject a single button click on a gamepad slot (press → sleep → release).
   * @param gamepad_nr The gamepad slot number (0 for first slot).
   * @param button_flag The platf:: button flag (e.g. platf::A).
   */
  void click_gamepad(int gamepad_nr, std::uint32_t button_flag);

  struct touch_port_t: public platf::touch_port_t {
    int env_width;
    int env_height;

    // Offset x and y coordinates of the client
    float client_offsetX;
    float client_offsetY;

    float scalar_inv;
    float scalar_tpcoords;

    int env_logical_width;
    int env_logical_height;

    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);

  /**
   * @brief Update a session's last real user input timestamp.
   * @param input The session input context.
   */
  void update_input_time(const std::shared_ptr<input_t> &input);

  /**
   * @brief Reset a session's force-hold and standby timing state.
   * @param input The session input context.
   */
  void reset_timeout_state(const std::shared_ptr<input_t> &input);

  /**
   * @brief Get a consistent snapshot of a session's timeout state.
   * @param input The session input context.
   * @return The force-hold and standby timing state.
   */
  timeout_state_t get_timeout_state(const std::shared_ptr<input_t> &input);
}  // namespace input
