/**
 * @file src/logging.cpp
 * @brief 日志系统的实现
 * 基于Boost.Log异步日志框架，支持文件输出、控制台输出、Android logcat输出
 */
// 标准库头文件
#include <fstream>    // 文件流（日志写入文件）
#include <iomanip>    // 格式化输出（时间戳格式化）
#include <iostream>   // 标准输出

// 第三方库头文件
#include <boost/core/null_deleter.hpp>         // 空删除器（shared_ptr管理cout时不释放）
#include <boost/format.hpp>                     // 字符串格式化
#include <boost/log/attributes/clock.hpp>       // 时钟属性
#include <boost/log/common.hpp>                 // Boost.Log核心
#include <boost/log/expressions.hpp>            // 日志过滤表达式
#include <boost/log/sinks.hpp>                  // 日志接收器
#include <boost/log/sources/severity_logger.hpp> // 严重级别日志器
#include <boost/log/utility/exception_handler.hpp> // 异常处理器

// 本地头文件
#include "logging.h"

// 条件编译头文件
#ifdef __ANDROID__
  #include <android/log.h>  // Android原生日志
#else
  #include <display_device/logging.h> // 显示设备库日志
#endif

extern "C" {
#include <libavutil/log.h> // FFmpeg日志接口
}

using namespace std::literals;

namespace bl = boost::log; // Boost.Log命名空间别名

// 全局异步日志接收器（输出到文件和/或控制台）
boost::shared_ptr<boost::log::sinks::asynchronous_sink<boost::log::sinks::text_ostream_backend>> sink;

// 各日志级别的全局日志器实例
bl::sources::severity_logger<int> verbose(0);  // 级别0：详尽输出（大量调试信息）
bl::sources::severity_logger<int> debug(1);    // 级别1：调试跟踪（流程追踪信息）
bl::sources::severity_logger<int> info(2);     // 级别2：一般通知信息
bl::sources::severity_logger<int> warning(3);  // 级别3：异常事件警告
bl::sources::severity_logger<int> error(4);    // 级别4：可恢复错误
bl::sources::severity_logger<int> fatal(5);    // 级别5：不可恢复的致命错误
#ifdef SUNSHINE_TESTS
bl::sources::severity_logger<int> tests(10);   // 级别10：自动化测试专用输出
#endif

// 定义Boost.Log属性关键字，用于从日志记录中提取严重级别
BOOST_LOG_ATTRIBUTE_KEYWORD(severity, "Severity", int)

namespace logging {
  /**
   * @brief RAII析构函数：自动调用deinit()反初始化日志系统
   */
  deinit_t::~deinit_t() {
    deinit();
  }

  /**
   * @brief 反初始化日志系统
   * 刷新所有缓冲的日志记录，然后从Boost.Log核心移除日志接收器
   */
  void deinit() {
    log_flush();
    bl::core::get()->remove_sink(sink);
    sink.reset();
  }

  /**
   * @brief 日志记录格式化函数
   * 格式：[YYYY-MM-DD HH:MM:SS.mmm]: 级别: 消息内容
   */
  void formatter(const boost::log::record_view &view, boost::log::formatting_ostream &os) {
    constexpr const char *message = "Message";
    constexpr const char *severity = "Severity";

    // 提取日志级别
    auto log_level = view.attribute_values()[severity].extract<int>().get();

    // 根据级别数字映射到可读文本
    std::string_view log_type;
    switch (log_level) {
      case 0:
        log_type = "Verbose: "sv;
        break;
      case 1:
        log_type = "Debug: "sv;
        break;
      case 2:
        log_type = "Info: "sv;
        break;
      case 3:
        log_type = "Warning: "sv;
        break;
      case 4:
        log_type = "Error: "sv;
        break;
      case 5:
        log_type = "Fatal: "sv;
        break;
#ifdef SUNSHINE_TESTS
      case 10:
        log_type = "Tests: "sv;
        break;
#endif
    };

    // 获取当前时间并格式化为 "YYYY-MM-DD HH:MM:SS.毫秒"
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - std::chrono::time_point_cast<std::chrono::seconds>(now)
    );

    auto t = std::chrono::system_clock::to_time_t(now);
    auto lt = *std::localtime(&t);

    // 输出格式化的日志行
    os << "["sv << std::put_time(&lt, "%Y-%m-%d %H:%M:%S.") << boost::format("%03u") % ms.count() << "]: "sv
       << log_type << view.attribute_values()[message].extract<std::string>();
  }
