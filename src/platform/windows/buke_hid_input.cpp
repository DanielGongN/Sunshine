/**
 * @file src/platform/windows/buke_hid_input.cpp
 * @brief Optional Buke HID keyboard and mouse input backend.
 */

#include "buke_hid_input.h"

#include "src/logging.h"

#include <array>

namespace platf {
  namespace {
    constexpr wchar_t dll_name[] = L"buke_km_hid64.dll";

    template<class Function>
    bool resolve_export(HMODULE module, const char *name, Function &function) {
      function = reinterpret_cast<Function>(GetProcAddress(module, name));
      if (!function) {
        BOOST_LOG(warning) << "Buke HID DLL is missing export " << name;
      }
      return function != nullptr;
    }
  }  // namespace

  buke_hid_input_t::buke_hid_input_t(HMODULE module):
      module_(module) {}

  buke_hid_input_t::~buke_hid_input_t() {
    if (module_) {
      FreeLibrary(module_);
    }
  }

  std::unique_ptr<buke_hid_input_t> buke_hid_input_t::create_from_executable_dir() {
    std::array<wchar_t, 32768> buffer {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
      BOOST_LOG(warning) << "Cannot determine Sunshine executable path for Buke HID";
      return nullptr;
    }

    std::wstring path(buffer.data(), length);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
      BOOST_LOG(warning) << "Cannot determine Sunshine executable directory for Buke HID";
      return nullptr;
    }

    path.resize(slash + 1);
    path += dll_name;
    return create_from_path(path);
  }

  std::unique_ptr<buke_hid_input_t> buke_hid_input_t::create_from_path(const std::wstring &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
      BOOST_LOG(info) << "Buke HID DLL unavailable; using Windows input";
      return nullptr;
    }

    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
      BOOST_LOG(warning) << "Cannot load Buke HID DLL (Windows error " << GetLastError() << ')';
      return nullptr;
    }

    auto result = std::unique_ptr<buke_hid_input_t>(new buke_hid_input_t(module));
    if (!result->resolve_exports()) {
      BOOST_LOG(warning) << "Buke HID DLL has incomplete exports; using Windows input";
      return nullptr;
    }
    if (!result->init_(0, 0)) {
      BOOST_LOG(warning) << "Buke HID Init failed; using Windows input";
      return nullptr;
    }

    BOOST_LOG(info) << "Buke HID keyboard and mouse input enabled";
    return result;
  }

  bool buke_hid_input_t::resolve_exports() {
    return resolve_export(module_, "Init", init_) &&
           resolve_export(module_, "MouseMove", mouse_move_) &&
           resolve_export(module_, "MouseMoveR", mouse_move_r_) &&
           resolve_export(module_, "MouseLeftDown", mouse_left_down_) &&
           resolve_export(module_, "MouseLeftUp", mouse_left_up_) &&
           resolve_export(module_, "MouseMiddleDown", mouse_middle_down_) &&
           resolve_export(module_, "MouseMiddleUp", mouse_middle_up_) &&
           resolve_export(module_, "MouseRightDown", mouse_right_down_) &&
           resolve_export(module_, "MouseRightUp", mouse_right_up_) &&
           resolve_export(module_, "Mouse4Down", mouse_4_down_) &&
           resolve_export(module_, "Mouse4Up", mouse_4_up_) &&
           resolve_export(module_, "Mouse5Down", mouse_5_down_) &&
           resolve_export(module_, "Mouse5Up", mouse_5_up_) &&
           resolve_export(module_, "MouseWheelUp", mouse_wheel_up_) &&
           resolve_export(module_, "MouseWheelDown", mouse_wheel_down_) &&
           resolve_export(module_, "KeyboardDown", keyboard_down_) &&
           resolve_export(module_, "KeyboardUp", keyboard_up_);
  }

  void buke_hid_input_t::key(std::uint16_t vk, bool release) {
    std::lock_guard lock(mutex_);
    (release ? keyboard_up_ : keyboard_down_)(vk);
  }

  void buke_hid_input_t::move_relative(int dx, int dy) {
    std::lock_guard lock(mutex_);
    mouse_move_r_(dx, dy);
  }

  void buke_hid_input_t::move_absolute(int x, int y) {
    std::lock_guard lock(mutex_);
    mouse_move_(x, y);
  }

  void buke_hid_input_t::button(int button, bool release) {
    std::lock_guard lock(mutex_);
    button_fn function;
    switch (button) {
      case 1:
        function = release ? mouse_left_up_ : mouse_left_down_;
        break;
      case 2:
        function = release ? mouse_middle_up_ : mouse_middle_down_;
        break;
      case 3:
        function = release ? mouse_right_up_ : mouse_right_down_;
        break;
      case 4:
        function = release ? mouse_4_up_ : mouse_4_down_;
        break;
      default:
        function = release ? mouse_5_up_ : mouse_5_down_;
        break;
    }
    function();
  }

  void buke_hid_input_t::wheel(int distance) {
    std::lock_guard lock(mutex_);
    wheel_remainder_ += distance;
    while (wheel_remainder_ >= WHEEL_DELTA) {
      mouse_wheel_up_();
      wheel_remainder_ -= WHEEL_DELTA;
    }
    while (wheel_remainder_ <= -WHEEL_DELTA) {
      mouse_wheel_down_();
      wheel_remainder_ += WHEEL_DELTA;
    }
  }
}  // namespace platf
