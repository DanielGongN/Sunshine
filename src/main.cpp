/**
 * @file src/main.cpp
 * @brief Definitions for the main entry point for Sunshine.
 */
// 标准库头文件包含
#include <codecvt>    // 编码转换支持（UTF-8与宽字符转换）
#include <csignal>    // 信号处理（SIGINT、SIGTERM等）
#include <filesystem> // 文件系统操作
#include <fstream>    // 文件流
#include <iostream>   // 标准输入输出

#ifdef __APPLE__
  #include <mach-o/dyld.h> // macOS获取可执行文件路径
#endif

// 本地项目头文件包含
#include "confighttp.h"     // HTTP配置服务器
#include "display_device.h" // 显示设备管理
#include "entry_handler.h"  // 命令行参数处理入口
#include "globals.h"        // 全局变量和生命周期管理
#include "httpcommon.h"     // HTTP公共接口
#include "logging.h"        // 日志系统
#include "main.h"           // main函数声明
#include "nvhttp.h"         // NVIDIA GameStream HTTP协议处理
#include "process.h"        // 子进程管理
#include "system_tray.h"    // 系统托盘图标管理
#include "upnp.h"           // UPnP端口映射
#include "video.h"          // 视频编码和捕获

extern "C" {
#include "rswrapper.h" // Reed-Solomon纠错编码C语言封装
}

using namespace std::literals; // 启用字面量后缀（如 "xxx"sv）

// 信号处理器映射表：信号编号 -> 回调函数
std::map<int, std::function<void()>> signal_handlers;

/**
 * @brief 信号转发函数，将系统信号转发到注册的处理函数
 * @param sig 接收到的信号编号
 */
void on_signal_forwarder(int sig) {
  signal_handlers.at(sig)(); // 查找并调用对应信号的处理函数
}

/**
 * @brief 注册信号处理函数的模板方法
 * @param sig 要监听的信号编号
 * @param fn 信号触发时要执行的回调函数
 */
template<class FN>
void on_signal(int sig, FN &&fn) {
  signal_handlers.emplace(sig, std::forward<FN>(fn)); // 将回调存入映射表

  std::signal(sig, on_signal_forwarder); // 注册系统信号处理器
}

// 命令行子命令映射表：命令名 -> 处理函数
// 支持的子命令：creds（凭证管理）、help（帮助）、version（版本）、restore-nvprefs-undo（恢复NVIDIA设置）
std::map<std::string_view, std::function<int(const char *name, int argc, char **argv)>> cmd_to_func {
  {"creds"sv, [](const char *name, int argc, char **argv) {
     return args::creds(name, argc, argv); // 处理凭证管理命令
   }},
  {"help"sv, [](const char *name, int argc, char **argv) {
     return args::help(name); // 显示帮助信息
   }},
  {"version"sv, [](const char *name, int argc, char **argv) {
     return args::version(); // 显示版本信息
   }},
#ifdef _WIN32
  {"restore-nvprefs-undo"sv, [](const char *name, int argc, char **argv) {
     return args::restore_nvprefs_undo(); // Windows专用：恢复NVIDIA控制面板设置
   }},
#endif
};

#ifdef _WIN32
/**
 * @brief Windows会话监视窗口消息处理函数
 * 用于接收系统关机/注销等会话结束消息（WM_ENDSESSION）
 */
LRESULT CALLBACK SessionMonitorWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_CLOSE:
      DestroyWindow(hwnd); // 销毁窗口
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0); // 退出消息循环
      return 0;
    case WM_ENDSESSION:
      {
        // 系统关机/注销时，阻塞地终止Sunshine进程
        std::cout << "Received WM_ENDSESSION"sv << std::endl;
        lifetime::exit_sunshine(0, false); // 以阻塞方式退出
        return 0;
      }
    default:
      return DefWindowProc(hwnd, uMsg, wParam, lParam); // 默认消息处理
  }
}

