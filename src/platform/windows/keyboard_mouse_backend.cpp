/**
 * @file src/platform/windows/keyboard_mouse_backend.cpp
 * @brief Selection of optional Windows keyboard and mouse input backends.
 */

#include "keyboard_mouse_backend.h"

#include "src/logging.h"

#include <array>

namespace platf {
  keyboard_mouse_backend_t select_keyboard_mouse_backend(const std::wstring &interception_path) {
    keyboard_mouse_backend_t backend;
    backend.initialize(interception_path);
    return backend;
  }

  keyboard_mouse_backend_t select_keyboard_mouse_backend_from_executable_dir() {
    keyboard_mouse_backend_t backend;
    backend.initialize_from_executable_dir();
    return backend;
  }

  void keyboard_mouse_backend_t::initialize(const std::wstring &interception_path) {
    if (initialized) {
      return;
    }
    interception = interception_input_t::create_from_path(interception_path);
    initialized = true;
  }

  void keyboard_mouse_backend_t::initialize_from_executable_dir() {
    if (initialized) {
      return;
    }
    std::array<wchar_t, 32768> buffer {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
      BOOST_LOG(warning) << "Cannot determine Sunshine executable path for Interception";
      initialized = true;
      return;
    }

    std::wstring directory(buffer.data(), length);
    const auto slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
      BOOST_LOG(warning) << "Cannot determine Sunshine executable directory for Interception";
      initialized = true;
      return;
    }
    directory.resize(slash + 1);
    initialize(directory + L"interception.dll");
  }
}  // namespace platf