#ifdef __ANDROID__
  namespace sinks = boost::log::sinks;
  namespace expr = boost::log::expressions;

  /**
   * @brief 将日志消息输出到Android原生日志系统（logcat）
   * @param message 日志消息内容
   * @param severity Sunshine日志级别，映射到Android日志优先级
   */
  void android_log(const std::string &message, int severity) {
    android_LogPriority android_priority;
    switch (severity) {
      case 0:
        android_priority = ANDROID_LOG_VERBOSE;
        break;
      case 1:
        android_priority = ANDROID_LOG_DEBUG;
        break;
      case 2:
        android_priority = ANDROID_LOG_INFO;
        break;
      case 3:
        android_priority = ANDROID_LOG_WARN;
        break;
      case 4:
        android_priority = ANDROID_LOG_ERROR;
        break;
      case 5:
        android_priority = ANDROID_LOG_FATAL;
        break;
      default:
        android_priority = ANDROID_LOG_UNKNOWN;
        break;
    }
    __android_log_print(android_priority, "Sunshine", "%s", message.c_str()); // 输出到logcat
  }

  /**
   * @brief Android平台自定义日志接收器后端
   * 将Boost.Log日志记录转发到Android原生日志系统
   */
  struct android_sink_backend: public sinks::basic_sink_backend<sinks::concurrent_feeding> {
    void consume(const bl::record_view &rec) {
      int log_sev = rec[severity].get();            // 提取日志级别
      const std::string log_msg = rec[expr::smessage].get(); // 提取日志消息
      android_log(log_msg, log_sev);                 // 转发到Android logcat
    }
  };
#endif

  /**
   * @brief 初始化日志系统
   * 创建异步日志接收器，配置文件输出和控制台输出，设置过滤器和格式化器
   */
  [[nodiscard]] std::unique_ptr<deinit_t> init(int min_log_level, const std::string &log_file) {
    if (sink) {
      // 如果已经初始化过，先反初始化（通常只在测试中出现）
      deinit();
    }

#ifndef __ANDROID__
    setup_av_logging(min_log_level);               // 配置FFmpeg日志重定向
    setup_libdisplaydevice_logging(min_log_level);  // 配置显示设备库日志重定向
#endif

    // 创建异步文本输出日志接收器
    sink = boost::make_shared<text_sink>();

#ifndef SUNSHINE_TESTS
    // 添加标准输出流作为日志目标（使用null_deleter避免shared_ptr释放cout）
    boost::shared_ptr<std::ostream> stream {&std::cout, boost::null_deleter()};
    sink->locked_backend()->add_stream(stream);
#endif

    // 添加日志文件作为输出目标
    sink->locked_backend()->add_stream(boost::make_shared<std::ofstream>(log_file));
    sink->set_filter(severity >= min_log_level);  // 设置最低日志级别过滤器
    sink->set_formatter(&formatter);               // 设置日志格式化函数

    // 防止异步接收器的后台线程因异常而终止
    // 没有这个设置，单个I/O错误（磁盘满、文件锁定等）会杀死线程，后续日志将丢失
    sink->set_exception_handler(bl::make_exception_suppressor());

    // 每条日志记录后立即刷新，确保磁盘上的日志文件内容不过时
    // 这对Windows服务运行模式尤其重要
    sink->locked_backend()->auto_flush(true);

    // 将配置好的日志接收器注册到Boost.Log核心
    bl::core::get()->add_sink(sink);

#ifdef __ANDROID__
    // Android平台额外添加一个同步logcat日志接收器
    auto android_sink = boost::make_shared<sinks::synchronous_sink<android_sink_backend>>();
    bl::core::get()->add_sink(android_sink);
#endif
    return std::make_unique<deinit_t>(); // 返回RAII守卫
  }