/**
 * @brief Windows控制台关闭事件处理函数
 * 当用户关闭控制台窗口时触发
 */
WINAPI BOOL ConsoleCtrlHandler(DWORD type) {
  if (type == CTRL_CLOSE_EVENT) {
    BOOST_LOG(info) << "Console closed handler called";
    lifetime::exit_sunshine(0, false); // 优雅退出
  }
  return FALSE;
}
#endif

// 编译时常量：系统托盘功能是否启用
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
constexpr bool tray_is_enabled = true;
#else
constexpr bool tray_is_enabled = false;
#endif

/**
 * @brief 主线程事件循环
 * 根据平台决定是否运行系统托盘事件循环，或仅等待关闭信号
 * @param shutdown_event 关闭事件，用于通知主线程退出
 */
/**
 * @brief 主线程事件循环：处理系统托盘事件或等待关闭信号
 */
void mainThreadLoop(const std::shared_ptr<safe::event_t<bool>> &shutdown_event) {
  bool run_loop = false;

  // 判断是否需要主线程事件循环
#ifndef _WIN32
  run_loop = tray_is_enabled && config::sunshine.system_tray;  // Windows上托盘运行在单独线程，不需要主线程循环
#endif

  if (!run_loop) {
    BOOST_LOG(info) << "No main thread features enabled, skipping event loop"sv;
    // 没有需要主线程处理的功能，等待关闭信号
    shutdown_event->view();
    return;
  }

  // 启动主线程事件循环（处理系统托盘事件）
  BOOST_LOG(info) << "Starting main loop"sv;
  while (system_tray::process_tray_events() == 0); // 持续处理托盘事件直到退出
  BOOST_LOG(info) << "Main loop has exited"sv;
}

/**
 * @brief Sunshine主入口函数
 * 完成初始化流程：设置工作目录 -> 安全设置 -> 编码设置 -> 解析配置 -> 初始化日志 ->
 * 处理子命令 -> 初始化显示设备 -> NVIDIA设置 -> 创建会话监视 ->
 * 注册信号处理 -> 初始化各子系统 -> 启动HTTP/RTSP服务 -> 进入主循环
 */
/**
 * @brief Sunshine程序主入口：解析参数→初始化模块→启动服务→主循环
 */
int main(int argc, char *argv[]) {
#ifdef __APPLE__
  // macOS处理：Bundle中的资源文件相对于可执行文件路径引用（如 ../Resources/assets）
  // 因此需要将当前工作目录设置为 Contents/MacOS
  {
    char executable[2048];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) == 0) {
      std::error_code ec;
      // 获取可执行文件的规范化父目录路径
      auto exec_dir = std::filesystem::weakly_canonical(std::filesystem::path {executable}, ec).parent_path();
      if (!ec) {
        std::filesystem::current_path(exec_dir, ec); // 切换工作目录
      }
      if (ec) {
        std::cerr << "Failed to set working directory to executable path: " << ec.message() << '\n';
      }
    }
  }
#endif

  lifetime::argv = argv; // 保存命令行参数到全局变量

  task_pool_util::TaskPool::task_id_t force_shutdown = nullptr; // 强制关闭任务ID

#ifdef _WIN32
  // Windows安全措施：限制DLL搜索路径，只搜索应用目录和System32
  // 防止用户在PATH中放置可写目录导致DLL劫持攻击
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

  setlocale(LC_ALL, "C"); // 设置C语言默认区域，避免本地化影响
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  // 设置全局C++区域为UTF-8编码（boost::log使用此区域）
  std::locale::global(std::locale(std::locale(), new std::codecvt_utf8<wchar_t>));
