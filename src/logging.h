/**
 * @file src/logging.h
 * @brief 日志系统相关函数的声明
 * 基于Boost.Log实现，支持多级日志（verbose/debug/info/warning/error/fatal）
 */
#pragma once

// 第三方库头文件
#include <boost/log/common.hpp>
#include <boost/log/sinks.hpp>

// 异步文本输出日志接收器类型定义
using text_sink = boost::log::sinks::asynchronous_sink<boost::log::sinks::text_ostream_backend>;

// 各级别日志器的外部声明（全局可用）
extern boost::log::sources::severity_logger<int> verbose;  // 级别0：详尽输出（大量信息）
extern boost::log::sources::severity_logger<int> debug;    // 级别1：调试跟踪
extern boost::log::sources::severity_logger<int> info;     // 级别2：一般信息通知
extern boost::log::sources::severity_logger<int> warning;  // 级别3：异常事件警告
extern boost::log::sources::severity_logger<int> error;    // 级别4：可恢复的错误
extern boost::log::sources::severity_logger<int> fatal;    // 级别5：不可恢复的致命错误
#ifdef SUNSHINE_TESTS
extern boost::log::sources::severity_logger<int> tests;    // 级别10：自动化测试输出
#endif

#include "config.h"
#include "stat_trackers.h"

/**
 * @brief 日志系统命名空间
 * 负责日志系统的初始化、反初始化、格式化输出，以及统计日志辅助类
 */
namespace logging {
  class deinit_t {
  public:
    /**
     * @brief 析构函数，作用域退出时自动反初始化日志系统（RAII模式）
     */
    ~deinit_t();
  };

  /**
   * @brief 反初始化日志系统，刷新缓冲区并移除日志接收器
   */
  void deinit();

  /**
   * @brief 日志记录格式化函数
   * 将日志记录格式化为 "[时间戳]: 级别: 消息" 的格式
   */
  void formatter(const boost::log::record_view &view, boost::log::formatting_ostream &os);

  /**
   * @brief 初始化日志系统
   * @param min_log_level 最低日志输出级别（0=verbose, 1=debug, 2=info, 3=warning, 4=error, 5=fatal）
   * @param log_file 日志文件输出路径
   * @return RAII守卫对象，离开作用域时自动反初始化日志系统
   */
  [[nodiscard]] std::unique_ptr<deinit_t> init(int min_log_level, const std::string &log_file);

  /**
   * @brief 配置FFmpeg（libav）的日志级别和回调，将FFmpeg日志重定向到Sunshine日志系统
   * @param min_log_level 最低日志级别
   */
  void setup_av_logging(int min_log_level);

  /**
   * @brief 配置libdisplaydevice库的日志，将其重定向到Sunshine日志系统
   * @param min_log_level 最低日志级别
   */
  void setup_libdisplaydevice_logging(int min_log_level);

  /**
   * @brief 刷新日志缓冲区，确保所有日志立即写入磁盘
   */
  void log_flush();

  /**
   * @brief 打印命令行帮助信息到标准输出
   * @param name 程序名称
   */
  void print_help(const char *name);

  /**
   * @brief 周期性统计日志辅助类（最小值/最大值/平均值）
   * 在指定时间间隔内收集数值样本，到期后输出统计结果
   * @tparam T 数值类型（支持整数和浮点数）
   */
  template<typename T>
  class min_max_avg_periodic_logger {
  public:
    /**
     * @brief 构造函数
     * @param severity 日志级别（决定输出到哪个日志器）
     * @param message 统计项描述（如 "帧处理时间"）
     * @param units 单位（如 "ms"）
     * @param interval_in_seconds 统计周期（默认20秒输出一次）
     */
    min_max_avg_periodic_logger(boost::log::sources::severity_logger<int> &severity, std::string_view message, std::string_view units, std::chrono::seconds interval_in_seconds = std::chrono::seconds(20)):
        severity(severity),
        message(message),
        units(units),
        interval(interval_in_seconds),
        enabled(config::sunshine.min_log_level <= severity.default_severity()) { // 仅在日志级别满足时启用
    }

