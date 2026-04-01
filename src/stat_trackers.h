/**
 * @file src/stat_trackers.h
 * @brief 串流统计跟踪的声明
 * 提供最小值/最大值/平均值等统计跟踪器，用于监控串流性能
 */
#pragma once

// 标准库头文件
#include <chrono>
#include <functional>
#include <limits>

// 第三方库头文件
#include <boost/format.hpp> // 字符串格式化

namespace stat_trackers {

  boost::format one_digit_after_decimal();  // 一位小数格式化器

  boost::format two_digits_after_decimal(); // 两位小数格式化器

  /**
   * @brief 最小值/最大值/平均值统计跟踪器
   * 在指定时间间隔内收集样本，到期后调用回调函数输出统计结果
   */
  template<typename T>
  class min_max_avg_tracker {
  public:
    using callback_function = std::function<void(T stat_min, T stat_max, double stat_avg)>;

    void collect_and_callback_on_interval(T stat, const callback_function &callback, std::chrono::seconds interval_in_seconds) {
      if (data.calls == 0) {
        data.last_callback_time = std::chrono::steady_clock::now();
      } else if (std::chrono::steady_clock::now() > data.last_callback_time + interval_in_seconds) {
        callback(data.stat_min, data.stat_max, data.stat_total / data.calls);
        data = {};
      }
      data.stat_min = std::min(data.stat_min, stat);
      data.stat_max = std::max(data.stat_max, stat);
      data.stat_total += stat;
      data.calls += 1;
    }

    void reset() {
      data = {};
    }

  private:
    struct {
      std::chrono::steady_clock::time_point last_callback_time = std::chrono::steady_clock::now();
      T stat_min = std::numeric_limits<T>::max();
      T stat_max = std::numeric_limits<T>::min();
      double stat_total = 0;
      uint32_t calls = 0;
    } data;
  };

}  // namespace stat_trackers
