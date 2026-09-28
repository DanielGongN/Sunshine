#ifdef _WIN32

  #include <cstdint>
  #include <mutex>
  #include <string>

namespace {
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

  std::mutex events_mutex;
  std::string events;
  bool context_result = true;
  bool keyboard_available = true;
  bool mouse_available = true;
}  // namespace

extern "C" __declspec(dllexport) void TestReset() {
  std::lock_guard lock(events_mutex);
  events.clear();
  context_result = true;
  keyboard_available = true;
  mouse_available = true;
}

extern "C" __declspec(dllexport) void TestSetContextResult(bool result) {
  context_result = result;
}

extern "C" __declspec(dllexport) void TestSetDeviceAvailability(bool keyboard, bool mouse) {
  keyboard_available = keyboard;
  mouse_available = mouse;
}

extern "C" __declspec(dllexport) const char *TestReadEvents() {
  std::lock_guard lock(events_mutex);
  return events.c_str();
}

extern "C" __declspec(dllexport) void *interception_create_context() {
  return context_result ? reinterpret_cast<void *>(1) : nullptr;
}

extern "C" __declspec(dllexport) void interception_destroy_context(void *) {}

extern "C" __declspec(dllexport) unsigned int interception_get_hardware_id(void *, int device, void *buffer, unsigned int size) {
  if (size < sizeof(wchar_t) * 2 || !buffer || !((keyboard_available && device == 1) || (mouse_available && device == 11))) {
    return 0;
  }

  auto *id = static_cast<wchar_t *>(buffer);
  id[0] = L'X';
  id[1] = L'\0';
  return sizeof(wchar_t) * 2;
}

  #ifndef INTERCEPTION_FAKE_OMIT_SEND
extern "C" __declspec(dllexport) int interception_send(void *, int device, const void *stroke, unsigned int count) {
  if (!stroke || count != 1) {
    return 0;
  }

  std::lock_guard lock(events_mutex);
  if (device <= 10) {
    const auto &key = *static_cast<const key_stroke_t *>(stroke);
    events += "K:" + std::to_string(device) + ',' + std::to_string(key.code) + ',' + std::to_string(key.state) + ';';
  } else {
    const auto &mouse = *static_cast<const mouse_stroke_t *>(stroke);
    events += "M:" + std::to_string(device) + ',' + std::to_string(mouse.state) + ',' + std::to_string(mouse.flags) + ',' +
              std::to_string(mouse.rolling) + ',' + std::to_string(mouse.x) + ',' + std::to_string(mouse.y) + ';';
  }
  return 1;
}
  #endif

#endif
