/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <functional>

// local includes
#include "platform/common.h"
#include "thread_safe.h"

namespace input {
  struct input_t;

  namespace detail {
    bool is_changed_active_gamepad_state(
      bool active,
      const platf::gamepad_state_t &old_state,
      const platf::gamepad_state_t &new_state
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
   * @brief Get the time point of a session's last real user input.
   * @param input The session input context.
   * @return The steady_clock time point of the last input event.
   */
  std::chrono::steady_clock::time_point get_last_input_time(const std::shared_ptr<input_t> &input);
}  // namespace input
