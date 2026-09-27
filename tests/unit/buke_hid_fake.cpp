#include <string>
#include <windows.h>

namespace {
  std::string events;
  bool init_result = true;

  void record(const char *event) {
    events += event;
    events += ';';
  }

  void record_move(const char *kind, int x, int y) {
    events += kind;
    events += ':';
    events += std::to_string(x);
    events += ',';
    events += std::to_string(y);
    events += ';';
  }
}  // namespace

extern "C" {
  __declspec(dllexport) void __cdecl TestReset() {
    events.clear();
  }

  __declspec(dllexport) void __cdecl TestSetInitResult(bool value) {
    init_result = value;
  }

  __declspec(dllexport) const char *__cdecl TestReadEvents() {
    return events.c_str();
  }

  __declspec(dllexport) bool __cdecl Init(unsigned int, unsigned char) {
    record("I");
    return init_result;
  }

  __declspec(dllexport) void __cdecl MouseMove(int x, int y) {
    record_move("A", x, y);
  }

  __declspec(dllexport) void __cdecl MouseMoveR(int x, int y) {
    record_move("R", x, y);
  }

  __declspec(dllexport) void __cdecl MouseLeftDown() {
    record("L+");
  }

  __declspec(dllexport) void __cdecl MouseLeftUp() {
    record("L-");
  }

  __declspec(dllexport) void __cdecl MouseMiddleDown() {
    record("M+");
  }

  __declspec(dllexport) void __cdecl MouseMiddleUp() {
    record("M-");
  }

  __declspec(dllexport) void __cdecl MouseRightDown() {
    record("B+");
  }

  __declspec(dllexport) void __cdecl MouseRightUp() {
    record("B-");
  }

  __declspec(dllexport) void __cdecl Mouse4Down() {
    record("X1+");
  }

  __declspec(dllexport) void __cdecl Mouse4Up() {
    record("X1-");
  }

  __declspec(dllexport) void __cdecl Mouse5Down() {
    record("X2+");
  }

  __declspec(dllexport) void __cdecl Mouse5Up() {
    record("X2-");
  }

  __declspec(dllexport) void __cdecl MouseWheelUp() {
    record("WU");
  }

  __declspec(dllexport) void __cdecl MouseWheelDown() {
    record("WD");
  }

  __declspec(dllexport) void __cdecl KeyboardDown(unsigned short vk) {
    record_move("K+", vk, 0);
  }

#ifndef BUKE_OMIT_KEYBOARD_UP
  __declspec(dllexport) void __cdecl KeyboardUp(unsigned short vk) {
    record_move("K-", vk, 0);
  }
#endif
}
