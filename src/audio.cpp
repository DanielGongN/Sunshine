/**
 * @file src/audio.cpp
 * @brief 音频捕获和编码的实现
 * 从系统音频设备捕获PCM数据，使用Opus编码器形成音频包发送给客户端
 */
// 标准库头文件
#include <thread>

// 第三方库头文件
#include <opus/opus_multistream.h> // Opus多流编码器（支持环绕声）

// 本地项目头文件
#include "audio.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "utility.h"

namespace audio {
  using namespace std::literals;
  using opus_t = util::safe_ptr<OpusMSEncoder, opus_multistream_encoder_destroy>;  // Opus多流编码器智能指针（自动释放）
  using sample_queue_t = std::shared_ptr<safe::queue_t<std::vector<float>>>;  // 音频采样队列（线程安全）

  // 前向声明内部函数
  static int start_audio_control(audio_ctx_t &ctx);  // 启动音频控制（初始化音频设备）
  static void stop_audio_control(audio_ctx_t &);  // 停止音频控制（恢复音频设备）
  static void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params);  // 应用自定义环绕声参数

  int map_stream(int channels, bool quality);  // 根据声道数和质量选择流配置索引

  constexpr auto SAMPLE_RATE = 48000;  // 固定采样率48kHz（Opus标准）

  // 注意：如果调整这里的码率，请同步更新 rtsp_stream::cmd_announce() 中的码率调整逻辑
  // 音频流配置数组：[立体声普通, 立体声高质量, 5.1普通, 5.1高质量, 7.1普通, 7.1高质量]
  opus_stream_config_t stream_configs[MAX_STREAM_CONFIG] {
    {  // STEREO（立体声普通质量）: 48kHz, 2声道, 1个流, 1个耦合流, 96kbps
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      96000,
    },
    {  // STEREO_HQ（立体声高质量）: 48kHz, 2声道, 1个流, 1个耦合流, 512kbps
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      512000,
    },
    {  // SURROUND51（5.1环绕声普通质量）: 48kHz, 6声道, 4个流, 2个耦合流, 256kbps
      SAMPLE_RATE,
      6,
      4,
      2,
      platf::speaker::map_surround51,
      256000,
    },
    {  // SURROUND51_HQ（5.1环绕声高质量）: 48kHz, 6声道, 6个独立流, 0个耦合, 1536kbps
      SAMPLE_RATE,
      6,
      6,
      0,
      platf::speaker::map_surround51,
      1536000,
    },
    {  // SURROUND71（7.1环绕声普通质量）: 48kHz, 8声道, 5个流, 3个耦合流, 450kbps
      SAMPLE_RATE,
      8,
      5,
      3,
      platf::speaker::map_surround71,
      450000,
    },
    {  // SURROUND71_HQ（7.1环绕声高质量）: 48kHz, 8声道, 8个独立流, 0个耦合, 2048kbps
      SAMPLE_RATE,
      8,
      8,
      0,
      platf::speaker::map_surround71,
      2048000,
    },
  };

  /**
   * @brief 音频编码线程函数。从采样队列取出PCM数据，用Opus编码后发送到音频包队列。
   * @param samples 输入的PCM采样队列（浮点格式）
   * @param config 音频配置（声道数、质量标志等）
   * @param channel_data 通道标识数据（用于区分不同客户端的音频流）
   */
  void encodeThread(sample_queue_t samples, config_t config, void *channel_data) {
    // 获取音频包输出队列（编码后的Opus包将发送到这里）
    auto packets = mail::man->queue<packet_t>(mail::audio_packets);

    // 根据声道数和质量选择对应的流配置
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    // 如果客户端提供了自定义环绕声参数，则覆盖默认配置
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    // 设置线程名称和高优先级（编码是CPU密集型任务）
    platf::set_thread_name("audio::encode");
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    // 创建Opus多流编码器实例
    // OPUS_APPLICATION_RESTRICTED_LOWDELAY: 使用限制性低延迟模式（适合实时流媒体）
    opus_t opus {opus_multistream_encoder_create(
      stream.sampleRate,
      stream.channelCount,
      stream.streams,
      stream.coupledStreams,
      stream.mapping,
      OPUS_APPLICATION_RESTRICTED_LOWDELAY,
      nullptr
    )};

    // 设置Opus编码参数
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_BITRATE(stream.bitrate));  // 设置目标码率
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_VBR(0));  // 禁用VBR，使用CBR恒定码率

    BOOST_LOG(info) << "Opus initialized: "sv << stream.sampleRate / 1000 << " kHz, "sv
                    << stream.channelCount << " channels, "sv
                    << stream.bitrate / 1000 << " kbps (total), LOWDELAY"sv;

    // 计算每帧采样数 = 包时长(ms) × 采样率 / 1000
    auto frame_size = config.packetDuration * stream.sampleRate / 1000;

    // 主编码循环：持续从队列取样本并编码
    while (auto sample = samples->pop()) {
      buffer_t packet {1400};  // 分配1400字节的输出缓冲区（适合单个UDP包大小）

      // 将浮点PCM样本编码为Opus比特流
      int bytes = opus_multistream_encode_float(opus.get(), sample->data(), frame_size, std::begin(packet), (opus_int32) packet.size());
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio: "sv << opus_strerror(bytes);
        packets->stop();  // 编码失败，停止输出队列
        return;
      }

      packet.fake_resize(bytes);  // 调整包大小为实际编码输出的字节数
      packets->raise(channel_data, std::move(packet));  // 将编码后的包发送到输出队列
    }
  }

  /**
   * @brief 音频捕获主函数。初始化音频设备，创建编码线程，循环采集音频样本。
   * @param mail 邮箱系统（用于接收关闭信号）
   * @param config 音频配置
   * @param channel_data 通道标识数据
   */
  void capture(safe::mail_t mail, config_t config, void *channel_data) {
    // 获取关闭事件，用于监听何时停止捕获
    auto shutdown_event = mail->event<bool>(mail::shutdown);

    // 如果配置中禁用了音频流，则等待关闭信号后直接返回
    if (!config::audio.stream) {
      shutdown_event->view();
      return;
    }

    // 根据声道数和质量选择流配置
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    // 获取共享的音频上下文引用（引用计数管理，首次调用时初始化音频设备）
    auto ref = get_audio_ctx_ref();
    if (!ref) {
      return;
    }

    // 初始化失败守卫：如果初始化过程中任何步骤失败，打印错误并等待关闭
    // 允许在无音频的情况下继续视频流
    auto init_failure_fg = util::fail_guard([&shutdown_event]() {
      BOOST_LOG(error) << "Unable to initialize audio capture. The stream will not have audio."sv;
      shutdown_event->view();  // 等待关闭信号
    });

    auto &control = ref->control;
    if (!control) {
      return;  // 音频控制器不可用
    }

    // 选择音频输出设备（sink），优先级从高到低：
    // 1. 配置文件中指定的虚拟sink
    // 2. 用户配置的音频sink
    // 3. 主机默认音频设备
    std::string *sink = &ref->sink.host;
    if (!config::audio.sink.empty()) {
      sink = &config::audio.sink;
    }

    // 如果禁用了主机音频播放或没有其他可用sink，优先使用虚拟sink
    if (ref->sink.null && (!config.flags[config_t::HOST_AUDIO] || sink->empty())) {
      auto &null = *ref->sink.null;
      // 根据声道数选择对应的虚拟音频设备
      switch (stream.channelCount) {
        case 2:
          sink = &null.stereo;  // 立体声虚拟设备
          break;
        case 6:
          sink = &null.surround51;  // 5.1环绕声虚拟设备
          break;
        case 8:
          sink = &null.surround71;  // 7.1环绕声虚拟设备
          break;
      }
    }

    // 只有第一个启动会话的客户端可以更改默认音频设备
    // 使用原子交换确保线程安全
    if (!ref->sink_flag->exchange(true, std::memory_order_acquire)) {
      // 如果选中的sink与当前不同，切换音频设备
      ref->restore_sink = ref->sink.host != *sink;
      if (ref->restore_sink) {
        if (control->set_sink(*sink)) {
          return;  // 切换失败
        }
      }
    }

    // 计算每帧采样数和获取配置标志
    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    bool host_audio = config.flags[config_t::HOST_AUDIO];  // 是否在主机上同步播放
    bool continuous_audio = config.flags[config_t::CONTINUOUS_AUDIO];  // 是否持续输出音频（即使静音）

    // 创建麦克风/音频捕获实例
    auto mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size, continuous_audio, host_audio);
    if (!mic) {
      return;  // 麦克风创建失败
    }

    // 音频初始化成功，禁用失败守卫（不再打印失败消息）
    init_failure_fg.disable();

    // 音频捕获使用关键优先级（最高）以确保实时性
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    // 创建采样队列（最多缓存30帧）并启动编码线程
    auto samples = std::make_shared<sample_queue_t::element_type>(30);
    std::thread thread {encodeThread, samples, config, channel_data};

    // 清理守卫：退出时停止采样队列、等待编码线程结束、等待关闭信号
    auto fg = util::fail_guard([&]() {
      samples->stop();
      thread.join();
      shutdown_event->view();
    });

    // 每帧总采样数 = 每声道采样数 × 声道数
    int samples_per_frame = frame_size * stream.channelCount;

    // 主采集循环：持续采集音频直到收到关闭信号
    while (!shutdown_event->peek()) {
      std::vector<float> sample_buffer;
      sample_buffer.resize(samples_per_frame);  // 为一帧音频分配缓冲区

      // 从音频设备采集一帧样本
      auto status = mic->sample(sample_buffer);
      switch (status) {
        case platf::capture_e::ok:
          break;  // 采集成功，继续处理
        case platf::capture_e::timeout:
          continue;  // 超时，重试
        case platf::capture_e::reinit:
          // 需要重新初始化音频设备（如设备断开/重连）
          BOOST_LOG(info) << "Reinitializing audio capture"sv;
          mic.reset();
          do {
            mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size, continuous_audio, host_audio);
            if (!mic) {
              BOOST_LOG(warning) << "Couldn't re-initialize audio input"sv;
            }
          } while (!mic && !shutdown_event->view(5s));  // 每5秒重试一次，直到成功或收到关闭信号
          continue;
        default:
          return;  // 其他错误，退出
      }

      // 将采集到的样本放入编码队列
      samples->raise(std::move(sample_buffer));
    }
  }

  /**
   * @brief 获取全局音频上下文的共享引用。
   * 使用static局部变量确保只初始化一次，引用计数管理生命周期。
   * 当最后一个引用释放时，自动调用stop_audio_control清理。
   */
  audio_ctx_ref_t get_audio_ctx_ref() {
    static auto control_shared {safe::make_shared<audio_ctx_t>(start_audio_control, stop_audio_control)};
    return control_shared.ref();
  }

  /**
   * @brief 检查音频上下文中的音频设备（sink）是否可用。
   * @return 如果sink设备存在且可用返回true。
   */
  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx) {
    if (!ctx.control) {
      return false;  // 音频控制器未初始化
    }

    // 优先使用主机sink，如果为空则使用配置文件中的sink
    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (sink.empty()) {
      return false;  // 没有可用的sink名称
    }

    return ctx.control->is_sink_available(sink);  // 向平台层查询设备是否存在
  }

  /**
   * @brief 根据声道数和质量标志映射到流配置数组索引。
   * @param channels 声道数（2=立体声, 6=5.1, 8=7.1）
   * @param quality 是否高质量模式（true则索引+1选择高码率配置）
   * @return 流配置数组索引
   */
  int map_stream(int channels, bool quality) {
    int shift = quality ? 1 : 0;  // 高质量模式偏移1
    switch (channels) {
      case 2:
        return STEREO + shift;     // 立体声: 索引0或1
      case 6:
        return SURROUND51 + shift; // 5.1: 索引2或3
      case 8:
        return SURROUND71 + shift; // 7.1: 索引4或5
    }
    return STEREO;  // 默认立体声
  }

  /**
   * @brief 启动音频控制（音频上下文初始化回调）。
   * 创建平台音频控制器，查询可用的音频设备信息。
   */
  int start_audio_control(audio_ctx_t &ctx) {
    // 失败守卫：如果初始化失败，打印警告
    auto fg = util::fail_guard([]() {
      BOOST_LOG(warning) << "There will be no audio"sv;
    });

    // 初始化原子标志（用于跟踪是否已切换音频设备）
    ctx.sink_flag = std::make_unique<std::atomic_bool>(false);

    // 标记默认音频设备尚未被替换
    ctx.restore_sink = false;

    // 创建平台特定的音频控制器（Windows: WASAPI, Linux: PulseAudio/PipeWire, macOS: CoreAudio）
    if (!(ctx.control = platf::audio_control())) {
      return 0;  // 创建失败，但不视为致命错误
    }

    // 查询当前音频设备信息（主机sink和虚拟sink名称）
    auto sink = ctx.control->sink_info();
    if (!sink) {
      // 查询失败，重置控制器让调用方知道
      ctx.control.reset();
      return 0;
    }

    ctx.sink = std::move(*sink);  // 保存音频设备信息

    fg.disable();  // 初始化成功，禁用失败守卫
    return 0;
  }

  /**
   * @brief 停止音频控制（音频上下文清理回调）。
   * 如果之前切换了音频设备，恢复到原始设备。
   */
  void stop_audio_control(audio_ctx_t &ctx) {
    // 如果没有替换过默认音频设备，无需恢复
    if (!ctx.restore_sink) {
      return;
    }

    // 恢复到之前的主机音频设备
    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (!sink.empty()) {
      // 尽力恢复，允许失败（设备可能已经不存在）
      ctx.control->set_sink(sink);
    }
  }

  /**
   * @brief 将客户端自定义的环绕声参数应用到流配置。
   * 覆盖默认的声道数、流数量、耦合流数量和声道映射。
   */
  void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params) {
    stream.channelCount = params.channelCount;    // 总声道数
    stream.streams = params.streams;              // Opus流数量
    stream.coupledStreams = params.coupledStreams; // 耦合（立体声配对）流数量
    stream.mapping = params.mapping;              // 声道到流的映射表
  }
}  // namespace audio
