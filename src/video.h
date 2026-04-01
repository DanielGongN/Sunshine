/**
 * @file src/video.h
 * @brief 视频编码和捕获的声明
 * 支持H.264/HEVC/AV1硬件编码，基于FFmpeg和平台特定编码器（NVENC、VAAPI、VideoToolbox等）
 */
#pragma once

// 本地项目头文件
#include "input.h"            // 输入子系统
#include "platform/common.h"  // 平台公共接口
#include "thread_safe.h"      // 线程安全工具
#include "video_colorspace.h" // 视频色彩空间处理

extern "C" {
#include <libavcodec/avcodec.h> // FFmpeg视频编解码器
#include <libswscale/swscale.h> // FFmpeg图像缩放和格式转换
}

struct AVPacket;

namespace video {

  /**
   * @brief 远程客户端请求的视频编码配置
   */
  struct config_t {
    int width;  // 视频宽度（像素）
    int height;  // 视频高度（像素）
    int framerate;  // 帧率（用于计算单帧比特率预算）
    int framerateX100;  // 帧率×100（支持NTSC等非integer帧率，如59.94=5994）
    int bitrate;  // 视频比特率（千比特/秒）
    int slicesPerFrame;  // 每帧切片数（用于并行编码）
    int numRefFrames;  // 最大参考帧数量

    /* 色彩范围和SDR编码色彩空间设置
       HDR编码色彩空间始终为 BT.2020+ST2084
       色彩范围 (encoderCscMode & 0x1): 0-有限范围, 1-全范围
       SDR色彩空间 (encoderCscMode >> 1): 0-BT.601, 1-BT.709, 2-BT.2020 */
    int encoderCscMode;

    int videoFormat;  // 视频格式: 0-H.264, 1-HEVC, 2-AV1

    /* 编码色彩深度: 0-8位, 1-10位
       当色彩深度>8位且捕获的显示器处于HDR模式时，激活HDR编码 */
    int dynamicRange;

    int chromaSamplingType;  // 色度采样类型: 0-4:2:0, 1-4:4:4

    int enableIntraRefresh;  // 帧内刷新: 0-禁用, 1-启用
  };

  platf::mem_type_e map_base_dev_type(AVHWDeviceType type);
  platf::pix_fmt_e map_pix_fmt(AVPixelFormat fmt);

  void free_ctx(AVCodecContext *ctx);
  void free_frame(AVFrame *frame);
  void free_buffer(AVBufferRef *ref);

  using avcodec_ctx_t = util::safe_ptr<AVCodecContext, free_ctx>;
  using avcodec_frame_t = util::safe_ptr<AVFrame, free_frame>;
  using avcodec_buffer_t = util::safe_ptr<AVBufferRef, free_buffer>;
  using sws_t = util::safe_ptr<SwsContext, sws_freeContext>;
  using img_event_t = std::shared_ptr<safe::event_t<std::shared_ptr<platf::img_t>>>;

  struct encoder_platform_formats_t {
    virtual ~encoder_platform_formats_t() = default;
    platf::mem_type_e dev_type;
    platf::pix_fmt_e pix_fmt_8bit;
    platf::pix_fmt_e pix_fmt_10bit;
    platf::pix_fmt_e pix_fmt_yuv444_8bit;
    platf::pix_fmt_e pix_fmt_yuv444_10bit;
  };

  struct encoder_platform_formats_avcodec: encoder_platform_formats_t {
    using init_buffer_function_t = std::function<util::Either<avcodec_buffer_t, int>(platf::avcodec_encode_device_t *)>;

    encoder_platform_formats_avcodec(
      const AVHWDeviceType &avcodec_base_dev_type,
      const AVHWDeviceType &avcodec_derived_dev_type,
      const AVPixelFormat &avcodec_dev_pix_fmt,
      const AVPixelFormat &avcodec_pix_fmt_8bit,
      const AVPixelFormat &avcodec_pix_fmt_10bit,
      const AVPixelFormat &avcodec_pix_fmt_yuv444_8bit,
      const AVPixelFormat &avcodec_pix_fmt_yuv444_10bit,
      const init_buffer_function_t &init_avcodec_hardware_input_buffer_function
    ):
        avcodec_base_dev_type {avcodec_base_dev_type},
        avcodec_derived_dev_type {avcodec_derived_dev_type},
        avcodec_dev_pix_fmt {avcodec_dev_pix_fmt},
        avcodec_pix_fmt_8bit {avcodec_pix_fmt_8bit},
        avcodec_pix_fmt_10bit {avcodec_pix_fmt_10bit},
        avcodec_pix_fmt_yuv444_8bit {avcodec_pix_fmt_yuv444_8bit},
        avcodec_pix_fmt_yuv444_10bit {avcodec_pix_fmt_yuv444_10bit},
        init_avcodec_hardware_input_buffer {init_avcodec_hardware_input_buffer_function} {
      dev_type = map_base_dev_type(avcodec_base_dev_type);
      pix_fmt_8bit = map_pix_fmt(avcodec_pix_fmt_8bit);
      pix_fmt_10bit = map_pix_fmt(avcodec_pix_fmt_10bit);
      pix_fmt_yuv444_8bit = map_pix_fmt(avcodec_pix_fmt_yuv444_8bit);
      pix_fmt_yuv444_10bit = map_pix_fmt(avcodec_pix_fmt_yuv444_10bit);
    }