    /**
     * @brief 收集数值样本，到达统计周期时输出日志
     * @param value 当前采样值
     */
    void collect_and_log(const T &value) {
      if (enabled) {
        // 定义统计结果打印回调：输出 "消息 (min/max/avg): 最小值单位/最大值单位/平均值单位"
        auto print_info = [&](const T &min_value, const T &max_value, double avg_value) {
          auto f = stat_trackers::two_digits_after_decimal();
          if constexpr (std::is_floating_point_v<T>) {
            BOOST_LOG(severity.get()) << message << " (min/max/avg): " << f % min_value << units << "/" << f % max_value << units << "/" << f % avg_value << units;
          } else {
            BOOST_LOG(severity.get()) << message << " (min/max/avg): " << min_value << units << "/" << max_value << units << "/" << f % avg_value << units;
          }
        };
        tracker.collect_and_callback_on_interval(value, print_info, interval); // 内部累积并在超时时触发回调
      }
    }

    /**
     * @brief 通过延迟求值函数收集样本（仅在日志启用时才调用函数，避免不必要的计算）
     */
    void collect_and_log(std::function<T()> func) {
      if (enabled) {
        collect_and_log(func());
      }
    }

    /**
     * @brief 重置统计数据
     */
    void reset() {
      if (enabled) {
        tracker.reset();
      }
    }

    /**
     * @brief 查询此日志器是否启用
     */
    bool is_enabled() const {
      return enabled;
    }

  private:
    std::reference_wrapper<boost::log::sources::severity_logger<int>> severity; // 日志级别引用
    std::string message;          // 统计项描述
    std::string units;            // 数值单位
    std::chrono::seconds interval; // 统计输出周期
    bool enabled;                 // 是否启用（取决于日志级别）
    stat_trackers::min_max_avg_tracker<T> tracker; // 底层统计追踪器
  };

  /**
   * @brief 周期性时间差统计日志辅助类
   * 测量两个时间点之间的间隔，定期输出统计结果（最小/最大/平均耗时）
   * 适用于测量帧处理时间、网络延迟等短时间间隔
   */
  class time_delta_periodic_logger {
  public:
    /**
     * @brief 构造函数
     * @param severity 日志级别
     * @param message 统计项描述
     * @param interval_in_seconds 统计周期
     */
    time_delta_periodic_logger(boost::log::sources::severity_logger<int> &severity, std::string_view message, std::chrono::seconds interval_in_seconds = std::chrono::seconds(20)):
        logger(severity, message, "ms", interval_in_seconds) {
    }

    /**
     * @brief 记录第一个时间点
     */
    void first_point(const std::chrono::steady_clock::time_point &point) {
      if (logger.is_enabled()) {
        point1 = point;
      }
    }

    /**
     * @brief 以当前时间作为第一个时间点
     */
    void first_point_now() {
      if (logger.is_enabled()) {
        first_point(std::chrono::steady_clock::now());
      }
    }

    /**
     * @brief 记录第二个时间点并计算时间差，到期时输出统计日志
     */
    void second_point_and_log(const std::chrono::steady_clock::time_point &point) {
      if (logger.is_enabled()) {
        logger.collect_and_log(std::chrono::duration<double, std::milli>(point - point1).count()); // 计算毫秒级时间差
      }
    }

    /**
     * @brief 以当前时间作为第二个时间点并记录
     */
    void second_point_now_and_log() {
      if (logger.is_enabled()) {
        second_point_and_log(std::chrono::steady_clock::now());
      }
    }

    void reset() {
      if (logger.is_enabled()) {
        logger.reset();
      }
    }

    bool is_enabled() const {
      return logger.is_enabled();
    }

  private:
    std::chrono::steady_clock::time_point point1 = std::chrono::steady_clock::now(); // 第一个时间点
    min_max_avg_periodic_logger<double> logger; // 底层统计日志器（单位：毫秒）
  };

  /**
   * @brief 用方括号包裹字符串，如 "abc" -> "[abc]"
   * @param input 输入字符串
   * @return 被方括号包裹的字符串
   */
  std::string bracket(const std::string &input);

  /**
   * @brief 用方括号包裹宽字符串（Unicode版本）
   */
  std::wstring bracket(const std::wstring &input);

}  // namespace logging