#pragma GCC diagnostic pop

  // 创建全局邮件系统（线程间安全通信的消息总线）
  mail::man = std::make_shared<safe::mail_raw_t>();

  // 解析配置文件和命令行参数
  if (config::parse(argc, argv)) {
    return 0; // 配置解析处理完成（可能是打印帮助信息等），直接退出
  }

  // 初始化日志系统，设置最低日志级别和日志文件路径
  auto log_deinit_guard = logging::init(config::sunshine.min_log_level, config::sunshine.log_file);
  if (!log_deinit_guard) {
    BOOST_LOG(error) << "Logging failed to initialize"sv;
  }

  // 从此处开始可以使用日志系统
  // 此前的日志只会输出到stdout，不会出现在UI的日志查看器中
  // 首先打印版本信息
  BOOST_LOG(info) << PROJECT_NAME << " version: " << PROJECT_VERSION << " commit: " << PROJECT_VERSION_COMMIT;

  // 记录发布者元数据
  log_publisher_data();

  // 打印所有被修改的配置项
  for (auto &[name, val] : config::modified_config_settings) {
    BOOST_LOG(info) << "config: '"sv << name << "' = "sv << val;
  }
  config::modified_config_settings.clear(); // 清空已打印的配置项

  // 如果用户指定了子命令（如 creds、help、version），执行对应的处理函数
  if (!config::sunshine.cmd.name.empty()) {
    auto fn = cmd_to_func.find(config::sunshine.cmd.name);
    if (fn == std::end(cmd_to_func)) {
      // 未知子命令，打印可用命令列表
      BOOST_LOG(fatal) << "Unknown command: "sv << config::sunshine.cmd.name;

      BOOST_LOG(info) << "Possible commands:"sv;
      for (auto &[key, _] : cmd_to_func) {
        BOOST_LOG(info) << '\t' << key;
      }

      return 7;
    }

    return fn->second(argv[0], config::sunshine.cmd.argc, config::sunshine.cmd.argv);
  }

  // 初始化显示设备管理守卫（优先执行，因为它还负责崩溃后的恢复）
  // 确保用户不会因显示设备状态异常而丢失显示输出
  // 此守卫应在强制关闭之前销毁，以加速清理过程
  auto display_device_deinit_guard = display_device::init(platf::appdata() / "display_device.state", config::video);
  if (!display_device_deinit_guard) {
    BOOST_LOG(error) << "Display device session failed to initialize"sv;
  }

