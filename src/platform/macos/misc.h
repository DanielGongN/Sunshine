/**
 * @file src/platform/macos/misc.h
 * @brief macOS平台杂项声明。包括网络、进程管理、线程等工具函数。
 */
#pragma once

// standard includes
#include <vector>

// platform includes
#include <CoreGraphics/CoreGraphics.h>

namespace platf {
  bool is_screen_capture_allowed();
}

namespace dyn {
  typedef void (*apiproc)();

  int load(void *handle, const std::vector<std::tuple<apiproc *, const char *>> &funcs, bool strict = true);
  void *handle(const std::vector<const char *> &libs);

}  // namespace dyn
