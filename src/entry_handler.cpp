/**
 * @file entry_handler.cpp
 * @brief 入口处理函数的实现
 * 包含命令行参数处理、程序生命周期管理、Windows服务控制等
 */
// 标准库头文件
#include <csignal>    // 信号处理
#include <format>     // C++20格式化
#include <iostream>   // 标准输出
#include <thread>     // 线程支持

// 本地项目头文件
#include "config.h"          // 配置管理
#include "confighttp.h"      // HTTP配置服务
#include "entry_handler.h"   // 本文件头文件
#include "globals.h"         // 全局变量
#include "httpcommon.h"      // HTTP公共接口
#include "logging.h"         // 日志系统
#include "network.h"         // 网络工具（端口映射等）
#include "platform/common.h" // 平台公共接口

extern "C" {
#ifdef _WIN32
  #include <iphlpapi.h> // Windows IP辅助API（查询TCP表用于检测服务就绪）
#endif
}

using namespace std::literals;

/**
 * @brief 启动Web管理界面
 * 拼接HTTPS URL并在默认浏览器中打开
 */
void launch_ui(const std::optional<std::string> &path) {
  // 拼接完整的HTTPS管理界面URL
  std::string url = std::format("https://localhost:{}", static_cast<int>(net::map_port(confighttp::PORT_HTTPS)));
  if (path) {
    url += *path; // 追加可选的子路径
  }
  platf::open_url(url); // 调用平台接口打开URL
}

namespace args {
  /**
   * @brief 处理凭证管理命令
   * 参数不足时显示帮助，否则保存新的用户名和密码
   */
  int creds(const char *name, int argc, char *argv[]) {
    if (argc < 2 || argv[0] == "help"sv || argv[1] == "help"sv) {
      help(name); // 参数不足或请求帮助时显示帮助
    }

    http::save_user_creds(config::sunshine.credentials_file, argv[0], argv[1]); // 保存用户凭证

    return 0;
  }

  /**
   * @brief 显示帮助信息
   */
  int help(const char *name) {
    logging::print_help(name);
    return 0;
  }

  /**
   * @brief 显示版本信息（版本已在启动时通过日志输出）
   */
  int version() {
    return 0;
  }

#ifdef _WIN32
  /**
   * @brief Windows专用：恢复NVIDIA控制面板设置
   * 加载NVIDIA设置库，从撤销文件恢复设置，然后卸载库
   */
  int restore_nvprefs_undo() {
    if (nvprefs_instance.load()) {
      nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
      nvprefs_instance.unload();
    }
    return 0;
  }
#endif
}  // namespace args

namespace lifetime {
  char **argv;                        // 程序命令行参数
  std::atomic_int desired_exit_code;   // 程序期望退出码（原子变量，线程安全）

  /**
   * @brief 优雅退出Sunshine
   * 通过触发SIGINT信号启动终止流程
   * async=false时会阻塞当前线程直到程序实际退出
   */    /**
     * @brief 优雅或强制退出Sunshine程序
     */  void exit_sunshine(int exit_code, bool async) {
    // 仅保存第一次调用时的退出码（原子比较交换）
    int zero = 0;
    desired_exit_code.compare_exchange_strong(zero, exit_code);

    // 触发SIGINT信号启动关闭流程
    std::raise(SIGINT);

    // 终止将异步进行，但调用者可能要求同步行为
    while (!async) {
      std::this_thread::sleep_for(1s); // 阻塞等待
    }
  }

  /**
   * @brief 触发调试器断点
   * Windows上使用DebugBreak()API，其他平台使用SIGTRAP信号
   */
  void debug_trap() {
#ifdef _WIN32
    DebugBreak();
#else
    std::raise(SIGTRAP);
#endif
  }

  char **get_argv() {
    return argv;
  }
}  // namespace lifetime

/**
 * @brief 记录发布者元数据到日志
 * 输出发布者名称、网站和支持链接（编译时注入）
 */
void log_publisher_data() {
  BOOST_LOG(info) << "Package Publisher: "sv << SUNSHINE_PUBLISHER_NAME;
  BOOST_LOG(info) << "Publisher Website: "sv << SUNSHINE_PUBLISHER_WEBSITE;
  BOOST_LOG(info) << "Get support: "sv << SUNSHINE_PUBLISHER_ISSUE_URL;
}

#ifdef _WIN32
/**
 * @brief 检查NVIDIA GameStream是否启用
 * 通过读取注册表键值判断 GeForce Experience 中的GameStream是否开启
 */