#ifndef __ANDROID__
  /**
   * @brief 配置FFmpeg（libav）日志系统
   * 根据Sunshine的日志级别设置FFmpeg的日志级别，并将FFmpeg日志重定向到Boost.Log
   */
  void setup_av_logging(int min_log_level) {
    if (min_log_level >= 1) {
      av_log_set_level(AV_LOG_QUIET); // 日志级别>=1时静默FFmpeg输出
    } else {
      av_log_set_level(AV_LOG_DEBUG); // verbose模式下开启FFmpeg调试输出
    }
    // 设置FFmpeg日志回调，将其映射到Sunshine的日志级别
    av_log_set_callback([](void *ptr, int level, const char *fmt, va_list vl) {
      static int print_prefix = 1;
      char buffer[1024];

      av_log_format_line(ptr, level, fmt, vl, buffer, sizeof(buffer), &print_prefix); // 格式化FFmpeg日志行
      if (level <= AV_LOG_ERROR) {
        // FFmpeg的AV_LOG_FATAL在某些情况下是预期的（如缺少编解码器支持），所以降级到error
        BOOST_LOG(error) << buffer;
      } else if (level <= AV_LOG_WARNING) {
        BOOST_LOG(warning) << buffer;
      } else if (level <= AV_LOG_INFO) {
        BOOST_LOG(info) << buffer;
      } else if (level <= AV_LOG_VERBOSE) {
        // AV_LOG_VERBOSE比AV_LOG_DEBUG更简洁
        BOOST_LOG(debug) << buffer;
      } else {
        BOOST_LOG(verbose) << buffer;
      }
    });
  }

  /**
   * @brief 配置libdisplaydevice库的日志输出
   * 将显示设备库的日志回调重定向到Sunshine的Boost.Log系统
   */
  void setup_libdisplaydevice_logging(int min_log_level) {
    // 将Sunshine日志级别映射到libdisplaydevice的日志级别范围
    constexpr int min_level {static_cast<int>(display_device::Logger::LogLevel::verbose)};
    constexpr int max_level {static_cast<int>(display_device::Logger::LogLevel::fatal)};
    const auto log_level {static_cast<display_device::Logger::LogLevel>(std::min(std::max(min_level, min_log_level), max_level))};

    display_device::Logger::get().setLogLevel(log_level); // 设置库的日志级别
    // 注册自定义回调，将库的日志转发到Sunshine的Boost.Log
    display_device::Logger::get().setCustomCallback([](const display_device::Logger::LogLevel level, const std::string &message) {
      switch (level) {
        case display_device::Logger::LogLevel::verbose:
          BOOST_LOG(verbose) << message;
          break;
        case display_device::Logger::LogLevel::debug:
          BOOST_LOG(debug) << message;
          break;
        case display_device::Logger::LogLevel::info:
          BOOST_LOG(info) << message;
          break;
        case display_device::Logger::LogLevel::warning:
          BOOST_LOG(warning) << message;
          break;
        case display_device::Logger::LogLevel::error:
          BOOST_LOG(error) << message;
          break;
        case display_device::Logger::LogLevel::fatal:
          BOOST_LOG(fatal) << message;
          break;
      }
    });
  }
#endif

  /**
   * @brief 刷新日志缓冲区，确保所有挂起的日志记录都写入磁盘
   */
  void log_flush() {
    if (sink) {
      sink->flush();
    }
  }

  /**
   * @brief 打印命令行帮助信息
   * 包括用法说明、可用选项和标志位
   */
  void print_help(const char *name) {
    std::cout
      << "Usage: "sv << name << " [options] [/path/to/configuration_file] [--cmd]"sv << std::endl
      << "    Any configurable option can be overwritten with: \"name=value\""sv << std::endl
      << std::endl
      << "    Note: The configuration will be created if it doesn't exist."sv << std::endl
      << std::endl
      << "    --help                    | print help"sv << std::endl
      << "    --creds username password | set user credentials for the Web manager"sv << std::endl
      << "    --version                 | print the version of sunshine"sv << std::endl
      << std::endl
      << "    flags"sv << std::endl
      << "        -0 | Read PIN from stdin"sv << std::endl
      << "        -1 | Do not load previously saved state and do retain any state after shutdown"sv << std::endl
      << "           | Effectively starting as if for the first time without overwriting any pairings with your devices"sv << std::endl
      << "        -2 | Force replacement of headers in video stream"sv << std::endl
      << "        -p | Enable/Disable UPnP"sv << std::endl
      << std::endl;
  }

  /**
   * @brief 用方括号包裹字符串
   */
  std::string bracket(const std::string &input) {
    return "["s + input + "]"s;
  }

  /**
   * @brief 用方括号包裹宽字符串
   */
  std::wstring bracket(const std::wstring &input) {
    return L"["s + input + L"]"s;
  }

}  // namespace logging
