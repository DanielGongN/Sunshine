/**
 * @file src/platform/windows/interception_input.cpp
 * @brief Optional Interception keyboard and mouse input backend.
 */

#include "interception_input.h"

#include "src/logging.h"

#include <algorithm>
#include <array>
#include <limits>

namespace platf {
  namespace {
    constexpr wchar_t dll_name[] = L"interception.dll";

    // These layouts and flags match the public Interception C ABI.
    struct key_stroke_t {
      std::uint16_t code;
      std::uint16_t state;
      std::uint32_t information;
    };

    struct mouse_stroke_t {
      std::uint16_t state;
      std::uint16_t flags;
      std::int16_t rolling;
      std::int32_t x;
      std::int32_t y;
      std::uint32_t information;
    };

    static_assert(sizeof(key_stroke_t) == 8);
    static_assert(sizeof(mouse_stroke_t) == 20);

    constexpr std::uint16_t mouse_left_down = 0x001;
    constexpr std::uint16_t mouse_left_up = 0x002;
    constexpr std::uint16_t mouse_right_down = 0x004;
    constexpr std::uint16_t mouse_right_up = 0x008;
    constexpr std::uint16_t mouse_middle_down = 0x010;
    constexpr std::uint16_t mouse_middle_up = 0x020;
    constexpr std::uint16_t mouse_x1_down = 0x040;
    constexpr std::uint16_t mouse_x1_up = 0x080;
    constexpr std::uint16_t mouse_x2_down = 0x100;
    constexpr std::uint16_t mouse_x2_up = 0x200;
    constexpr std::uint16_t mouse_wheel = 0x400;
    constexpr std::uint16_t mouse_hwheel = 0x800;

    template<class Function>
    bool resolve_export(HMODULE module, const char *name, Function &function) {
      function = reinterpret_cast<Function>(GetProcAddress(module, name));
      if (!function) {
        BOOST_LOG(warning) << "Interception DLL is missing export " << name;
      }
      return function != nullptr;
    }
  }  // namespace

  interception_input_t::interception_input_t(HMODULE module):
      module_(module) {}

  interception_input_t::~interception_input_t() {
    if (context_ && destroy_context_) {
      destroy_context_(context_);
    }
    if (module_) {
      FreeLibrary(module_);
    }
  }

  std::unique_ptr<interception_input_t> interception_input_t::create_from_executable_dir() {
    std::array<wchar_t, 32768> buffer {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
      BOOST_LOG(warning) << "Cannot determine Sunshine executable path for Interception";
      return nullptr;
    }

    std::wstring path(buffer.data(), length);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
      BOOST_LOG(warning) << "Cannot determine Sunshine executable directory for Interception";
      return nullptr;
    }

    path.resize(slash + 1);
    path += dll_name;
    return create_from_path(path);
  }

  std::unique_ptr<interception_input_t> interception_input_t::create_from_path(const std::wstring &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
      BOOST_LOG(info) << "Interception DLL unavailable; trying Buke HID";
      return nullptr;
    }

    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
      BOOST_LOG(warning) << "Cannot load Interception DLL (Windows error " << GetLastError() << ')';
      return nullptr;
    }

    auto result = std::unique_ptr<interception_input_t>(new interception_input_t(module));
    if (!result->resolve_exports()) {
      BOOST_LOG(warning) << "Interception DLL has incomplete exports; trying Buke HID";
      return nullptr;
    }

    result->context_ = result->create_context_();
    if (!result->context_) {
      BOOST_LOG(warning) << "Interception driver context unavailable; trying Buke HID";
      return nullptr;
    }
    if (!result->select_devices()) {
      BOOST_LOG(warning) << "Interception keyboard or mouse device unavailable; trying Buke HID";
      return nullptr;
    }

    BOOST_LOG(info) << "Interception keyboard and mouse input enabled";
    return result;
  }

  bool interception_input_t::resolve_exports() {
    return resolve_export(module_, "interception_create_context", create_context_) &&
           resolve_export(module_, "interception_destroy_context", destroy_context_) &&
           resolve_export(module_, "interception_get_hardware_id", get_hardware_id_) &&
           resolve_export(module_, "interception_send", send_);
  }

  bool interception_input_t::select_devices() {
    std::array<wchar_t, 256> hardware_id {};
    for (int device = 1; device <= 10; ++device) {
      if (get_hardware_id_(context_, device, hardware_id.data(), static_cast<unsigned int>(hardware_id.size() * sizeof(wchar_t))) > 0) {
        keyboard_device_ = device;
        break;
      }
    }
    for (int device = 11; device <= 20; ++device) {
      if (get_hardware_id_(context_, device, hardware_id.data(), static_cast<unsigned int>(hardware_id.size() * sizeof(wchar_t))) > 0) {
        mouse_device_ = device;
        break;
      }
    }
    return keyboard_device_ != 0 && mouse_device_ != 0;
  }

  bool interception_input_t::send(int device, const void *stroke) {
    std::lock_guard lock(mutex_);
    if (send_(context_, device, stroke, 1) == 1) {
      return true;
    }
    ++send_failures_;
    if (send_failures_ == 1 || send_failures_ % 100 == 0) {
      BOOST_LOG(error) << "Interception input send failed (count " << send_failures_ << ')';
    }
    return false;
  }

  bool interception_input_t::key(std::uint16_t scan_code, std::uint16_t state) {
    const key_stroke_t stroke {scan_code, state, 0};
    return send(keyboard_device_, &stroke);
  }

  bool interception_input_t::move_relative(int dx, int dy) {
    if (dx == 0 && dy == 0) {
      return true;
    }
    const mouse_stroke_t stroke {0, 0, 0, dx, dy, 0};
    return send(mouse_device_, &stroke);
  }

  bool interception_input_t::button(int button, bool release) {
    std::uint16_t state;
    switch (button) {
      case 1:
        state = release ? mouse_left_up : mouse_left_down;
        break;
      case 2:
        state = release ? mouse_middle_up : mouse_middle_down;
        break;
      case 3:
        state = release ? mouse_right_up : mouse_right_down;
        break;
      case 4:
        state = release ? mouse_x1_up : mouse_x1_down;
        break;
      default:
        state = release ? mouse_x2_up : mouse_x2_down;
        break;
    }
    const mouse_stroke_t stroke {state, 0, 0, 0, 0, 0};
    return send(mouse_device_, &stroke);
  }

  bool interception_input_t::wheel(int distance, bool horizontal) {
    while (distance != 0) {
      const int step = std::clamp(distance, static_cast<int>(std::numeric_limits<std::int16_t>::min()), static_cast<int>(std::numeric_limits<std::int16_t>::max()));
      const mouse_stroke_t stroke {horizontal ? mouse_hwheel : mouse_wheel, 0, static_cast<std::int16_t>(step), 0, 0, 0};
      if (!send(mouse_device_, &stroke)) {
        return false;
      }
      distance -= step;
    }
    return true;
  }
}  // namespace platf
