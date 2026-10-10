/**
 * @file src/platform/windows/keyboard_mouse_backend.h
 * @brief Selection of optional Windows keyboard and mouse input backends.
 */
#pragma once

#include "interception_input.h"

#include <memory>
#include <string>

namespace platf {
  struct keyboard_mouse_backend_t {
    std::unique_ptr<interception_input_t> interception;
  };

  keyboard_mouse_backend_t select_keyboard_mouse_backend(const std::wstring &interception_path);
  keyboard_mouse_backend_t select_keyboard_mouse_backend_from_executable_dir();
}  // namespace platf