    AVHWDeviceType avcodec_base_dev_type;
    AVHWDeviceType avcodec_derived_dev_type;
    AVPixelFormat avcodec_dev_pix_fmt;
    AVPixelFormat avcodec_pix_fmt_8bit;
    AVPixelFormat avcodec_pix_fmt_10bit;
    AVPixelFormat avcodec_pix_fmt_yuv444_8bit;
    AVPixelFormat avcodec_pix_fmt_yuv444_10bit;

    init_buffer_function_t init_avcodec_hardware_input_buffer;
  };

  struct encoder_platform_formats_nvenc: encoder_platform_formats_t {
    encoder_platform_formats_nvenc(
      const platf::mem_type_e &dev_type,
      const platf::pix_fmt_e &pix_fmt_8bit,
      const platf::pix_fmt_e &pix_fmt_10bit,
      const platf::pix_fmt_e &pix_fmt_yuv444_8bit,
      const platf::pix_fmt_e &pix_fmt_yuv444_10bit
    ) {
      encoder_platform_formats_t::dev_type = dev_type;
      encoder_platform_formats_t::pix_fmt_8bit = pix_fmt_8bit;
      encoder_platform_formats_t::pix_fmt_10bit = pix_fmt_10bit;
      encoder_platform_formats_t::pix_fmt_yuv444_8bit = pix_fmt_yuv444_8bit;
      encoder_platform_formats_t::pix_fmt_yuv444_10bit = pix_fmt_yuv444_10bit;
    }
  };

  struct encoder_t {
    std::string_view name;

    enum flag_e {
      PASSED,  ///< Indicates the encoder is supported.
      REF_FRAMES_RESTRICT,  ///< Set maximum reference frames.
      DYNAMIC_RANGE,  ///< HDR support.
      YUV444,  ///< YUV 4:4:4 support.
      VUI_PARAMETERS,  ///< AMD encoder with VAAPI doesn't add VUI parameters to SPS.
      MAX_FLAGS  ///< Maximum number of flags.
    };

    static std::string_view from_flag(flag_e flag) {
#define _CONVERT(x) \
  case flag_e::x: \
    return std::string_view(#x)
      switch (flag) {
        _CONVERT(PASSED);
        _CONVERT(REF_FRAMES_RESTRICT);
        _CONVERT(DYNAMIC_RANGE);
        _CONVERT(YUV444);
        _CONVERT(VUI_PARAMETERS);
        _CONVERT(MAX_FLAGS);
      }
#undef _CONVERT

      return {"unknown"};
    }

    struct option_t {
      KITTY_DEFAULT_CONSTR_MOVE(option_t)
      option_t(const option_t &) = default;

      std::string name;
      std::variant<int, int *, std::optional<int> *, std::function<int()>, std::string, std::string *, std::function<const std::string(const config_t &)>> value;

      option_t(std::string &&name, decltype(value) &&value):
          name {std::move(name)},
          value {std::move(value)} {
      }
    };

    const std::unique_ptr<const encoder_platform_formats_t> platform_formats;

    struct codec_t {
      std::vector<option_t> common_options;
      std::vector<option_t> sdr_options;
      std::vector<option_t> hdr_options;
      std::vector<option_t> sdr444_options;
      std::vector<option_t> hdr444_options;
      std::vector<option_t> fallback_options;

      std::string name;
      std::bitset<MAX_FLAGS> capabilities;

      bool operator[](flag_e flag) const {
        return capabilities[(std::size_t) flag];
      }

      std::bitset<MAX_FLAGS>::reference operator[](flag_e flag) {
        return capabilities[(std::size_t) flag];
      }
    };

    codec_t av1;
    codec_t hevc;
    codec_t h264;

    const codec_t &codec_from_config(const config_t &config) const {
      switch (config.videoFormat) {
        default:
          BOOST_LOG(error) << "Unknown video format " << config.videoFormat << ", falling back to H.264";
          // fallthrough
        case 0:
          return h264;
        case 1:
          return hevc;
        case 2:
          return av1;
      }
    }

    uint32_t flags;
  };

  struct encode_session_t {
    virtual ~encode_session_t() = default;

    virtual int convert(platf::img_t &img) = 0;

