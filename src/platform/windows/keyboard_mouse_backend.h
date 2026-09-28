/**
 * @file src/platform/windows/keyboard_mouse_backend.h
 * @brief Selection of optional Windows keyboard and mouse input backends.
 */
#pragma once

#include "buke_hid_input.h"
#include "interception_input.h"

#include <memory>
#include <string>

namespace platf {
  struct keyboard_mouse_backend_t {
    std::unique_ptr<interception_input_t> interception;
    std::unique_ptr<buke_hid_input_t> buke_hid;
  };

  keyboard_mouse_backend_t select_keyboard_mouse_backend(const std::wstring &interception_path, const std::wstring &buke_path);
  keyboard_mouse_backend_t select_keyboard_mouse_backend_from_executable_dir();
}  // namespace platf
