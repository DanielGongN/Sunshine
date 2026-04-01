/**
 * @file globals.h
 * @brief 全局变量和函数的声明
 * 包含跨模块共享的全局对象，如任务池、邮件系统、NVIDIA设置等
 */
#pragma once

// 本地项目头文件
#include "entry_handler.h" // 命令行参数处理
#include "thread_pool.h"   // 线程池定义

/**
 * @brief 全局线程池，用于处理延迟任务和异步任务调度
 */
extern thread_pool_util::ThreadPool task_pool;

/**
 * @brief 是否显示鼠标光标的全局标志（串流时控制远程桌面光标显示）
 */
extern bool display_cursor;

#ifdef _WIN32
  // 声明用于修改NVIDIA控制面板设置的全局单例
  #include "platform/windows/nvprefs/nvprefs_interface.h"

/**
 * @brief NVIDIA控制面板设置管理的全局单例对象
 * 用于在串流期间优化GPU设置，退出时恢复原始设置
 */
extern nvprefs::nvprefs_interface nvprefs_instance;
#endif

/**
 * @brief 进程级通信命名空间（邮件系统）
 * 提供线程安全的消息传递机制，各子系统通过邮件名称订阅和发送消息
 */
namespace mail {
#define MAIL(x) \
  constexpr auto x = std::string_view { \
    #x \
  }

  /**
   * @brief 全局邮件管理器，所有邮件通道的根对象
   */
  extern safe::mail_t man;

  // 全局邮件通道（跨模块使用）
  MAIL(shutdown);              // 关闭信号
  MAIL(broadcast_shutdown);    // 广播关闭信号（通知所有串流会话）
  MAIL(video_packets);         // 视频数据包通道
  MAIL(audio_packets);         // 音频数据包通道
  MAIL(switch_display);        // 切换显示器信号

  // 局部邮件通道（特定模块间使用）
  MAIL(touch_port);            // 触摸端口信息
  MAIL(idr);                   // IDR帧请求（关键帧请求）
  MAIL(invalidate_ref_frames); // 参考帧失效通知
  MAIL(gamepad_feedback);      // 游戏手柄反馈（震动等）
  MAIL(hdr);                   // HDR模式切换
#undef MAIL

}  // namespace mail
