/**
 * @file src/platform/linux/misc.h
 * @brief Linux平台杂项声明。包括文件描述符工具、网络、进程管理等工具函数。
 */
#pragma once

// standard includes
#include <unistd.h>
#include <vector>

// local includes
#include "src/utility.h"

KITTY_USING_MOVE_T(file_t, int, -1, {
  if (el >= 0) {
    close(el);
  }
});

enum class window_system_e {
  NONE,  ///< No window system
  X11,  ///< X11
  WAYLAND,  ///< Wayland
};

extern window_system_e window_system;

namespace dyn {
  typedef void (*apiproc)(void);

  int load(void *handle, const std::vector<std::tuple<apiproc *, const char *>> &funcs, bool strict = true);
  void *handle(const std::vector<const char *> &libs);

}  // namespace dyn