    virtual void request_idr_frame() = 0;

    virtual void request_normal_frame() = 0;

    virtual void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) = 0;
  };

  // encoders
  extern encoder_t software;

#if !defined(__APPLE__)
  extern encoder_t nvenc;  // available for windows and linux
#endif

#ifdef _WIN32
  extern encoder_t amdvce;
  extern encoder_t quicksync;
  extern encoder_t mediafoundation;
#endif

#if defined(__linux__) || defined(linux) || defined(__linux) || defined(__FreeBSD__)
  extern encoder_t vaapi;
#endif

#ifdef __APPLE__
  extern encoder_t videotoolbox;
#endif

  struct packet_raw_t {
    virtual ~packet_raw_t() = default;

    virtual bool is_idr() = 0;

    virtual int64_t frame_index() = 0;

    virtual uint8_t *data() = 0;

    virtual size_t data_size() = 0;

    struct replace_t {
      std::string_view old;
      std::string_view _new;

      KITTY_DEFAULT_CONSTR_MOVE(replace_t)

      replace_t(std::string_view old, std::string_view _new) noexcept:
          old {std::move(old)},
          _new {std::move(_new)} {
      }
    };

    std::vector<replace_t> *replacements = nullptr;
    void *channel_data = nullptr;
    bool after_ref_frame_invalidation = false;
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
  };

  struct packet_raw_avcodec: packet_raw_t {
    packet_raw_avcodec() {
      av_packet = av_packet_alloc();
    }

    ~packet_raw_avcodec() {
      av_packet_free(&this->av_packet);
    }

    bool is_idr() override {
      return av_packet->flags & AV_PKT_FLAG_KEY;
    }

    int64_t frame_index() override {
      return av_packet->pts;
    }

    uint8_t *data() override {
      return av_packet->data;
    }

    size_t data_size() override {
      return av_packet->size;
    }

    AVPacket *av_packet;
  };

  struct packet_raw_generic: packet_raw_t {
    packet_raw_generic(std::vector<uint8_t> &&frame_data, int64_t frame_index, bool idr):
        frame_data {std::move(frame_data)},
        index {frame_index},
        idr {idr} {
    }

    bool is_idr() override {
      return idr;
    }

    int64_t frame_index() override {
      return index;
    }

    uint8_t *data() override {
      return frame_data.data();
    }

    size_t data_size() override {
      return frame_data.size();
    }

    std::vector<uint8_t> frame_data;
    int64_t index;
    bool idr;
  };

  using packet_t = std::unique_ptr<packet_raw_t>;

  struct hdr_info_raw_t {
    explicit hdr_info_raw_t(bool enabled):
        enabled {enabled},
        metadata {} {};
    explicit hdr_info_raw_t(bool enabled, const SS_HDR_METADATA &metadata):
        enabled {enabled},
        metadata {metadata} {};

    bool enabled;
    SS_HDR_METADATA metadata;
  };

  using hdr_info_t = std::unique_ptr<hdr_info_raw_t>;

  extern int active_hevc_mode;
  extern int active_av1_mode;
  extern bool last_encoder_probe_supported_ref_frames_invalidation;
  extern std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec;  // 0 - H.264, 1 - HEVC, 2 - AV1

  void capture(
    safe::mail_t mail,
    config_t config,
    void *channel_data
  );

  bool validate_encoder(encoder_t &encoder, bool expect_failure);

  /**
   * @brief Probe encoders and select the preferred encoder.
   * This is called once at startup and each time a stream is launched to
   * ensure the best encoder is selected. Encoder availability can change
   * at runtime due to all sorts of things from driver updates to eGPUs.
   *
   * @warning This is only safe to call when there is no client actively streaming.
   */
  int probe_encoders();

  // Several NTSC standard refresh rates are hardcoded here, because their
  // true rate requires a denominator of 1001. ffmpeg's av_d2q() would assume it could
  // reduce 29.97 to 2997/100 but this would be slightly wrong. We also include
  // support for 23.976 film in case someone wants to stream a film at the perfect
  // framerate.
  inline AVRational framerateX100_to_rational(const int framerateX100) {
    if (framerateX100 % 2997 == 0) {
      // Multiples of NTSC 29.97 e.g. 59.94, 119.88
      return AVRational {(framerateX100 / 2997) * 30000, 1001};
    }
    switch (framerateX100) {
      case 2397:  // the other weird NTSC framerate, assume these want 23.976 film
      case 2398:
        return AVRational {24000, 1001};
      default:
        // any other fractional rate can be reduced by ffmpeg. Max is set to 1 << 26 based on docs:
        // "rational numbers with |num| <= 1<<26 && |den| <= 1<<26 can be recovered exactly from their double representation"
        return av_d2q((double) framerateX100 / 100.0f, 1 << 26);
    }
  }
}  // namespace video
