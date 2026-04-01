/**
 * @file src/audio.h
 * @brief 音频捕获和编码的声明
 * 使用Opus编码器将捕获的音频编码后发送给Moonlight客户端
 */
#pragma once

// 本地项目头文件
#include "platform/common.h" // 平台公共接口（音频捕获）
#include "thread_safe.h"     // 线程安全工具
#include "utility.h"          // 工具函数

#include <bitset>

namespace audio {
  /**
   * @brief Opus音频流配置枚举
   */
  enum stream_config_e : int {
    STEREO,  ///< 立体声（2声道）
    HIGH_STEREO,  ///< 高质量立体声
    SURROUND51,  ///< 5.1环绕声
    HIGH_SURROUND51,  ///< 高质量5.1环绕声
    SURROUND71,  ///< 7.1环绕声
    HIGH_SURROUND71,  ///< 高质量7.1环绕声
    MAX_STREAM_CONFIG  ///< 音频流配置数量上限
  };

  /**
   * @brief Opus编码器流配置结构体
   */
  struct opus_stream_config_t {
    std::int32_t sampleRate;       // 采样率（Hz）
    int channelCount;              // 声道数
    int streams;                   // 独立流数
    int coupledStreams;            // 耦合流数（立体声对）
    const std::uint8_t *mapping;  // 声道映射表
    int bitrate;                   // 比特率（bps）
  };

  /**
   * @brief 音频流参数结构体
   */
  struct stream_params_t {
    int channelCount;        // 声道数
    int streams;             // 独立流数
    int coupledStreams;      // 耦合流数
    std::uint8_t mapping[8]; // 声道映射表
  };

  extern opus_stream_config_t stream_configs[MAX_STREAM_CONFIG]; // 预定义的Opus流配置数组

  /**
   * @brief 音频配置结构体（由Moonlight客户端传递）
   */
  struct config_t {
    enum flags_e : int {
      HIGH_QUALITY,  ///< 高质量音频模式
      HOST_AUDIO,  ///< 同时在主机播放音频
      CUSTOM_SURROUND_PARAMS,  ///< 自定义环绕声参数
      CONTINUOUS_AUDIO,  ///< 持续音频模式（不中断）
      MAX_FLAGS  ///< 标志位数量上限
    };

    int packetDuration;  // 音频包时长（毫秒）
    int channels;        // 声道数
    int mask;            // 声道布局掩码

    stream_params_t customStreamParams; // 自定义的流参数

    std::bitset<MAX_FLAGS> flags; // 配置标志位集
  };

  /**
   * @brief 音频上下文结构体（管理音频捕获控制和音频接收器）
   */
  struct audio_ctx_t {
    // 仅对第一个串流会话切换音频接收器
    std::unique_ptr<std::atomic_bool> sink_flag;

    std::unique_ptr<platf::audio_control_t> control; // 平台级音频控制接口

    bool restore_sink;    // 是否在串流结束后恢复原音频接收器
    platf::sink_t sink;   // 当前音频接收器信息
  };

  using buffer_t = util::buffer_t<std::uint8_t>;      // 音频数据缓冲区
  using packet_t = std::pair<void *, buffer_t>;         // 音频数据包（通道数据 + 编码后的字节）
  using audio_ctx_ref_t = safe::shared_t<audio_ctx_t>::ptr_t; // 音频上下文的线程安全共享指针

  /**
   * @brief 音频捕获主函数（在串流线程中运行）
   * 持续捕获系统音频，Opus编码后通过邮件系统发送给串流模块
   */
  void capture(safe::mail_t mail, config_t config, void *channel_data);

  /**
   * @brief 获取音频上下文的共享引用
   * 可用于延长音频接收器的生命周期，确保接收器能被正确恢复
   */
  audio_ctx_ref_t get_audio_ctx_ref();

  /**
   * @brief Check if the audio sink held by audio context is available.
   * @returns True if available (and can probably be restored), false otherwise.
   * @note Useful for delaying the release of audio context shared pointer (which
   *       tries to restore original sink).
   *
   * @examples
   * audio_ctx_ref_t audio = get_audio_ctx_ref()
   * if (audio.get()) {
   *     return is_audio_ctx_sink_available(*audio.get());
   * }
   * return false;
   * @examples_end
   */
  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx);
}  // namespace audio
