/**
 * @file src/stream.h
 * @brief 串流协议的声明
 * 管理视频/音频/控制数据的实时传输，基于ENet可靠UDP协议
 */
#pragma once

// 标准库头文件
#include <utility>

// 第三方库头文件
#include <boost/asio.hpp> // 异步I/O

// 本地项目头文件
#include "audio.h"   // 音频配置
#include "crypto.h"  // 加密工具
#include "video.h"   // 视频配置

namespace stream {
  // 串流端口偏移量（相对于基础端口）
  constexpr auto VIDEO_STREAM_PORT = 9;   // 视频流端口
  constexpr auto CONTROL_PORT = 10;        // 控制通道端口
  constexpr auto AUDIO_STREAM_PORT = 11;   // 音频流端口

  struct session_t; // 串流会话前向声明

  /**
   * @brief 串流配置结构体（包含音视频配置、数据包大小、FEC等）
   */
  struct config_t {
    audio::config_t audio;    // 音频配置
    video::config_t monitor;  // 视频配置

    int packetsize;             // 数据包大小
    int minRequiredFecPackets;  // 最少所需FEC（前向纠错）数据包数
    int mlFeatureFlags;         // Moonlight特性标志
    int controlProtocolType;    // 控制协议类型
    int audioQosType;           // 音频QoS类型
    int videoQosType;           // 视频QoS类型

    uint32_t encryptionFlagsEnabled; // 加密标志

    std::optional<int> gcmap; // 游戏手柄映射
  };

  /**
   * @brief 串流会话管理命名空间
   */
  namespace session {
    /**
     * @brief 会话状态枚举
     */
    enum class state_e : int {
      STOPPED,  ///< 会话已停止
      STOPPING,  ///< 会话正在停止
      STARTING,  ///< 会话正在启动
      RUNNING,  ///< 会话运行中
    };

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session); // 分配会话
    int start(session_t &session, const std::string &addr_string); // 启动会话
    void stop(session_t &session);  // 停止会话
    void join(session_t &session);  // 等待会话线程结束
    state_e state(session_t &session); // 查询会话状态
  }  // namespace session
}  // namespace stream
