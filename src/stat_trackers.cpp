/**
 * @file src/stat_trackers.cpp
 * @brief 串流统计跟踪的实现
 * 提供数字格式化工具函数
 */
#include "stat_trackers.h"

namespace stat_trackers {

  /**
   * @brief 创建一位小数的格式化器
   */
  boost::format one_digit_after_decimal() {
    return boost::format("%1$.1f");
  }

  boost::format two_digits_after_decimal() {
    return boost::format("%1$.2f");
  }

}  // namespace stat_trackers