bool is_gamestream_enabled() {
  DWORD enabled;
  DWORD size = sizeof(enabled);
  return RegGetValueW(
           HKEY_LOCAL_MACHINE,
           L"SOFTWARE\\NVIDIA Corporation\\NvStream",
           L"EnableStreaming",
           RRF_RT_REG_DWORD,
           nullptr,
           &enabled,
           &size
         ) == ERROR_SUCCESS &&
         enabled != 0;
}

namespace service_ctrl {
  class service_controller {
  public:
    /**
     * @brief Constructor for service_controller class.
     * @param service_desired_access SERVICE_* desired access flags.
     */
    service_controller(DWORD service_desired_access) {
      scm_handle = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
      if (!scm_handle) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "OpenSCManager() failed: "sv << winerr;
        return;
      }

      service_handle = OpenServiceA(scm_handle, "SunshineService", service_desired_access);
      if (!service_handle) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "OpenService() failed: "sv << winerr;
        return;
      }
    }

    ~service_controller() {
      if (service_handle) {
        CloseServiceHandle(service_handle);
      }

      if (scm_handle) {
        CloseServiceHandle(scm_handle);
      }
    }

    /**
     * @brief Asynchronously starts the Sunshine service.
     */
    bool start_service() {
      if (!service_handle) {
        return false;
      }

      if (!StartServiceA(service_handle, 0, nullptr)) {
        auto winerr = GetLastError();
        if (winerr != ERROR_SERVICE_ALREADY_RUNNING) {
          BOOST_LOG(error) << "StartService() failed: "sv << winerr;
          return false;
        }
      }

      return true;
    }

    /**
     * @brief Query the service status.
     * @param status The SERVICE_STATUS struct to populate.
     */
    bool query_service_status(SERVICE_STATUS &status) {
      if (!service_handle) {
        return false;
      }

      if (!QueryServiceStatus(service_handle, &status)) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "QueryServiceStatus() failed: "sv << winerr;
        return false;
      }

      return true;
    }

  private:
    SC_HANDLE scm_handle = nullptr;
    SC_HANDLE service_handle = nullptr;
  };

  bool is_service_running() {
    service_controller sc {SERVICE_QUERY_STATUS};

    SERVICE_STATUS status;
    if (!sc.query_service_status(status)) {
      return false;
    }

    return status.dwCurrentState == SERVICE_RUNNING;
  }

  bool start_service() {
    service_controller sc {SERVICE_QUERY_STATUS | SERVICE_START};

    std::cout << "Starting Sunshine..."sv;

    // This operation is asynchronous, so we must wait for it to complete
    if (!sc.start_service()) {
      return false;
    }

    SERVICE_STATUS status;
    do {
      Sleep(1000);
      std::cout << '.';
    } while (sc.query_service_status(status) && status.dwCurrentState == SERVICE_START_PENDING);

    if (status.dwCurrentState != SERVICE_RUNNING) {
      BOOST_LOG(error) << std::format("{} failed to start: {}"sv, platf::SERVICE_NAME, status.dwWin32ExitCode);
      return false;
    }

    std::cout << std::endl;
    return true;
  }

  bool wait_for_ui_ready() {
    std::cout << "Waiting for Web UI to be ready...";

    // Wait up to 30 seconds for the web UI to start
    for (int i = 0; i < 30; i++) {
      PMIB_TCPTABLE tcp_table = nullptr;
      ULONG table_size = 0;
      ULONG err;

      auto fg = util::fail_guard([&tcp_table]() {
        free(tcp_table);
      });

      do {
        // Query all open TCP sockets to look for our web UI port
        err = GetTcpTable(tcp_table, &table_size, false);
        if (err == ERROR_INSUFFICIENT_BUFFER) {
          free(tcp_table);
          tcp_table = (PMIB_TCPTABLE) malloc(table_size);
        }
      } while (err == ERROR_INSUFFICIENT_BUFFER);

      if (err != NO_ERROR) {
        BOOST_LOG(error) << "Failed to query TCP table: "sv << err;
        return false;
      }

      uint16_t port_nbo = htons(net::map_port(confighttp::PORT_HTTPS));
      for (DWORD i = 0; i < tcp_table->dwNumEntries; i++) {
        auto &entry = tcp_table->table[i];

        // Look for our port in the listening state
        if (entry.dwLocalPort == port_nbo && entry.dwState == MIB_TCP_STATE_LISTEN) {
          std::cout << std::endl;
          return true;
        }
      }

      Sleep(1000);
      std::cout << '.';
    }

    std::cout << "timed out"sv << std::endl;
    return false;
  }
}  // namespace service_ctrl
#endif
