/**
 * @file src/platform/windows/interception_input.h
 * @brief Optional Interception keyboard and mouse input backend.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <windows.h>

namespace platf {
  class interception_input_t {
  public:
    static std::unique_ptr<interception_input_t> create_from_executable_dir();
    static std::unique_ptr<interception_input_t> create_from_path(const std::wstring &path);

    interception_input_t(const interception_input_t &) = delete;
    interception_input_t &operator=(const interception_input_t &) = delete;
    ~interception_input_t();

    bool key(std::uint16_t scan_code, std::uint16_t state);
    bool move_relative(int dx, int dy);
    bool button(int button, bool release);
    bool wheel(int distance, bool horizontal);

  private:
    using context_t = void *;
    using create_context_fn = context_t(__cdecl *)();
    using destroy_context_fn = void(__cdecl *)(context_t);
    using get_hardware_id_fn = unsigned int(__cdecl *)(context_t, int, void *, unsigned int);
    using send_fn = int(__cdecl *)(context_t, int, const void *, unsigned int);

    explicit interception_input_t(HMODULE module);

    bool resolve_exports();
    bool select_devices();
    bool send(int device, const void *stroke);

    HMODULE module_ = nullptr;
    context_t context_ = nullptr;
    create_context_fn create_context_ = nullptr;
    destroy_context_fn destroy_context_ = nullptr;
    get_hardware_id_fn get_hardware_id_ = nullptr;
    send_fn send_ = nullptr;
    int keyboard_device_ = 0;
    int mouse_device_ = 0;
    std::mutex mutex_;
    std::uint64_t send_failures_ = 0;
  };
}  // namespace platf
