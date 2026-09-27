/**
 * @file src/platform/windows/buke_hid_input.h
 * @brief Optional Buke HID keyboard and mouse input backend.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <Windows.h>

namespace platf {
  class buke_hid_input_t {
  public:
    static std::unique_ptr<buke_hid_input_t> create_from_executable_dir();
    static std::unique_ptr<buke_hid_input_t> create_from_path(const std::wstring &path);

    buke_hid_input_t(const buke_hid_input_t &) = delete;
    buke_hid_input_t &operator=(const buke_hid_input_t &) = delete;
    ~buke_hid_input_t();

    void key(std::uint16_t vk, bool release);
    void move_relative(int dx, int dy);
    void move_absolute(int x, int y);
    void button(int button, bool release);
    void wheel(int distance);

  private:
    using init_fn = bool(__cdecl *)(unsigned int, unsigned char);
    using move_fn = void(__cdecl *)(int, int);
    using button_fn = void(__cdecl *)();
    using keyboard_fn = void(__cdecl *)(unsigned short);

    explicit buke_hid_input_t(HMODULE module);
    bool resolve_exports();

    HMODULE module_;
    std::mutex mutex_;
    std::int64_t wheel_remainder_ = 0;

    init_fn init_ = nullptr;
    move_fn mouse_move_ = nullptr;
    move_fn mouse_move_r_ = nullptr;
    button_fn mouse_left_down_ = nullptr;
    button_fn mouse_left_up_ = nullptr;
    button_fn mouse_middle_down_ = nullptr;
    button_fn mouse_middle_up_ = nullptr;
    button_fn mouse_right_down_ = nullptr;
    button_fn mouse_right_up_ = nullptr;
    button_fn mouse_4_down_ = nullptr;
    button_fn mouse_4_up_ = nullptr;
    button_fn mouse_5_down_ = nullptr;
    button_fn mouse_5_up_ = nullptr;
    button_fn mouse_wheel_up_ = nullptr;
    button_fn mouse_wheel_down_ = nullptr;
    keyboard_fn keyboard_down_ = nullptr;
    keyboard_fn keyboard_up_ = nullptr;
  };
}  // namespace platf
