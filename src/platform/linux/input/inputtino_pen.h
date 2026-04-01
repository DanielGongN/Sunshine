/**
 * @file src/platform/linux/input/inputtino_pen.h
 * @brief inputtino触笔输入处理声明。定义虚拟触笔设备接口。
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

namespace platf::pen {
  void update(client_input_raw_t *raw, const touch_port_t &touch_port, const pen_input_t &pen);
}
