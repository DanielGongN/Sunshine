/**
 * @file src/process.h
 * @brief 串流会话启动的应用程序进程管理的声明
 * 负责启动/停止游戏或应用程序，管理预备命令、进程生命周期等
 */
#pragma once

#ifndef __kernel_entry
  #define __kernel_entry
#endif

// 标准库头文件
#include <optional>
#include <unordered_map>

// 第三方库头文件
#include <boost/process/v1.hpp> // 进程管理库

// 本地项目头文件
#include "config.h"          // 配置管理
#include "platform/common.h" // 平台公共接口
#include "rtsp.h"            // RTSP会话信息
#include "utility.h"          // 工具函数

#define DEFAULT_APP_IMAGE_PATH SUNSHINE_ASSETS_DIR "/box.png" // 默认应用图标路径

namespace proc {
  using file_t = util::safe_ptr_v2<FILE, int, fclose>; // FILE指针智能包装

  typedef config::prep_cmd_t cmd_t; // 预备命令类型别名

  /**
   * @brief 应用程序上下文结构体
   * prep_cmds  -- 预备命令（启动前执行，任一失败则停止）
   * detached   -- 分离命令（后台进程，Sunshine不管理其生命周期）
   * cmd        -- 主程序命令（持续运行直到会话结束或进程退出）
   * working_dir-- 工作目录（某些游戏需要正确的工作目录才能运行）
   * cmd_output -- 输出重定向（空=追加到Sunshine输出, "null"=丢弃, 其他=写入指定文件）
   */
  struct ctx_t {
    std::vector<cmd_t> prep_cmds; // 预备命令列表

    /**
     * 分离命令列表
     * 某些应用（如Steam）会快速退出或无限期运行
     * 用户可以用分离命令来启动后台进程，Sunshine不会管理其生命周期
     */
    std::vector<std::string> detached;

    std::string name;          // 应用名称
    std::string cmd;           // 主程序启动命令
    std::string working_dir;   // 工作目录
    std::string output;        // 输出重定向目标
    std::string image_path;    // 应用图标路径
    std::string id;            // 应用唯一标识
    bool elevated;             // 是否以管理员权限运行
    bool auto_detach;          // 自动分离（进程快速退出时不视为失败）
    bool wait_all;             // 等待所有子进程结束
    std::chrono::seconds exit_timeout; // 退出超时时间
  };

  class proc_t {
  public:
    KITTY_DEFAULT_CONSTR_MOVE_THROW(proc_t)

    proc_t(
      boost::process::v1::environment &&env,
      std::vector<ctx_t> &&apps
    ):
        _app_id(0),
        _env(std::move(env)),
        _apps(std::move(apps)) {
    }

    int execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);

    /**
     * @return `_app_id` if a process is running, otherwise returns `0`
     */
    int running();

    ~proc_t();

    const std::vector<ctx_t> &get_apps() const;
    std::vector<ctx_t> &get_apps();
    std::string get_app_image(int app_id);
    std::string get_last_run_app_name();
    void terminate();

  private:
    int _app_id;

    boost::process::v1::environment _env;
    std::vector<ctx_t> _apps;
    ctx_t _app;
    std::chrono::steady_clock::time_point _app_launch_time;

    // If no command associated with _app_id, yet it's still running
    bool placebo {};

    boost::process::v1::child _process;
    boost::process::v1::group _process_group;

    file_t _pipe;
    std::vector<cmd_t>::const_iterator _app_prep_it;
    std::vector<cmd_t>::const_iterator _app_prep_begin;
  };

  /**
   * @brief Calculate a stable id based on name and image data
   * @return Tuple of id calculated without index (for use if no collision) and one with.
   */
  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index);

  bool check_valid_png(const std::filesystem::path &path);
  std::string validate_app_image_path(std::string app_image_path);
  void refresh(const std::string &file_name);
  std::optional<proc::proc_t> parse(const std::string &file_name);

  /**
   * @brief Initialize proc functions
   * @return Unique pointer to `deinit_t` to manage cleanup
   */
  std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Terminates all child processes in a process group.
   * @param proc The child process itself.
   * @param group The group of all children in the process tree.
   * @param exit_timeout The timeout to wait for the process group to gracefully exit.
   */
  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout);

  extern proc_t proc;
}  // namespace proc