#ifdef _WIN32
  // 如果系统有NVIDIA GPU，修改NVIDIA控制面板的相关设置以优化串流性能
  if (nvprefs_instance.load()) {
    // 从上次异常终止留下的撤销文件中恢复全局设置
    nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
    // 为sunshine.exe修改应用程序配置文件（针对本程序的GPU设置）
    nvprefs_instance.modify_application_profile();
    // 修改全局GPU设置，同时生成撤销文件以便异常终止后恢复
    nvprefs_instance.modify_global_profile();
    // 卸载动态库，使Sunshine能够在驱动重新安装后继续运行
    nvprefs_instance.unload();
  }

  // 设置进程关闭优先级为最低（0x100），使Windows在关机/注销时尽可能晚地终止Sunshine
  SetProcessShutdownParameters(0x100, SHUTDOWN_NORETRY);

  // 创建隐藏窗口来接收系统关机通知（因为加载了gdi32.dll所以需要用窗口消息机制）
  std::promise<HWND> session_monitor_hwnd_promise;
  auto session_monitor_hwnd_future = session_monitor_hwnd_promise.get_future();
  std::promise<void> session_monitor_join_thread_promise;
  auto session_monitor_join_thread_future = session_monitor_join_thread_promise.get_future();

  // 在独立线程中创建会话监视窗口并运行其消息循环
  std::thread session_monitor_thread([&]() {
    platf::set_thread_name("session_monitor"); // 设置线程名便于调试
    session_monitor_join_thread_promise.set_value_at_thread_exit(); // 线程退出时通知

    // 注册窗口类
    WNDCLASSA wnd_class {};
    wnd_class.lpszClassName = "SunshineSessionMonitorClass";
    wnd_class.lpfnWndProc = SessionMonitorWindowProc; // 设置消息处理函数
    if (!RegisterClassA(&wnd_class)) {
      session_monitor_hwnd_promise.set_value(nullptr);
      BOOST_LOG(error) << "Failed to register session monitor window class"sv << std::endl;
      return;
    }

    // 创建隐藏窗口
    auto wnd = CreateWindowExA(
      0,
      wnd_class.lpszClassName,
      "Sunshine Session Monitor Window",
      0,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      nullptr,
      nullptr,
      nullptr,
      nullptr
    );

    session_monitor_hwnd_promise.set_value(wnd); // 将窗口句柄传递给主线程

    if (!wnd) {
      BOOST_LOG(error) << "Failed to create session monitor window"sv << std::endl;
      return;
    }

    ShowWindow(wnd, SW_HIDE); // 隐藏窗口

    // 运行Windows消息循环，处理WM_ENDSESSION等系统消息
    MSG msg {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  });

  // 创建RAII守卫：在作用域退出时清理会话监视线程
  auto session_monitor_join_thread_guard = util::fail_guard([&]() {
    if (session_monitor_hwnd_future.wait_for(1s) == std::future_status::ready) {
      if (HWND session_monitor_hwnd = session_monitor_hwnd_future.get()) {
        PostMessage(session_monitor_hwnd, WM_CLOSE, 0, 0); // 发送关闭消息
      }

      if (session_monitor_join_thread_future.wait_for(1s) == std::future_status::ready) {
        session_monitor_thread.join(); // 等待线程正常退出
        return;
      } else {
        BOOST_LOG(warning) << "session_monitor_join_thread_future reached timeout";
      }
    } else {
      BOOST_LOG(warning) << "session_monitor_hwnd_future reached timeout";
    }

    session_monitor_thread.detach(); // 超时则分离线程
  });

#endif

  task_pool.start(1); // 启动任务池（1个工作线程），用于延迟任务调度

  // 在日志系统初始化后创建信号处理器
  // 注册SIGINT（Ctrl+C中断）信号处理
  auto shutdown_event = mail::man->event<bool>(mail::shutdown); // 获取关闭事件
  on_signal(SIGINT, [&force_shutdown, &display_device_deinit_guard, shutdown_event]() {
    BOOST_LOG(info) << "Interrupt handler called"sv;

    // 设置10秒后强制关闭的定时任务（防止优雅关闭卡死）
    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
      logging::log_flush();
      lifetime::debug_trap(); // 触发调试断点/强制终止
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    // 触发关闭事件，跳出主循环
    shutdown_event->raise(true);
    system_tray::end_tray(); // 关闭系统托盘

    display_device_deinit_guard = nullptr; // 释放显示设备守卫，恢复显示设置
  });

  // 注册SIGTERM（终止）信号处理，逻辑与SIGINT相同
  on_signal(SIGTERM, [&force_shutdown, &display_device_deinit_guard, shutdown_event]() {
    BOOST_LOG(info) << "Terminate handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
      logging::log_flush();
      lifetime::debug_trap();
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    shutdown_event->raise(true);
    system_tray::end_tray();

    display_device_deinit_guard = nullptr;
  });

#ifdef _WIN32
  // Windows：注册控制台关闭事件处理，使关闭控制台窗口时能优雅退出
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif

  // 刷新应用程序列表（从配置文件加载可启动的游戏/应用）
  proc::refresh(config::stream.file_apps);

  // 以下子系统初始化如果失败，仅记录错误并继续运行
  // 这样用户仍可通过Web UI访问界面来修复配置问题或查看日志

  // 初始化平台相关功能（输入、显示等操作系统级别的初始化）
  auto platf_deinit_guard = platf::init();
  if (!platf_deinit_guard) {
    BOOST_LOG(error) << "Platform failed to initialize"sv;
  }

  // 初始化进程管理子系统
  auto proc_deinit_guard = proc::init();
  if (!proc_deinit_guard) {
    BOOST_LOG(error) << "Proc failed to initialize"sv;
  }

  reed_solomon_init(); // 初始化Reed-Solomon纠错编码器（用于网络传输纠错）
  auto input_deinit_guard = input::init(); // 初始化输入子系统（键盘、鼠标等）

  // 探测游戏手柄输入是否可用
  if (input::probe_gamepads()) {
    BOOST_LOG(warning) << "No gamepad input is available"sv;
  }

  // 探测可用的视频编码器（H.264/HEVC/AV1硬件编码器）
  if (video::probe_encoders()) {
    BOOST_LOG(error) << "Video failed to find working encoder"sv;
  }

  // 初始化HTTP服务器（Web管理界面）
  if (http::init()) {
    BOOST_LOG(fatal) << "HTTP interface failed to initialize"sv;

#ifdef _WIN32
    BOOST_LOG(fatal) << "To relaunch Sunshine successfully, use the shortcut in the Start Menu. Do not run Sunshine.exe manually."sv;
    std::this_thread::sleep_for(10s); // Windows上等待10秒让用户看到错误信息
#endif

    return -1; // HTTP初始化失败是致命错误，必须退出
  }

  // 异步启动mDNS服务发布（让Moonlight客户端能自动发现Sunshine）
  std::unique_ptr<platf::deinit_t> mDNS;
  auto sync_mDNS = std::async(std::launch::async, [&mDNS]() {
    mDNS = platf::publish::start();
  });

  // 异步启动UPnP端口映射（自动在路由器上开放所需端口）
  std::unique_ptr<platf::deinit_t> upnp_unmap;
  auto sync_upnp = std::async(std::launch::async, [&upnp_unmap]() {
    upnp_unmap = upnp::start();
  });

  // 临时修复：Simple-Web-Server库需要更新或替换
  // 检查是否在启动过程中已收到关闭信号
  if (shutdown_event->peek()) {
    return lifetime::desired_exit_code;
  }

  // 启动三个核心服务线程
  std::thread httpThread {nvhttp::start};      // NVIDIA HTTP配对/控制协议服务
  std::thread configThread {confighttp::start}; // Web配置界面HTTP服务
  std::thread rtspThread {rtsp_stream::start};  // RTSP串流协议服务

#ifdef _WIN32
  // 检测GameStream冲突：如果使用默认端口且GeForce Experience的GameStream仍然启用
  if (config::sunshine.port == 47989 && is_gamestream_enabled()) {
    BOOST_LOG(fatal) << "GameStream is still enabled in GeForce Experience! This *will* cause streaming problems with Sunshine!"sv;
    BOOST_LOG(fatal) << "Disable GameStream on the SHIELD tab in GeForce Experience or change the Port setting on the Advanced tab in the Sunshine Web UI."sv;
  }
#endif

  // 初始化系统托盘图标
  if (tray_is_enabled && config::sunshine.system_tray) {
    BOOST_LOG(info) << "Starting system tray"sv;
#ifdef _WIN32
    // Windows特殊处理：作为服务运行时，首次启动托盘图标可能不显示（已知bug）
    // 因此Windows上将托盘放在独立线程中运行
    system_tray::init_tray_threaded();
#else
    // 其他平台在主线程初始化托盘
    system_tray::init_tray();
#endif
  }

  // 进入主线程事件循环（阻塞直到收到退出信号）
  mainThreadLoop(shutdown_event);

  // 等待所有服务线程结束
  httpThread.join();
  configThread.join();
  rtspThread.join();

  // 停止并等待任务池中所有任务完成
  task_pool.stop();
  task_pool.join();

#ifdef _WIN32
  // 程序退出前恢复NVIDIA控制面板全局设置
  if (nvprefs_instance.owning_undo_file() && nvprefs_instance.load()) {
    nvprefs_instance.restore_global_profile(); // 从撤销文件恢复
    nvprefs_instance.unload();
  }
#endif

  return lifetime::desired_exit_code; // 返回期望的退出码
}
