/**
 * @file src/rtsp.h
 * @brief RTSP串流协议的声明
 * RTSP用于建立和控制串流会话（会话协商、参数交换）
 */
#pragma once

// 标准库头文件
#include <atomic>

// 本地项目头文件
#include "crypto.h"      // 加密工具
#include "thread_safe.h" // 线程安全工具

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21; // RTSP协商端口偏移量

  /**
   * @brief 串流会话启动参数结构体
   * 包含从客户端协商获得的所有串流参数
   */
  struct launch_session_t {
    uint32_t id;                      // 会话ID

    crypto::aes_t gcm_key;            // AES-GCM加密密钥
    crypto::aes_t iv;                 // 初始化向量

    std::string av_ping_payload;      // 音视频ping负载
    uint32_t control_connect_data;    // 控制连接数据

    bool host_audio;                  // 是否在主机播放音频
    std::string unique_id;            // 客户端唯一标识
    int width;                        // 视频宽度
    int height;                       // 视频高度
    int fps;                          // 帧率
    int gcmap;                        // 游戏手柄映射
    int appid;                        // 应用程序ID
    int surround_info;                // 环绕声信息
    std::string surround_params;      // 环绕声参数
    bool continuous_audio;            // 持续音频模式
    bool enable_hdr;                  // 启用HDR
    bool enable_sops;                 // 启用服务器优化游戏设置

    std::optional<crypto::cipher::gcm_t> rtsp_cipher; // RTSP会话加密器
    std::string rtsp_url_scheme;      // RTSP URL方案
    uint32_t rtsp_iv_counter;         // RTSP IV计数器
  };

  void launch_session_raise(std::shared_ptr<launch_session_t> launch_session); // 提交新的串流会话

  /**
   * @brief 清除指定会话的状态
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief 获取当前活跃会话数量
   */
  int session_count();

  /**
   * @brief 终止所有运行中的串流会话
   */
  void terminate_sessions();

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
