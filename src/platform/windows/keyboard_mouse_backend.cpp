/**
 * @file src/platform/windows/keyboard_mouse_backend.cpp
 * @brief Selection of optional Windows keyboard and mouse input backends.
 */

#include "keyboard_mouse_backend.h"

#include "src/logging.h"

#include <array>

namespace platf {
  keyboard_mouse_backend_t select_keyboard_mouse_backend(const std::wstring &interception_path, const std::wstring &buke_path) {
    keyboard_mouse_backend_t backend;
    backend.interception = interception_input_t::create_from_path(interception_path);
    if (!backend.interception) {
      backend.buke_hid = buke_hid_input_t::create_from_path(buke_path);
    }
    return backend;
  }

  keyboard_mouse_backend_t select_keyboard_mouse_backend_from_executable_dir() {
    std::array<wchar_t, 32768> buffer {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
      BOOST_LOG(warning) << "Cannot determine Sunshine executable path for optional keyboard and mouse backends";
      return {};
    }

    std::wstring directory(buffer.data(), length);
    const auto slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
      BOOST_LOG(warning) << "Cannot determine Sunshine executable directory for optional keyboard and mouse backends";
      return {};
    }
    directory.resize(slash + 1);
    return select_keyboard_mouse_backend(directory + L"interception.dll", directory + L"buke_km_hid64.dll");
  }
}  // namespace platf
