/**
 * @file entry_handler.h
 * @brief 入口处理函数的声明
 * 包含命令行参数处理、程序生命周期管理、服务控制等功能
 */
#pragma once

// 标准库头文件
#include <atomic>      // 原子操作（线程安全的退出码）
#include <string_view> // 字符串视图

// 本地项目头文件
#include "thread_pool.h" // 线程池
#include "thread_safe.h" // 线程安全工具

/**
 * @brief 启动Web管理界面（在默认浏览器中打开）
 * @param path 可选的URL子路径（如 "/pin" 打开PIN配对页面）
 */
void launch_ui(const std::optional<std::string> &path = std::nullopt);

/**
 * @brief 命令行参数处理函数命名空间
 * 处理子命令：creds（凭证）、help（帮助）、version（版本）等
 */
namespace args {
  /**
   * @brief 重置Web管理界面的用户凭证（用户名和密码）
   * @param name 程序名称
   * @param argc 参数数量
   * @param argv 参数数组（[0]=用户名, [1]=密码）
   */
  int creds(const char *name, int argc, char *argv[]);

  /**
   * @brief 打印帮助信息到标准输出
   * @param name 程序名称
   */
  int help(const char *name);

  /**
   * @brief 打印版本信息到标准输出
   */
  int version();

#ifdef _WIN32
  /**
   * @brief 恢复NVIDIA控制面板全局设置
   * 当Sunshine异常终止后，从撤销文件恢复NVIDIA设置
   * 通常由卸载程序调用
   */
  int restore_nvprefs_undo();
#endif
}  // namespace args

/**
 * @brief Sunshine程序生命周期管理命名空间
 * 提供优雅退出、调试中断等功能
 */
namespace lifetime {
  extern char **argv;                      // 命令行参数数组
  extern std::atomic_int desired_exit_code; // 期望的退出码（原子操作，线程安全）

  /**
   * @brief 优雅地终止Sunshine程序
   * @param exit_code 程序退出码
   * @param async true=异步非阻塞退出, false=阻塞直到程序实际退出
   */
  void exit_sunshine(int exit_code, bool async);

  /**
   * @brief 触发调试器断点，若无调试器附加则强制终止程序
   */
  void debug_trap();

  /**
   * @brief 获取传递给main()的命令行参数数组
   */
  char **get_argv();
}  // namespace lifetime

/**
 * @brief 记录发布者元数据（从CMake编译时注入的发布者名称、网站、支持链接等）
 */
void log_publisher_data();

#ifdef _WIN32
/**
 * @brief 检测NVIDIA GeForce Experience中的GameStream是否启用
 * GameStream与Sunshine使用相同端口，启用会导致冲突
 * @return true=GameStream已启用, false=未启用
 */
bool is_gamestream_enabled();

/**
 * @brief Windows服务控制命名空间
 * 提供Sunshine Windows服务的查询、启动、等待就绪等功能
 */
namespace service_ctrl {
  /**
   * @brief 检查Sunshine Windows服务是否正在运行
   */
  bool is_service_running();

  /**
   * @brief 启动Sunshine Windows服务并等待启动完成
   */
  bool start_service();

  /**
   * @brief 等待Web管理界面就绪（通过查询TCP监听端口确认）
   */
  bool wait_for_ui_ready();
}  // namespace service_ctrl
#endif
