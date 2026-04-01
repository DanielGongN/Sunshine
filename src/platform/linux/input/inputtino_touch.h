/**
 * @file src/platform/linux/input/inputtino_touch.h
 * @brief inputtino触摸输入处理声明。定义虚拟触摸屏设备接口。
 */
#pragma once

// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

// local includes
#include "inputtino_common.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace platf::touch {
  void update(client_input_raw_t *raw, const touch_port_t &touch_port, const touch_input_t &touch);
}
