/**
 * @file src/video.cpp
 * @brief 视频捕获和编码的实现
 * 屏幕捕获 -> 色彩空间转换 -> 硬件/软件编码(H.264/HEVC/AV1) -> 打包发送
 */
// 标准库头文件
#include <atomic>
#include <bitset>
#include <list>
#include <thread>

// 第三方库头文件
#include <boost/pointer_cast.hpp>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

// local includes
#include "cbs.h"
#include "config.h"
#include "display_device.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "nvenc/nvenc_base.h"
#include "platform/common.h"
#include "sync.h"
#include "video.h"

#ifdef _WIN32
extern "C" {
  #include <libavutil/hwcontext_d3d11va.h>
}
#endif

using namespace std::literals;

namespace video {

  namespace {
    /**
     * @brief Check if we can allow probing for the encoders.
     * @return True if there should be no issues with the probing, false if we should prevent it.
     */
    bool allow_encoder_probing() {
      const auto devices {display_device::enumerate_devices()};

      // If there are no devices, then either the API is not working correctly or OS does not support the lib.
      // Either way we should not block the probing in this case as we can't tell what's wrong.
      if (devices.empty()) {
        return true;
      }

      // Since Windows 11 24H2, it is possible that there will be no active devices present
      // for some reason (probably a bug). Trying to probe encoders in such a state locks/breaks the DXGI
      // and also the display device for Windows. So we must have at least 1 active device.
      const bool at_least_one_device_is_active = std::any_of(std::begin(devices), std::end(devices), [](const auto &device) {
        // If device has additional info, it is active.
        return static_cast<bool>(device.m_info);
      });

      if (at_least_one_device_is_active) {
        return true;
      }

      BOOST_LOG(error) << "No display devices are active at the moment! Cannot probe the encoders.";
      return false;
    }
  }  // namespace

  // === FFmpeg资源释放函数 ===
  void free_ctx(AVCodecContext *ctx) {
    avcodec_free_context(&ctx);  // 释放编码器上下文
  }

  void free_frame(AVFrame *frame) {
    av_frame_free(&frame);  // 释放视频帧
  }

  void free_buffer(AVBufferRef *ref) {
    av_buffer_unref(&ref);  // 释放缓冲区引用
  }

  namespace nv {

    enum class profile_h264_e : int {
      high = 2,  ///< High profile
      high_444p = 3,  ///< High 4:4:4 Predictive profile
    };

    enum class profile_hevc_e : int {
      main = 0,  ///< Main profile
      main_10 = 1,  ///< Main 10 profile
      rext = 2,  ///< Rext profile
    };

  }  // namespace nv

  namespace qsv {

    enum class profile_h264_e : int {
      high = 100,  ///< High profile
      high_444p = 244,  ///< High 4:4:4 Predictive profile
    };

    enum class profile_hevc_e : int {
      main = 1,  ///< Main profile
      main_10 = 2,  ///< Main 10 profile
      rext = 4,  ///< RExt profile
    };

    enum class profile_av1_e : int {
      main = 1,  ///< Main profile
      high = 2,  ///< High profile
    };

  }  // namespace qsv

  util::Either<avcodec_buffer_t, int> dxgi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  util::Either<avcodec_buffer_t, int> vaapi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  util::Either<avcodec_buffer_t, int> cuda_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  util::Either<avcodec_buffer_t, int> vt_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);

  /**
   * @brief 软件编码设备类 - 使用CPU进行色彩空间转换和缩放
   */
  class avcodec_software_encode_device_t: public platf::avcodec_encode_device_t {
  public:
    int convert(platf::img_t &img) override {
      // 检查是否需要宽高比填充（黑边），即输出尺寸与缩放后尺寸不同
      bool requires_padding = (sw_frame->width != sws_output_frame->width || sw_frame->height != sws_output_frame->height);

      // 设置输入帧数据指针：直接引用捕获图像的内存
      sws_input_frame->data[0] = img.data;
      sws_input_frame->linesize[0] = img.row_pitch;

      // 执行色彩空间转换和缩放（BGR0 -> YUV），如需填充则先缩放到中间缓冲区
      auto status = sws_scale_frame(sws.get(), requires_padding ? sws_output_frame.get() : sw_frame.get(), sws_input_frame.get());
      if (status < 0) {
        char string[AV_ERROR_MAX_STRING_SIZE];
        BOOST_LOG(error) << "Couldn't scale frame: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
        return -1;
      }

      // 如果需要宽高比填充，将缩放后的图像复制到带黑边的最终帧中
      if (requires_padding) {
        auto fmt_desc = av_pix_fmt_desc_get((AVPixelFormat) sws_output_frame->format);
        auto planes = av_pix_fmt_count_planes((AVPixelFormat) sws_output_frame->format);
        // 遍历每个YUV平面（Y/U/V），处理色度子采样的偏移
        for (int plane = 0; plane < planes; plane++) {
          auto shift_h = plane == 0 ? 0 : fmt_desc->log2_chroma_h;  // 色度垂直子采样
          auto shift_w = plane == 0 ? 0 : fmt_desc->log2_chroma_w;  // 色度水平子采样
          auto offset = ((offsetW >> shift_w) * fmt_desc->comp[plane].step) + (offsetH >> shift_h) * sw_frame->linesize[plane];

          // 逐行复制以保持每行的前导填充（黑边）
          for (int line = 0; line < sws_output_frame->height >> shift_h; line++) {
            memcpy(sw_frame->data[plane] + offset + (line * sw_frame->linesize[plane]), sws_output_frame->data[plane] + (line * sws_output_frame->linesize[plane]), (size_t) (sws_output_frame->width >> shift_w) * fmt_desc->comp[plane].step);
          }
        }
      }

      // 如果目标帧是硬件帧，需要从系统内存（RAM）上传到显存（VRAM）
      if (frame->hw_frames_ctx) {
        auto status = av_hwframe_transfer_data(frame, sw_frame.get(), 0);
        if (status < 0) {
          char string[AV_ERROR_MAX_STRING_SIZE];
          BOOST_LOG(error) << "Failed to transfer image data to hardware frame: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
          return -1;
        }
      }

      return 0;
    }

    int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) override {
      this->frame = frame;

      // 如果是硬件帧，在GPU上分配帧缓冲区
      if (hw_frames_ctx) {
        hw_frame.reset(frame);

        if (av_hwframe_get_buffer(hw_frames_ctx, frame, 0)) {
          return -1;
        }
      } else {
        sw_frame.reset(frame);
      }

      return 0;
    }

    void apply_colorspace() override {
      // 设置SWS色彩空间转换参数：源空间 -> 目标空间（Rec.601/709/2020）
      auto avcodec_colorspace = avcodec_colorspace_from_sunshine_colorspace(colorspace);
      sws_setColorspaceDetails(sws.get(), sws_getCoefficients(SWS_CS_DEFAULT), 0, sws_getCoefficients(avcodec_colorspace.software_format), avcodec_colorspace.range - 1, 0, 1 << 16, 1 << 16);
    }

      // 用于保持宽高比时，预填充黑色背景
    void prefill() {
      auto frame = sw_frame ? sw_frame.get() : this->frame;
      av_frame_get_buffer(frame, 0);
      av_frame_make_writable(frame);
      ptrdiff_t linesize[4] = {frame->linesize[0], frame->linesize[1], frame->linesize[2], frame->linesize[3]};
      av_image_fill_black(frame->data, linesize, (AVPixelFormat) frame->format, frame->color_range, frame->width, frame->height);
    }

    /**
     * @brief 初始化软件编码设备：创建SWS缩放器，处理宽高比保持和黑边填充
     */
    int init(int in_width, int in_height, AVFrame *frame, AVPixelFormat format, bool hardware) {
      // 硬件编码但图像在内存中时，需要额外的软件帧用于CPU->GPU传输
      if (hardware) {
        sw_frame.reset(av_frame_alloc());

        sw_frame->width = frame->width;
        sw_frame->height = frame->height;
        sw_frame->format = format;
      } else {
        this->frame = frame;
      }

      // Fill aspect ratio padding in the destination frame
      prefill();

      auto out_width = frame->width;
      auto out_height = frame->height;

      // Ensure aspect ratio is maintained
      auto scalar = std::fminf((float) out_width / in_width, (float) out_height / in_height);
      out_width = in_width * scalar;
      out_height = in_height * scalar;

      // 输入帧：BGR0格式的捕获图像
      sws_input_frame.reset(av_frame_alloc());
      sws_input_frame->width = in_width;
      sws_input_frame->height = in_height;
      sws_input_frame->format = AV_PIX_FMT_BGR0;

      // 输出帧：缩放后的YUV图像（可能小于最终帧尺寸）
      sws_output_frame.reset(av_frame_alloc());
      sws_output_frame->width = out_width;
      sws_output_frame->height = out_height;
      sws_output_frame->format = format;

      // 计算宽高比填充的偏移量（两侧均匀分配黑边）
      offsetW = (frame->width - out_width) / 2;
      offsetH = (frame->height - out_height) / 2;

      // 创建SWS缩放上下文，使用Lanczos算法进行高质量缩放
      sws.reset(sws_alloc_context());
      if (!sws) {
        return -1;
      }

      AVDictionary *options {nullptr};
      av_dict_set_int(&options, "srcw", sws_input_frame->width, 0);
      av_dict_set_int(&options, "srch", sws_input_frame->height, 0);
      av_dict_set_int(&options, "src_format", sws_input_frame->format, 0);
      av_dict_set_int(&options, "dstw", sws_output_frame->width, 0);
      av_dict_set_int(&options, "dsth", sws_output_frame->height, 0);
      av_dict_set_int(&options, "dst_format", sws_output_frame->format, 0);
      av_dict_set_int(&options, "sws_flags", SWS_LANCZOS | SWS_ACCURATE_RND, 0);
      av_dict_set_int(&options, "threads", config::video.min_threads, 0);

      auto status = av_opt_set_dict(sws.get(), &options);
      av_dict_free(&options);
      if (status < 0) {
        char string[AV_ERROR_MAX_STRING_SIZE];
        BOOST_LOG(error) << "Failed to set SWS options: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
        return -1;
      }

      status = sws_init_context(sws.get(), nullptr, nullptr);
      if (status < 0) {
        char string[AV_ERROR_MAX_STRING_SIZE];
        BOOST_LOG(error) << "Failed to initialize SWS: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
        return -1;
      }

      return 0;
    }

    avcodec_frame_t hw_frame;    // 硬件帧（GPU内存）
    avcodec_frame_t sw_frame;     // 软件帧（系统内存）
    avcodec_frame_t sws_input_frame;   // 色彩转换输入帧
    avcodec_frame_t sws_output_frame;  // 色彩转换输出帧
    sws_t sws;  // SWS缩放上下文

    // 输入图像在输出帧中的像素偏移（用于宽高比填充）
    int offsetW;
    int offsetH;
  };

  // === 编码器特性标志位 ===
  enum flag_e : uint32_t {
    DEFAULT = 0,                      ///< 默认标志
    PARALLEL_ENCODING = 1 << 1,       ///< 捕获和编码可以在不同线程上并行运行
    H264_ONLY = 1 << 2,               ///< 仅支持H.264（HEVC太重时使用）
    LIMITED_GOP_SIZE = 1 << 3,         ///< 部分编码器不支持无限GOP（如VAAPI）
    SINGLE_SLICE_ONLY = 1 << 4,       ///< 禁用多切片（旧版Intel核显问题）
    CBR_WITH_VBR = 1 << 5,            ///< 用VBR模式模拟CBR
    RELAXED_COMPLIANCE = 1 << 6,      ///< 使用非官方合规模式
    NO_RC_BUF_LIMIT = 1 << 7,         ///< 不设置rc_buffer_size
    REF_FRAMES_INVALIDATION = 1 << 8, ///< 支持参考帧失效
    ALWAYS_REPROBE = 1 << 9,          ///< 低优先级编码器，每次重新探测
    YUV444_SUPPORT = 1 << 10,         ///< 可能支持YUV 4:4:4色度采样
    ASYNC_TEARDOWN = 1 << 11,         ///< 支持异步拆除（避免NVENC挂起）
    FIXED_GOP_SIZE = 1 << 12,         ///< 固定小GOP（不支持按需IDR帧）
  };;

  /**
   * @brief AVCodec编码会话类 - 封装FFmpeg软件/硬件编码器
   */
  class avcodec_encode_session_t: public encode_session_t {
  public:
    avcodec_encode_session_t() = default;

    avcodec_encode_session_t(avcodec_ctx_t &&avcodec_ctx, std::unique_ptr<platf::avcodec_encode_device_t> encode_device, int inject):
        avcodec_ctx {std::move(avcodec_ctx)},
        device {std::move(encode_device)},
        inject {inject} {
    }

    avcodec_encode_session_t(avcodec_encode_session_t &&other) noexcept = default;

    ~avcodec_encode_session_t() {
      // 刷新编码器中的剩余帧
      if (avcodec_send_frame(avcodec_ctx.get(), nullptr) == 0) {
        packet_raw_avcodec pkt;
        while (avcodec_receive_packet(avcodec_ctx.get(), pkt.av_packet) == 0);  // 接收并丢弃缓存的包
      }

      // 释放顺序关键：编码器上下文依赖硬件设备，必须先释放上下文
      avcodec_ctx.reset();
      device.reset();
    }

    // Ensure objects are destroyed in the correct order
    avcodec_encode_session_t &operator=(avcodec_encode_session_t &&other) {
      device = std::move(other.device);
      avcodec_ctx = std::move(other.avcodec_ctx);
      replacements = std::move(other.replacements);
      sps = std::move(other.sps);
      vps = std::move(other.vps);

      inject = other.inject;

      return *this;
    }

    int convert(platf::img_t &img) override {
      if (!device) {
        return -1;
      }
      return device->convert(img);
    }

    void request_idr_frame() override {
      if (device && device->frame) {
        auto &frame = device->frame;
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->flags |= AV_FRAME_FLAG_KEY;
      }
    }

    void request_normal_frame() override {
      if (device && device->frame) {
        auto &frame = device->frame;
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        frame->flags &= ~AV_FRAME_FLAG_KEY;
      }
    }

    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      BOOST_LOG(error) << "Encoder doesn't support reference frame invalidation";
      request_idr_frame();
    }

    avcodec_ctx_t avcodec_ctx;
    std::unique_ptr<platf::avcodec_encode_device_t> device;

    std::vector<packet_raw_t::replace_t> replacements;

    cbs::nal_t sps;
    cbs::nal_t vps;

    // inject sps/vps data into idr pictures
    int inject;
  };

  /**
   * @brief NVENC编码会话类 - 封装NVIDIA硬件编码器
   */
  class nvenc_encode_session_t: public encode_session_t {
  public:
    nvenc_encode_session_t(std::unique_ptr<platf::nvenc_encode_device_t> encode_device):
        device(std::move(encode_device)) {
    }

    int convert(platf::img_t &img) override {
      if (!device) {
        return -1;
      }
      return device->convert(img);
    }

    void request_idr_frame() override {
      force_idr = true;
    }

    void request_normal_frame() override {
      force_idr = false;
    }

    /**
     * @brief 失效参考帧范围，失败时回退到强制IDR帧
     */
    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      if (!device || !device->nvenc) {
        return;
      }

      // 如果失效失败，强制下一帧为IDR关键帧
      if (!device->nvenc->invalidate_ref_frames(first_frame, last_frame)) {
        force_idr = true;
      }
    }

    /**
     * @brief 调用NVENC编码一帧，编码后清除IDR强制标志
     */
    nvenc::nvenc_encoded_frame encode_frame(uint64_t frame_index) {
      if (!device || !device->nvenc) {
        return {};
      }

      auto result = device->nvenc->encode_frame(frame_index, force_idr);
      force_idr = false;
      return result;
    }

  private:
    std::unique_ptr<platf::nvenc_encode_device_t> device;
    bool force_idr = false;
  };

  // === 同步编码会话的上下文和配置结构 ===
  struct sync_session_ctx_t {
    safe::signal_t *join_event;                                // 线程完成信号
    safe::mail_raw_t::event_t<bool> shutdown_event;            // 会话关闭事件
    safe::mail_raw_t::queue_t<packet_t> packets;               // 编码后的数据包队列
    safe::mail_raw_t::event_t<bool> idr_events;                // IDR帧请求事件
    safe::mail_raw_t::event_t<hdr_info_t> hdr_events;          // HDR信息更新事件
    safe::mail_raw_t::event_t<input::touch_port_t> touch_port_events;  // 触摸端口事件

    config_t config;      // 编码配置
    int frame_nr;         // 帧计数器
    void *channel_data;   // 通道数据指针
  };

  struct sync_session_t {
    sync_session_ctx_t *ctx;
    std::unique_ptr<encode_session_t> session;
  };

  using encode_session_ctx_queue_t = safe::queue_t<sync_session_ctx_t>;
  using encode_e = platf::capture_e;

  struct capture_ctx_t {
    img_event_t images;
    config_t config;
  };

  struct capture_thread_async_ctx_t {
    std::shared_ptr<safe::queue_t<capture_ctx_t>> capture_ctx_queue;
    std::thread capture_thread;

    safe::signal_t reinit_event;
    const encoder_t *encoder_p;
    sync_util::sync_t<std::weak_ptr<platf::display_t>> display_wp;
  };

  struct capture_thread_sync_ctx_t {
    encode_session_ctx_queue_t encode_session_ctx_queue {30};
  };

  int start_capture_sync(capture_thread_sync_ctx_t &ctx);
  void end_capture_sync(capture_thread_sync_ctx_t &ctx);
  int start_capture_async(capture_thread_async_ctx_t &ctx);
  void end_capture_async(capture_thread_async_ctx_t &ctx);

  // Keep a reference counter to ensure the capture thread only runs when other threads have a reference to the capture thread
  auto capture_thread_async = safe::make_shared<capture_thread_async_ctx_t>(start_capture_async, end_capture_async);
  auto capture_thread_sync = safe::make_shared<capture_thread_sync_ctx_t>(start_capture_sync, end_capture_sync);

#ifdef _WIN32
  encoder_t nvenc {
    "nvenc"sv,
    std::make_unique<encoder_platform_formats_nvenc>(
      platf::mem_type_e::dxgi,
      platf::pix_fmt_e::nv12,
      platf::pix_fmt_e::p010,
      platf::pix_fmt_e::ayuv,
      platf::pix_fmt_e::yuv444p16
    ),
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_nvenc"s,
    },
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_nvenc"s,
    },
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_nvenc"s,
    },
    PARALLEL_ENCODING | REF_FRAMES_INVALIDATION | YUV444_SUPPORT | ASYNC_TEARDOWN  // flags
  };
#elif !defined(__APPLE__)
  encoder_t nvenc {
    "nvenc"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
  #ifdef _WIN32
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
  #else
      AV_HWDEVICE_TYPE_CUDA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_CUDA,
  #endif
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
  #ifdef _WIN32
      dxgi_init_avcodec_hardware_input_buffer
  #else
      cuda_init_avcodec_hardware_input_buffer
  #endif
    ),
    {
      // Common options
      {
        {"delay"s, 0},
        {"forced-idr"s, 1},
        {"zerolatency"s, 1},
        {"surfaces"s, 1},
        {"cbr_padding"s, false},
        {"preset"s, &config::video.nv_legacy.preset},
        {"tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {"rc"s, NV_ENC_PARAMS_RC_CBR},
        {"multipass"s, &config::video.nv_legacy.multipass},
        {"aq"s, &config::video.nv_legacy.aq},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_nvenc"s,
    },
    {
      // Common options
      {
        {"delay"s, 0},
        {"forced-idr"s, 1},
        {"zerolatency"s, 1},
        {"surfaces"s, 1},
        {"cbr_padding"s, false},
        {"preset"s, &config::video.nv_legacy.preset},
        {"tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {"rc"s, NV_ENC_PARAMS_RC_CBR},
        {"multipass"s, &config::video.nv_legacy.multipass},
        {"aq"s, &config::video.nv_legacy.aq},
      },
      {
        // SDR-specific options
        {"profile"s, (int) nv::profile_hevc_e::main},
      },
      {
        // HDR-specific options
        {"profile"s, (int) nv::profile_hevc_e::main_10},
      },
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_nvenc"s,
    },
    {
      {
        {"delay"s, 0},
        {"forced-idr"s, 1},
        {"zerolatency"s, 1},
        {"surfaces"s, 1},
        {"cbr_padding"s, false},
        {"preset"s, &config::video.nv_legacy.preset},
        {"tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {"rc"s, NV_ENC_PARAMS_RC_CBR},
        {"coder"s, &config::video.nv_legacy.h264_coder},
        {"multipass"s, &config::video.nv_legacy.multipass},
        {"aq"s, &config::video.nv_legacy.aq},
      },
      {
        // SDR-specific options
        {"profile"s, (int) nv::profile_h264_e::high},
      },
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_nvenc"s,
    },
    PARALLEL_ENCODING
  };
#endif

#ifdef _WIN32
  encoder_t quicksync {
    "quicksync"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_QSV,
      AV_PIX_FMT_QSV,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_VUYX,
      AV_PIX_FMT_XV30,
      dxgi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"preset"s, &config::video.qsv.qsv_preset},
        {"forced_idr"s, 1},
        {"async_depth"s, 1},
        {"low_delay_brc"s, 1},
        {"low_power"s, 1},
      },
      {
        // SDR-specific options
        {"profile"s, (int) qsv::profile_av1_e::main},
      },
      {
        // HDR-specific options
        {"profile"s, (int) qsv::profile_av1_e::main},
      },
      {
        // YUV444 SDR-specific options
        {"profile"s, (int) qsv::profile_av1_e::high},
      },
      {
        // YUV444 HDR-specific options
        {"profile"s, (int) qsv::profile_av1_e::high},
      },
      {},  // Fallback options
      "av1_qsv"s,
    },
    {
      // Common options
      {
        {"preset"s, &config::video.qsv.qsv_preset},
        {"forced_idr"s, 1},
        {"async_depth"s, 1},
        {"low_delay_brc"s, 1},
        {"low_power"s, 1},
        {"recovery_point_sei"s, 0},
        {"pic_timing_sei"s, 0},
      },
      {
        // SDR-specific options
        {"profile"s, (int) qsv::profile_hevc_e::main},
      },
      {
        // HDR-specific options
        {"profile"s, (int) qsv::profile_hevc_e::main_10},
      },
      {
        // YUV444 SDR-specific options
        {"profile"s, (int) qsv::profile_hevc_e::rext},
      },
      {
        // YUV444 HDR-specific options
        {"profile"s, (int) qsv::profile_hevc_e::rext},
      },
      {
        // Fallback options
        {"low_power"s, []() {
           return config::video.qsv.qsv_slow_hevc ? 0 : 1;
         }},
      },
      "hevc_qsv"s,
    },
    {
      // Common options
      {
        {"preset"s, &config::video.qsv.qsv_preset},
        {"cavlc"s, &config::video.qsv.qsv_cavlc},
        {"forced_idr"s, 1},
        {"async_depth"s, 1},
        {"low_delay_brc"s, 1},
        {"low_power"s, 1},
        {"recovery_point_sei"s, 0},
        {"vcm"s, 1},
        {"pic_timing_sei"s, 0},
        {"max_dec_frame_buffering"s, 1},
      },
      {
        // SDR-specific options
        {"profile"s, (int) qsv::profile_h264_e::high},
      },
      {},  // HDR-specific options
      {
        // YUV444 SDR-specific options
        {"profile"s, (int) qsv::profile_h264_e::high_444p},
      },
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"low_power"s, 0},  // Some old/low-end Intel GPUs don't support low power encoding
      },
      "h264_qsv"s,
    },
    PARALLEL_ENCODING | CBR_WITH_VBR | RELAXED_COMPLIANCE | NO_RC_BUF_LIMIT | YUV444_SUPPORT
  };

  encoder_t amdvce {
    "amdvce"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      dxgi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"filler_data"s, false},
        {"forced_idr"s, 1},
        {"latency"s, "lowest_latency"s},
        {"async_depth"s, 1},
        {"skip_frame"s, 0},
        {"log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         }},
        {"preencode"s, &config::video.amd.amd_preanalysis},
        {"quality"s, &config::video.amd.amd_quality_av1},
        {"rc"s, &config::video.amd.amd_rc_av1},
        {"usage"s, &config::video.amd.amd_usage_av1},
        {"enforce_hrd"s, &config::video.amd.amd_enforce_hrd},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_amf"s,
    },
    {
      // Common options
      {
        {"filler_data"s, false},
        {"forced_idr"s, 1},
        {"latency"s, 1},
        {"async_depth"s, 1},
        {"skip_frame"s, 0},
        {"log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         }},
        {"gops_per_idr"s, 1},
        {"header_insertion_mode"s, "idr"s},
        {"preencode"s, &config::video.amd.amd_preanalysis},
        {"quality"s, &config::video.amd.amd_quality_hevc},
        {"rc"s, &config::video.amd.amd_rc_hevc},
        {"usage"s, &config::video.amd.amd_usage_hevc},
        {"vbaq"s, &config::video.amd.amd_vbaq},
        {"enforce_hrd"s, &config::video.amd.amd_enforce_hrd},
        {"level"s, [](const config_t &cfg) {
           auto size = cfg.width * cfg.height;
           // For 4K and below, try to use level 5.1 or 5.2 if possible
           if (size <= 8912896) {
             if (size * cfg.framerate <= 534773760) {
               return "5.1"s;
             } else if (size * cfg.framerate <= 1069547520) {
               return "5.2"s;
             }
           }
           return "auto"s;
         }},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_amf"s,
    },
    {
      // Common options
      {
        {"filler_data"s, false},
        {"forced_idr"s, 1},
        {"latency"s, 1},
        {"async_depth"s, 1},
        {"frame_skipping"s, 0},
        {"log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         }},
        {"preencode"s, &config::video.amd.amd_preanalysis},
        {"quality"s, &config::video.amd.amd_quality_h264},
        {"rc"s, &config::video.amd.amd_rc_h264},
        {"usage"s, &config::video.amd.amd_usage_h264},
        {"vbaq"s, &config::video.amd.amd_vbaq},
        {"enforce_hrd"s, &config::video.amd.amd_enforce_hrd},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"usage"s, 2 /* AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY */},  // Workaround for https://github.com/GPUOpen-LibrariesAndSDKs/AMF/issues/410
      },
      "h264_amf"s,
    },
    PARALLEL_ENCODING
  };

  encoder_t mediafoundation {
    "mediafoundation"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
      AV_PIX_FMT_NV12,  // SDR 4:2:0 8-bit (only format Qualcomm supports)
      AV_PIX_FMT_NONE,  // No HDR - Qualcomm MF only supports 8-bit
      AV_PIX_FMT_NONE,  // No YUV444 SDR
      AV_PIX_FMT_NONE,  // No YUV444 HDR
      dxgi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options for AV1 - Qualcomm MF encoder
      {
        {"hw_encoding"s, 1},
        {"rate_control"s, "cbr"s},
        {"scenario"s, "display_remoting"s},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_mf"s,
    },
    {
      // Common options for HEVC - Qualcomm MF encoder
      {
        {"hw_encoding"s, 1},
        {"rate_control"s, "cbr"s},
        {"scenario"s, "display_remoting"s},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_mf"s,
    },
    {
      // Common options for H.264 - Qualcomm MF encoder
      {
        {"hw_encoding"s, 1},
        {"rate_control"s, "cbr"s},
        {"scenario"s, "display_remoting"s},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_mf"s,
    },
    PARALLEL_ENCODING | FIXED_GOP_SIZE  // MF encoder doesn't support on-demand IDR frames
  };
#endif

  encoder_t software {
    "software"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_NONE,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_YUV420P,
      AV_PIX_FMT_YUV420P10,
      AV_PIX_FMT_YUV444P,
      AV_PIX_FMT_YUV444P10,
      nullptr
    ),
    {
      // libsvtav1 takes different presets than libx264/libx265.
      // We set an infinite GOP length, use a low delay prediction structure,
      // force I frames to be key frames, and set max bitrate to default to work
      // around a FFmpeg bug with CBR mode.
      {
        {"svtav1-params"s, "keyint=-1:pred-struct=1:force-key-frames=1:mbr=0"s},
        {"preset"s, &config::video.sw.svtav1_preset},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options

#ifdef ENABLE_BROKEN_AV1_ENCODER
           // Due to bugs preventing on-demand IDR frames from working and very poor
           // real-time encoding performance, we do not enable libsvtav1 by default.
           // It is only suitable for testing AV1 until the IDR frame issue is fixed.
      "libsvtav1"s,
#else
      {},
#endif
    },
    {
      // x265's Info SEI is so long that it causes the IDR picture data to be
      // kicked to the 2nd packet in the frame, breaking Moonlight's parsing logic.
      // It also looks like gop_size isn't passed on to x265, so we have to set
      // 'keyint=-1' in the parameters ourselves.
      {
        {"forced-idr"s, 1},
        {"x265-params"s, "info=0:keyint=-1"s},
        {"preset"s, &config::video.sw.sw_preset},
        {"tune"s, &config::video.sw.sw_tune},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "libx265"s,
    },
    {
      // Common options
      {
        {"preset"s, &config::video.sw.sw_preset},
        {"tune"s, &config::video.sw.sw_tune},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "libx264"s,
    },
    H264_ONLY | PARALLEL_ENCODING | ALWAYS_REPROBE | YUV444_SUPPORT
  };

#if defined(__linux__) || defined(linux) || defined(__linux) || defined(__FreeBSD__)
  encoder_t vaapi {
    "vaapi"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VAAPI,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VAAPI,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      vaapi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"async_depth"s, 1},
        {"idr_interval"s, std::numeric_limits<int>::max()},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_vaapi"s,
    },
    {
      // Common options
      {
        {"async_depth"s, 1},
        {"sei"s, 0},
        {"idr_interval"s, std::numeric_limits<int>::max()},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_vaapi"s,
    },
    {
      // Common options
      {
        {"async_depth"s, 1},
        {"sei"s, 0},
        {"idr_interval"s, std::numeric_limits<int>::max()},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_vaapi"s,
    },
    // RC buffer size will be set in platform code if supported
    LIMITED_GOP_SIZE | PARALLEL_ENCODING | NO_RC_BUF_LIMIT
  };
#endif

#ifdef __APPLE__
  encoder_t videotoolbox {
    "videotoolbox"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VIDEOTOOLBOX,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      vt_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"allow_sw"s, &config::video.vt.vt_allow_sw},
        {"require_sw"s, &config::video.vt.vt_require_sw},
        {"realtime"s, &config::video.vt.vt_realtime},
        {"prio_speed"s, 1},
        {"max_ref_frames"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_videotoolbox"s,
    },
    {
      // Common options
      {
        {"allow_sw"s, &config::video.vt.vt_allow_sw},
        {"require_sw"s, &config::video.vt.vt_require_sw},
        {"realtime"s, &config::video.vt.vt_realtime},
        {"prio_speed"s, 1},
        {"max_ref_frames"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_videotoolbox"s,
    },
    {
      // Common options
      {
        {"allow_sw"s, &config::video.vt.vt_allow_sw},
        {"require_sw"s, &config::video.vt.vt_require_sw},
        {"realtime"s, &config::video.vt.vt_realtime},
        {"prio_speed"s, 1},
        {"max_ref_frames"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"flags"s, "-low_delay"},
      },
      "h264_videotoolbox"s,
    },
    DEFAULT
  };
#endif

  static const std::vector<encoder_t *> encoders {
#ifndef __APPLE__
    &nvenc,
#endif
#ifdef _WIN32
    &quicksync,
    &amdvce,
    &mediafoundation,
#endif
#if defined(__linux__) || defined(linux) || defined(__linux) || defined(__FreeBSD__)
    &vaapi,
#endif
#ifdef __APPLE__
    &videotoolbox,
#endif
    &software
  };

  static encoder_t *chosen_encoder;
  int active_hevc_mode;
  int active_av1_mode;
  bool last_encoder_probe_supported_ref_frames_invalidation = false;
  std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec = {};

  // === 显示器重置和刷新 ===

  /**
   * @brief 重置显示设备，尝试两次初始化，失败时睡眠200ms重试
   */
  void reset_display(std::shared_ptr<platf::display_t> &disp, const platf::mem_type_e &type, const std::string &display_name, const config_t &config) {
    // We try this twice, in case we still get an error on reinitialization
    for (int x = 0; x < 2; ++x) {
      disp.reset();
      disp = platf::display(type, display_name, config);
      if (disp) {
        break;
      }

      // The capture code depends on us to sleep between failures
      std::this_thread::sleep_for(200ms);
    }
  }

  /**
   * @brief 刷新显示器名称列表，保持当前显示器索引指向同一显示器。
   * 更新流程：记住当前名称 -> 重新枚举 -> 在新列表中查找旧名称 -> 更新索引
   */
  void refresh_displays(platf::mem_type_e dev_type, std::vector<std::string> &display_names, int &current_display_index) {
    // It is possible that the output name may be empty even if it wasn't before (device disconnected) or vice-versa
    const auto output_name {display_device::map_output_name(config::video.output_name)};
    std::string current_display_name;

    // If we have a current display index, let's start with that
    if (current_display_index >= 0 && current_display_index < display_names.size()) {
      current_display_name = display_names.at(current_display_index);
    }

    // Refresh the display names
    auto old_display_names = std::move(display_names);
    display_names = platf::display_names(dev_type);

    // If we now have no displays, let's put the old display array back and fail
    if (display_names.empty() && !old_display_names.empty()) {
      BOOST_LOG(error) << "No displays were found after reenumeration!"sv;
      display_names = std::move(old_display_names);
      return;
    } else if (display_names.empty()) {
      display_names.emplace_back(output_name);
    }

    // We now have a new display name list, so reset the index back to 0
    current_display_index = 0;

    // If we had a name previously, let's try to find it in the new list
    if (!current_display_name.empty()) {
      for (int x = 0; x < display_names.size(); ++x) {
        if (display_names[x] == current_display_name) {
          current_display_index = x;
          return;
        }
      }

      // The old display was removed, so we'll start back at the first display again
      BOOST_LOG(warning) << "Previous active display ["sv << current_display_name << "] is no longer present"sv;
    } else {
      for (int x = 0; x < display_names.size(); ++x) {
        if (display_names[x] == output_name) {
          current_display_index = x;
          return;
        }
      }
    }
  }

  /**
   * @brief 异步捕获线程主循环。
   * 职责：管理显示器生命周期、图像池分配和回收、捕获帧分发给多个编码会话。
   * 支持显示器切换和重新初始化。
   */
  void captureThread(
    std::shared_ptr<safe::queue_t<capture_ctx_t>> capture_ctx_queue,
    sync_util::sync_t<std::weak_ptr<platf::display_t>> &display_wp,
    safe::signal_t &reinit_event,
    const encoder_t &encoder
  ) {
    std::vector<capture_ctx_t> capture_ctxs;

    // 失败守卫：退出时停止所有捕获上下文和编码会话
    auto fg = util::fail_guard([&]() {
      capture_ctx_queue->stop();

      // Stop all sessions listening to this thread
      for (auto &capture_ctx : capture_ctxs) {
        capture_ctx.images->stop();
      }
      for (auto &capture_ctx : capture_ctx_queue->unsafe()) {
        capture_ctx.images->stop();
      }
    });

    auto switch_display_event = mail::man->event<int>(mail::switch_display);

    // 等待第一个捕获上下文（编码会话注册）或队列停止
    auto initial_capture_ctx = capture_ctx_queue->pop();
    if (!initial_capture_ctx) {
      return;
    }
    capture_ctxs.emplace_back(std::move(*initial_capture_ctx));

    // 在此时获取显示器名称（而不是启动时），以获取最新的可用显示器列表
    std::vector<std::string> display_names;
    int display_p = -1;
    refresh_displays(encoder.platform_formats->dev_type, display_names, display_p);
    // 创建显示设备（屏幕捕获后端）
    auto disp = platf::display(encoder.platform_formats->dev_type, display_names[display_p], capture_ctxs.front().config);
    if (!disp) {
      return;
    }
    display_wp = disp;

    // === 图像缓冲池管理 ===
    constexpr auto capture_buffer_size = 12;  // 最大缓冲池大小
    std::list<std::shared_ptr<platf::img_t>> imgs(capture_buffer_size);

    // 图像使用时间戳记录，用于智能回收未使用的图像缓冲区
    std::vector<std::optional<std::chrono::steady_clock::time_point>> imgs_used_timestamps;
    const std::chrono::seconds trim_timeot = 3s;  // 未使用图像的超时回收时间
    /**
     * @brief 图像池修剪函数：根据使用历史和超时时间回收未使用的图像缓冲区
     */
    auto trim_imgs = [&]() {
      // 统计当前池中已分配和正在使用的图像数量
      size_t allocated_count = 0;
      size_t used_count = 0;
      for (const auto &img : imgs) {
        if (img) {
          allocated_count += 1;
          if (img.use_count() > 1) {
            used_count += 1;
          }
        }
      }

      // 记录当前使用数量的时间戳
      const auto now = std::chrono::steady_clock::now();
      if (imgs_used_timestamps.size() <= used_count) {
        imgs_used_timestamps.resize(used_count + 1);
      }
      imgs_used_timestamps[used_count] = now;

      // 根据最后使用时间戳和超时时间决定保留多少缓冲区
      size_t trim_target = used_count;
      for (size_t i = used_count; i < imgs_used_timestamps.size(); i++) {
        if (imgs_used_timestamps[i] && now - *imgs_used_timestamps[i] < trim_timeot) {
          trim_target = i;
        }
      }

      // 从后往前回收超出目标的未使用缓冲区（优先回收最旧的）
      if (allocated_count > trim_target) {
        size_t to_trim = allocated_count - trim_target;
                // prioritize trimming least recently used
        for (auto it = imgs.rbegin(); it != imgs.rend(); it++) {
          auto &img = *it;
          if (img && img.use_count() == 1) {
            img.reset();  // 释放图像缓冲区
            to_trim -= 1;
            if (to_trim == 0) {
              break;
            }
          }
        }
        // 清理不再相关的时间戳记录
        imgs_used_timestamps.resize(trim_target + 1);
      }
    };

    /**
     * @brief 从图像池中获取空闲图像的回调函数。
     * 优先复用已分配但未使用的图像，其次分配新图像，池满时等待。
     */
    auto pull_free_image_callback = [&](std::shared_ptr<platf::img_t> &img_out) -> bool {
      img_out.reset();
      while (capture_ctx_queue->running()) {
        // pick first allocated but unused
        for (auto it = imgs.begin(); it != imgs.end(); it++) {
          if (*it && it->use_count() == 1) {
            img_out = *it;
            if (it != imgs.begin()) {
              // move image to the front of the list to prioritize its reusal
              imgs.erase(it);
              imgs.push_front(img_out);
            }
            break;
          }
        }
        // otherwise pick first unallocated
        if (!img_out) {
          for (auto it = imgs.begin(); it != imgs.end(); it++) {
            if (!*it) {
              // allocate image
              *it = disp->alloc_img();
              img_out = *it;
              if (it != imgs.begin()) {
                // move image to the front of the list to prioritize its reusal
                imgs.erase(it);
                imgs.push_front(img_out);
              }
              break;
            }
          }
        }
        if (img_out) {
          // trim allocated but unused portion of the pool based on timeouts
        // 找到空闲图像后，修剪图像池并重置时间戳
          trim_imgs();
          img_out->frame_timestamp.reset();
          return true;
        } else {
          // 图像池已满，睡眠1ms后重试
          std::this_thread::sleep_for(1ms);
        }
      }
      return false;
    };

    // 捕获在此线程上进行，设置线程名和关键优先级
    platf::set_thread_name("video::capture");
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    // === 主捕获循环 ===
    while (capture_ctx_queue->running()) {
      bool artificial_reinit = false;

      auto push_captured_image_callback = [&](std::shared_ptr<platf::img_t> &&img, bool frame_captured) -> bool {
        KITTY_WHILE_LOOP(auto capture_ctx = std::begin(capture_ctxs), capture_ctx != std::end(capture_ctxs), {
          if (!capture_ctx->images->running()) {
            capture_ctx = capture_ctxs.erase(capture_ctx);

            continue;
          }

          if (frame_captured) {
            capture_ctx->images->raise(img);
          }

          ++capture_ctx;
        })

        if (!capture_ctx_queue->running()) {
          return false;
        }

        // 检查是否有新的捕获上下文要添加（新的编码会话）
        while (capture_ctx_queue->peek()) {
          capture_ctxs.emplace_back(std::move(*capture_ctx_queue->pop()));
        }

        // 检查是否有显示器切换请求，需要重新初始化
        if (switch_display_event->peek()) {
          artificial_reinit = true;
          return false;
        }

        return true;
      };

      auto status = disp->capture(push_captured_image_callback, pull_free_image_callback, &display_cursor);

      if (artificial_reinit && status != platf::capture_e::error) {
        status = platf::capture_e::reinit;

        artificial_reinit = false;
      }

      switch (status) {
        case platf::capture_e::reinit:
          {
            // 触发重新初始化事件，通知编码线程暂停
            reinit_event.raise(true);

            // 释放所有图像（可能持有显示设备引用）
            for (auto &img : imgs) {
              img.reset();
            }

            // 等待其他线程释放显示设备引用，确保安全重置
            while (display_wp->use_count() != 1) {
              // 释放编码器未消费的帧，避免在此线程释放以防止竞态条件
              KITTY_WHILE_LOOP(auto capture_ctx = std::begin(capture_ctxs), capture_ctx != std::end(capture_ctxs), {
                if (!capture_ctx->images->running()) {
                  capture_ctx = capture_ctxs.erase(capture_ctx);
                  continue;
                }

                while (capture_ctx->images->peek()) {
                  capture_ctx->images->pop();
                }

                ++capture_ctx;
              });

              std::this_thread::sleep_for(20ms);
            }

            while (capture_ctx_queue->running()) {
              // 释放显示设备后再重新枚举，因为某些捕获后端不支持多个会话
              disp.reset();

              // 刷新显示器列表（可能是显示器断开导致的重初始化）
              refresh_displays(encoder.platform_formats->dev_type, display_names, display_p);

              // 处理挂起的显示器切换请求
              if (switch_display_event->peek()) {
                display_p = std::clamp(*switch_display_event->pop(), 0, (int) display_names.size() - 1);
              }

              // reset_display() will sleep between retries
              reset_display(disp, encoder.platform_formats->dev_type, display_names[display_p], capture_ctxs.front().config);
              if (disp) {
                break;
              }
            }
            if (!disp) {
              return;
            }

            display_wp = disp;

            reinit_event.reset();
            continue;
          }
        case platf::capture_e::error:
        case platf::capture_e::ok:
        case platf::capture_e::timeout:
        case platf::capture_e::interrupted:
          return;
        default:
          BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
          return;
      }
    }
  }

  /**
   * @brief AVCodec编码一帧：发送帧到FFmpeg编码器，接收编码后的数据包。
   * 处理IDR帧检测、SPS/VPS注入（首帧时）、包分发。
   */
  int encode_avcodec(int64_t frame_nr, avcodec_encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    auto &frame = session.device->frame;
    frame->pts = frame_nr;  // 设置显示时间戳

    auto &ctx = session.avcodec_ctx;

    auto &sps = session.sps;  // 序列参数集
    auto &vps = session.vps;  // 视频参数集（HEVC专用）

    // 将帧发送到编码器
    auto ret = avcodec_send_frame(ctx.get(), frame);
    if (ret < 0) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Could not send a frame for encoding: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, ret);

      return -1;
    }

    // 循环接收编码后的数据包（一帧可能给多个包）
    while (ret >= 0) {
      auto packet = std::make_unique<packet_raw_avcodec>();
      auto av_packet = packet.get()->av_packet;

      ret = avcodec_receive_packet(ctx.get(), av_packet);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return 0;
      } else if (ret < 0) {
        return ret;
      }

      if (av_packet->flags & AV_PKT_FLAG_KEY) {
        BOOST_LOG(debug) << "Frame "sv << frame_nr << ": IDR Keyframe (AV_FRAME_FLAG_KEY)"sv;
      }

      if ((frame->flags & AV_FRAME_FLAG_KEY) && !(av_packet->flags & AV_PKT_FLAG_KEY)) {
        BOOST_LOG(error) << "Encoder did not produce IDR frame when requested!"sv;
      }

      if (session.inject) {
        // 首次编码时提取并替换SPS/VPS参数，确保正确的色彩空间信息
        if (session.inject == 1) {
          // H.264: 提取SPS
          auto h264 = cbs::make_sps_h264(ctx.get(), av_packet);

          sps = std::move(h264.sps);
        } else {
          auto hevc = cbs::make_sps_hevc(ctx.get(), av_packet);

          sps = std::move(hevc.sps);
          vps = std::move(hevc.vps);

          session.replacements.emplace_back(
            std::string_view((char *) std::begin(vps.old), vps.old.size()),
            std::string_view((char *) std::begin(vps._new), vps._new.size())
          );
        }

        session.inject = 0;

        session.replacements.emplace_back(
          std::string_view((char *) std::begin(sps.old), sps.old.size()),
          std::string_view((char *) std::begin(sps._new), sps._new.size())
        );
      }

      if (av_packet && av_packet->pts == frame_nr) {
        packet->frame_timestamp = frame_timestamp;
      }

      packet->replacements = &session.replacements;
      packet->channel_data = channel_data;
      packets->raise(std::move(packet));
    }

    return 0;
  }

  /**
   * @brief NVENC编码一帧：调用NVENC SDK编码帧并封装为数据包
   */
  int encode_nvenc(int64_t frame_nr, nvenc_encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    auto encoded_frame = session.encode_frame(frame_nr);
    if (encoded_frame.data.empty()) {
      BOOST_LOG(error) << "NvENC returned empty packet";
      return -1;
    }

    if (frame_nr != encoded_frame.frame_index) {
      BOOST_LOG(error) << "NvENC frame index mismatch " << frame_nr << " " << encoded_frame.frame_index;
    }

    auto packet = std::make_unique<packet_raw_generic>(std::move(encoded_frame.data), encoded_frame.frame_index, encoded_frame.idr);
    packet->channel_data = channel_data;
    packet->after_ref_frame_invalidation = encoded_frame.after_ref_frame_invalidation;
    packet->frame_timestamp = frame_timestamp;
    packets->raise(std::move(packet));

    return 0;
  }

  /**
   * @brief 编码分发函数：根据会话类型调用对应的avcodec或nvenc编码函数
   */
  int encode(int64_t frame_nr, encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    if (auto avcodec_session = dynamic_cast<avcodec_encode_session_t *>(&session)) {
      return encode_avcodec(frame_nr, *avcodec_session, packets, channel_data, frame_timestamp);
    } else if (auto nvenc_session = dynamic_cast<nvenc_encode_session_t *>(&session)) {
      return encode_nvenc(frame_nr, *nvenc_session, packets, channel_data, frame_timestamp);
    }

    return -1;
  }

  /**
   * @brief 创建AVCodec编码会话。
   * 处理流程：
   * 1. 查找编码器并验证编解码器支持
   * 2. 配置AVCodecContext（分辨率、帧率、编码配置文件、色彩空间）
   * 3. 初始化硬件帧上下文（如适用）
   * 4. 设置比特率和缓冲区大小
   * 5. 打开编码器（失败时使用回退选项重试）
   * 6. 创建帧并附加HDR元数据（如适用）
   */
  std::unique_ptr<avcodec_encode_session_t> make_avcodec_encode_session(
    platf::display_t *disp,
    const encoder_t &encoder,
    const config_t &config,
    int width,
    int height,
    std::unique_ptr<platf::avcodec_encode_device_t> encode_device
  ) {
    auto platform_formats = dynamic_cast<const encoder_platform_formats_avcodec *>(encoder.platform_formats.get());
    if (!platform_formats) {
      return nullptr;
    }

    bool hardware = platform_formats->avcodec_base_dev_type != AV_HWDEVICE_TYPE_NONE;

    auto &video_format = encoder.codec_from_config(config);
    if (!video_format[encoder_t::PASSED] || !disp->is_codec_supported(video_format.name, config)) {
      BOOST_LOG(error) << encoder.name << ": "sv << video_format.name << " mode not supported"sv;
      return nullptr;
    }

    if (config.dynamicRange && !video_format[encoder_t::DYNAMIC_RANGE]) {
      BOOST_LOG(error) << video_format.name << ": dynamic range not supported"sv;
      return nullptr;
    }

    if (config.chromaSamplingType == 1 && !video_format[encoder_t::YUV444]) {
      BOOST_LOG(error) << video_format.name << ": YUV 4:4:4 not supported"sv;
      return nullptr;
    }

    auto codec = avcodec_find_encoder_by_name(video_format.name.c_str());
    if (!codec) {
      BOOST_LOG(error) << "Couldn't open ["sv << video_format.name << ']';

      return nullptr;
    }

    // 根据色彩深度和色度采样类型选择像素格式
    auto colorspace = encode_device->colorspace;
    auto sw_fmt = (colorspace.bit_depth == 8 && config.chromaSamplingType == 0)  ? platform_formats->avcodec_pix_fmt_8bit :     // 8位 YUV420
                  (colorspace.bit_depth == 8 && config.chromaSamplingType == 1)  ? platform_formats->avcodec_pix_fmt_yuv444_8bit :  // 8位 YUV444
                  (colorspace.bit_depth == 10 && config.chromaSamplingType == 0) ? platform_formats->avcodec_pix_fmt_10bit :    // 10位 YUV420
                  (colorspace.bit_depth == 10 && config.chromaSamplingType == 1) ? platform_formats->avcodec_pix_fmt_yuv444_10bit : // 10位 YUV444
                                                                                   AV_PIX_FMT_NONE;

    // 允许一次重试以应用回退配置选项
    avcodec_ctx_t ctx;
    for (int retries = 0; retries < 2; retries++) {
      // 创建AVCodecContext并设置基本参数
      ctx.reset(avcodec_alloc_context3(codec));
      ctx->width = config.width;    // 视频宽度
      ctx->height = config.height;  // 视频高度
      ctx->time_base = AVRational {1, config.framerate};  // 时间基准
      ctx->framerate = AVRational {config.framerate, 1};  // 帧率
      // 支持小数帧率（如119.88fps）
      if (config.framerateX100 > 0) {
        AVRational fps = video::framerateX100_to_rational(config.framerateX100);
        ctx->framerate = fps;
        ctx->time_base = AVRational {fps.den, fps.num};
      }

      // 根据视频格式设置编码配置文件
      switch (config.videoFormat) {
        case 0:
          // H.264: 不支持10位编码，根据YUV444选择配置文件
          assert(!config.dynamicRange);
          ctx->profile = (config.chromaSamplingType == 1) ? AV_PROFILE_H264_HIGH_444_PREDICTIVE : AV_PROFILE_H264_HIGH;
          break;

        case 1:
          // HEVC: 8位和10位YUV444使用相同的RExt配置文件
          if (config.chromaSamplingType == 1) {
            ctx->profile = AV_PROFILE_HEVC_REXT;
          } else {
            ctx->profile = config.dynamicRange ? AV_PROFILE_HEVC_MAIN_10 : AV_PROFILE_HEVC_MAIN;
          }
          break;

        case 2:
          // AV1: Main配置支持两种位深，YUV444需要High配置
          ctx->profile = (config.chromaSamplingType == 1) ? AV_PROFILE_AV1_HIGH : AV_PROFILE_AV1_MAIN;
          break;
      }

      // B帧会延迟解码器输出，因此禁用
      ctx->max_b_frames = 0;

      // 设置GOP大小：默认无限GOP，IDR帧按需生成
      if (encoder.flags & FIXED_GOP_SIZE) {
        // 固定GOP（如Media Foundation不支持按需IDR）
        ctx->gop_size = 120;  // 约60fps下约2秒
        ctx->keyint_min = 120;
      } else {
        ctx->gop_size = encoder.flags & LIMITED_GOP_SIZE ?
                          std::numeric_limits<std::int16_t>::max() :
                          std::numeric_limits<int>::max();
        ctx->keyint_min = std::numeric_limits<int>::max();
      }

      // 部分客户端解码器有参考帧数量限制
      if (config.numRefFrames) {
        if (video_format[encoder_t::REF_FRAMES_RESTRICT]) {
          ctx->refs = config.numRefFrames;
        } else {
          BOOST_LOG(warning) << "Client requested reference frame limit, but encoder doesn't support it!"sv;
        }
      }

      // 强制重置标志位以避免AVCodecContext复用时的冲突
      ctx->flags = 0;
      ctx->flags |= AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY;  // 封闭GOP + 低延迟

      ctx->flags2 |= AV_CODEC_FLAG2_FAST;  // 允许非规范的加速技巧

      // 设置色彩空间参数（Rec.601/709/2020、Range、传输函数）
      auto avcodec_colorspace = avcodec_colorspace_from_sunshine_colorspace(colorspace);

      ctx->color_range = avcodec_colorspace.range;
      ctx->color_primaries = avcodec_colorspace.primaries;
      ctx->color_trc = avcodec_colorspace.transfer_function;
      ctx->colorspace = avcodec_colorspace.matrix;

      // SPS生成时使用的像素格式
      ctx->sw_pix_fmt = sw_fmt;

      // === 硬件编码初始化 ===
      if (hardware) {
        avcodec_buffer_t encoding_stream_context;

        ctx->pix_fmt = platform_formats->avcodec_dev_pix_fmt;

        // Create the base hwdevice context
        auto buf_or_error = platform_formats->init_avcodec_hardware_input_buffer(encode_device.get());
        if (buf_or_error.has_right()) {
          return nullptr;
        }
        encoding_stream_context = std::move(buf_or_error.left());

        // If this encoder requires derivation from the base, derive the desired type
        if (platform_formats->avcodec_derived_dev_type != AV_HWDEVICE_TYPE_NONE) {
          avcodec_buffer_t derived_context;

          // Allow the hwdevice to prepare for this type of context to be derived
          if (encode_device->prepare_to_derive_context(platform_formats->avcodec_derived_dev_type)) {
            return nullptr;
          }

          auto err = av_hwdevice_ctx_create_derived(&derived_context, platform_formats->avcodec_derived_dev_type, encoding_stream_context.get(), 0);
          if (err) {
            char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
            BOOST_LOG(error) << "Failed to derive device context: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

            return nullptr;
          }

          encoding_stream_context = std::move(derived_context);
        }

          // 初始化硬件帧上下文（GPU显存中的帧缓冲区）
          avcodec_buffer_t frame_ref {av_hwframe_ctx_alloc(encoding_stream_context.get())};

          auto frame_ctx = (AVHWFramesContext *) frame_ref->data;
          frame_ctx->format = ctx->pix_fmt;
          frame_ctx->sw_format = sw_fmt;
          frame_ctx->height = ctx->height;
          frame_ctx->width = ctx->width;
          frame_ctx->initial_pool_size = 0;

          // Allow the hwdevice to modify hwframe context parameters
          encode_device->init_hwframes(frame_ctx);

          if (auto err = av_hwframe_ctx_init(frame_ref.get()); err < 0) {
            return nullptr;
          }

          ctx->hw_frames_ctx = av_buffer_ref(frame_ref.get());
        }

        ctx->slices = config.slicesPerFrame;  // 硬件编码用客户端请求的切片数
      } else /* 软件编码 */ {
        ctx->pix_fmt = sw_fmt;

        // 保证切片数足够以充分利用CPU并行性
        ctx->slices = std::max(config.slicesPerFrame, config::video.min_threads);
      }

      if (encoder.flags & SINGLE_SLICE_ONLY) {
        ctx->slices = 1;  // 部分编码器不支持多切片
      }

      ctx->thread_type = FF_THREAD_SLICE;  // 使用切片级别的多线程
      ctx->thread_count = ctx->slices;     // 线程数 = 切片数

      // === 应用编码器特定选项 ===
      AVDictionary *options {nullptr};
      auto handle_option = [&options, &config](const encoder_t::option_t &option) {
        std::visit(
          util::overloaded {
            [&](int v) {
              av_dict_set_int(&options, option.name.c_str(), v, 0);
            },
            [&](int *v) {
              av_dict_set_int(&options, option.name.c_str(), *v, 0);
            },
            [&](std::optional<int> *v) {
              if (*v) {
                av_dict_set_int(&options, option.name.c_str(), **v, 0);
              }
            },
            [&](const std::function<int()> &v) {
              av_dict_set_int(&options, option.name.c_str(), v(), 0);
            },
            [&](const std::string &v) {
              av_dict_set(&options, option.name.c_str(), v.c_str(), 0);
            },
            [&](std::string *v) {
              if (!v->empty()) {
                av_dict_set(&options, option.name.c_str(), v->c_str(), 0);
              }
            },
            [&](const std::function<const std::string(const config_t &cfg)> &v) {
              av_dict_set(&options, option.name.c_str(), v(config).c_str(), 0);
            }
          },
          option.value
        );
      };

      // 应用通用选项，然后是特定格式的覆盖选项
      for (auto &option : video_format.common_options) {
        handle_option(option);
      }
      for (auto &option : (config.dynamicRange ? video_format.hdr_options : video_format.sdr_options)) {
        handle_option(option);
      }
      if (config.chromaSamplingType == 1) {
        for (auto &option : (config.dynamicRange ? video_format.hdr444_options : video_format.sdr444_options)) {
          handle_option(option);
        }
      }
      if (retries > 0) {
        for (auto &option : video_format.fallback_options) {
          handle_option(option);
        }
      }

      // 设置比特率：如果配置了最大比特率限制，取较小值
      auto bitrate = ((config::video.max_bitrate > 0) ? std::min(config.bitrate, config::video.max_bitrate) : config.bitrate) * 1000;
      BOOST_LOG(info) << "Streaming bitrate is " << bitrate;
      ctx->rc_max_rate = bitrate;
      ctx->bit_rate = bitrate;

      if (encoder.flags & CBR_WITH_VBR) {
        // 确保rc_max_bitrate != bit_rate以强制VBR模式
        ctx->bit_rate--;
      } else {
        ctx->rc_min_rate = bitrate;  // 使用严格的CBR模式
      }

      if (encoder.flags & RELAXED_COMPLIANCE) {
        ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
      }

      // 设置率控缓冲区大小
      if (!(encoder.flags & NO_RC_BUF_LIMIT)) {
        if (!hardware && (ctx->slices > 1 || config.videoFormat == 1)) {
          // 软件编码多切片时使用较大的缓冲区，避免libx264/libx265质量严重下降
          ctx->rc_buffer_size = bitrate / ((config.framerate * 10) / 15);
        } else {
          ctx->rc_buffer_size = bitrate / config.framerate;

#ifndef __APPLE__
          if (encoder.name == "nvenc" && config::video.nv_legacy.vbv_percentage_increase > 0) {
            ctx->rc_buffer_size += ctx->rc_buffer_size * config::video.nv_legacy.vbv_percentage_increase / 100;
          }
#endif
        }
      }

      // Allow the encoding device a final opportunity to set/unset or override any options
      encode_device->init_codec_options(ctx.get(), &options);

      if (auto status = avcodec_open2(ctx.get(), codec, &options)) {
        char err_str[AV_ERROR_MAX_STRING_SIZE] {0};

        if (!video_format.fallback_options.empty() && retries == 0) {
          BOOST_LOG(info)
            << "Retrying with fallback configuration options for ["sv << video_format.name << "] after error: "sv
            << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, status);

          continue;
        } else {
          BOOST_LOG(error)
            << "Could not open codec ["sv
            << video_format.name << "]: "sv
            << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, status);

          return nullptr;
        }
      }

      // Successfully opened the codec
      break;
    }

    avcodec_frame_t frame {av_frame_alloc()};
    frame->format = ctx->pix_fmt;
    frame->width = ctx->width;
    frame->height = ctx->height;
    frame->color_range = ctx->color_range;
    frame->color_primaries = ctx->color_primaries;
    frame->color_trc = ctx->color_trc;
    frame->colorspace = ctx->colorspace;
    frame->chroma_location = ctx->chroma_sample_location;

    // 附加HDR元数据到AVFrame
    if (colorspace_is_hdr(colorspace)) {
      SS_HDR_METADATA hdr_metadata;
      if (disp->get_hdr_metadata(hdr_metadata)) {
        auto mdm = av_mastering_display_metadata_create_side_data(frame.get());

        mdm->display_primaries[0][0] = av_make_q(hdr_metadata.displayPrimaries[0].x, 50000);
        mdm->display_primaries[0][1] = av_make_q(hdr_metadata.displayPrimaries[0].y, 50000);
        mdm->display_primaries[1][0] = av_make_q(hdr_metadata.displayPrimaries[1].x, 50000);
        mdm->display_primaries[1][1] = av_make_q(hdr_metadata.displayPrimaries[1].y, 50000);
        mdm->display_primaries[2][0] = av_make_q(hdr_metadata.displayPrimaries[2].x, 50000);
        mdm->display_primaries[2][1] = av_make_q(hdr_metadata.displayPrimaries[2].y, 50000);

        mdm->white_point[0] = av_make_q(hdr_metadata.whitePoint.x, 50000);
        mdm->white_point[1] = av_make_q(hdr_metadata.whitePoint.y, 50000);

        mdm->min_luminance = av_make_q(hdr_metadata.minDisplayLuminance, 10000);
        mdm->max_luminance = av_make_q(hdr_metadata.maxDisplayLuminance, 1);

        mdm->has_luminance = hdr_metadata.maxDisplayLuminance != 0 ? 1 : 0;
        mdm->has_primaries = hdr_metadata.displayPrimaries[0].x != 0 ? 1 : 0;

        if (hdr_metadata.maxContentLightLevel != 0 || hdr_metadata.maxFrameAverageLightLevel != 0) {
          auto clm = av_content_light_metadata_create_side_data(frame.get());

          clm->MaxCLL = hdr_metadata.maxContentLightLevel;
          clm->MaxFALL = hdr_metadata.maxFrameAverageLightLevel;
        }
      } else {
        BOOST_LOG(error) << "Couldn't get display hdr metadata when colorspace selection indicates it should have one";
      }
    }

    std::unique_ptr<platf::avcodec_encode_device_t> encode_device_final;

    if (!encode_device->data) {
      auto software_encode_device = std::make_unique<avcodec_software_encode_device_t>();

      if (software_encode_device->init(width, height, frame.get(), sw_fmt, hardware)) {
        return nullptr;
      }
      software_encode_device->colorspace = colorspace;

      encode_device_final = std::move(software_encode_device);
    } else {
      encode_device_final = std::move(encode_device);
    }

    if (encode_device_final->set_frame(frame.release(), ctx->hw_frames_ctx)) {
      return nullptr;
    }

    encode_device_final->apply_colorspace();

    auto session = std::make_unique<avcodec_encode_session_t>(
      std::move(ctx),
      std::move(encode_device_final),

      // 0 ==> don't inject, 1 ==> inject for h264, 2 ==> inject for hevc
      config.videoFormat <= 1 ? (1 - (int) video_format[encoder_t::VUI_PARAMETERS]) * (1 + config.videoFormat) : 0
    );

    return session;
  }

  std::unique_ptr<nvenc_encode_session_t> make_nvenc_encode_session(const config_t &client_config, std::unique_ptr<platf::nvenc_encode_device_t> encode_device) {
    if (!encode_device->init_encoder(client_config, encode_device->colorspace)) {
      return nullptr;
    }

    return std::make_unique<nvenc_encode_session_t>(std::move(encode_device));
  }

  std::unique_ptr<encode_session_t> make_encode_session(platf::display_t *disp, const encoder_t &encoder, const config_t &config, int width, int height, std::unique_ptr<platf::encode_device_t> encode_device) {
    if (dynamic_cast<platf::avcodec_encode_device_t *>(encode_device.get())) {
      auto avcodec_encode_device = boost::dynamic_pointer_cast<platf::avcodec_encode_device_t>(std::move(encode_device));
      return make_avcodec_encode_session(disp, encoder, config, width, height, std::move(avcodec_encode_device));
    } else if (dynamic_cast<platf::nvenc_encode_device_t *>(encode_device.get())) {
      auto nvenc_encode_device = boost::dynamic_pointer_cast<platf::nvenc_encode_device_t>(std::move(encode_device));
      return make_nvenc_encode_session(config, std::move(nvenc_encode_device));
    }

    return nullptr;
  }

  /**
   * @brief 编码主循环：持续从图像队列读取帧、转换格式、编码、发送。
   * 处理IDR帧请求、参考帧失效、最小帧率保证。
   */
  void encode_run(
    int &frame_nr,  // Store progress of the frame number
    safe::mail_t mail,
    img_event_t images,
    config_t config,
    std::shared_ptr<platf::display_t> disp,
    std::unique_ptr<platf::encode_device_t> encode_device,
    safe::signal_t &reinit_event,
    const encoder_t &encoder,
    void *channel_data
  ) {
    auto session = make_encode_session(disp.get(), encoder, config, disp->width, disp->height, std::move(encode_device));
    if (!session) {
      return;
    }

    // 为避免NVENC挂起和加速编码器重新初始化，在单独线程上完成编码器拆除
    auto fail_guard = util::fail_guard([&encoder, &session] {
      if (encoder.flags & ASYNC_TEARDOWN) {
        // 异步拆除：将耗时的清理操作移到后台线程
        std::thread encoder_teardown_thread {[session = std::move(session)]() mutable {
          BOOST_LOG(info) << "Starting async encoder teardown";
          session.reset();
          BOOST_LOG(info) << "Async encoder teardown complete";
        }};
        encoder_teardown_thread.detach();
      }
    });

    // 根据客户端目标帧率设置最大帧时间（事件驱动捕获时使用极低最小帧率）
    double def_fps_target = (disp->is_event_driven() ? 1 : config.framerate);
    double minimum_fps_target = (config::video.minimum_fps_target > 0.0) ? config::video.minimum_fps_target : def_fps_target;
    std::chrono::duration<double, std::milli> max_frametime {1000.0 / minimum_fps_target};
    BOOST_LOG(info) << "Minimum FPS target set to ~"sv << (minimum_fps_target / 2) << "fps ("sv << max_frametime.count() * 2 << "ms)"sv;

    auto shutdown_event = mail->event<bool>(mail::shutdown);
    auto packets = mail::man->queue<packet_t>(mail::video_packets);
    auto idr_events = mail->event<bool>(mail::idr);
    auto invalidate_ref_frames_events = mail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);

    {
      // 加载虚拟图像以确保等待第一帧超时时仍有内容可编码
      auto dummy_img = disp->alloc_img();
      if (!dummy_img || disp->dummy_img(dummy_img.get()) || session->convert(*dummy_img)) {
        return;
      }
    }

    while (true) {
      // 退出编码循环的条件：
      // a) 流结束  b) Sunshine退出  c) 捕获端等待重新初始化且已编码至少一帧
      if (shutdown_event->peek() || !images->running() || (reinit_event.peek() && frame_nr > 1)) {
        break;
      }

      bool requested_idr_frame = false;

      // 处理参考帧失效请求（网络丢包后的恢复机制）
      while (invalidate_ref_frames_events->peek()) {
        if (auto frames = invalidate_ref_frames_events->pop(0ms)) {
          session->invalidate_ref_frames(frames->first, frames->second);
        }
      }

      if (idr_events->peek()) {
        requested_idr_frame = true;
        idr_events->pop();
      }

      if (requested_idr_frame) {
        session->request_idr_frame();
      }

      std::optional<std::chrono::steady_clock::time_point> frame_timestamp;

      // 以最低帧率编码以避免静态内容的图像质量问题
      if (!requested_idr_frame || images->peek()) {
        if (auto img = images->pop(max_frametime)) {
          frame_timestamp = img->frame_timestamp;
          if (session->convert(*img)) {
            BOOST_LOG(error) << "Could not convert image"sv;
            return;
          }
        } else if (!images->running()) {
          break;
        }
      }

      if (encode(frame_nr++, *session, packets, channel_data, frame_timestamp)) {
        BOOST_LOG(error) << "Could not encode video packet"sv;
        return;
      }

      session->request_normal_frame();

      // 流式传输时检查鼠标是否存在，启用Mouse Keys强制光标显示（KVM场景）
      platf::enable_mouse_keys();
    }
  }

  /**
   * @brief 计算触摸端口映射：将显示器坐标转换为客户端坐标。
   * 处理宽高比计算、偏移量、缩放比例和逻辑分辨率映射。
   */
  input::touch_port_t make_port(platf::display_t *display, const config_t &config) {
    float wd = display->width;
    float hd = display->height;

    float wt = config.width;
    float ht = config.height;

    auto scalar = std::fminf(wt / wd, ht / hd);

    // we initialize scalar_tpcoords and logical dimensions to default values in case they are not set (non-KMS)
    float scalar_tpcoords = 1.0f;
    int display_env_logical_width = 0;
    int display_env_logical_height = 0;
    if (display->logical_width && display->logical_height && display->env_logical_width && display->env_logical_height) {
      float lwd = display->logical_width;
      float lhd = display->logical_height;
      scalar_tpcoords = std::fminf(wd / lwd, hd / lhd);
      display_env_logical_width = display->env_logical_width;
      display_env_logical_height = display->env_logical_height;
    }

    auto w2 = scalar * wd;
    auto h2 = scalar * hd;

    auto offsetX = (config.width - w2) * 0.5f;
    auto offsetY = (config.height - h2) * 0.5f;

    return input::touch_port_t {
      {
        display->offset_x,
        display->offset_y,
        config.width,
        config.height,
      },
      display->env_width,
      display->env_height,
      offsetX,
      offsetY,
      1.0f / scalar,
      scalar_tpcoords,
      display_env_logical_width,
      display_env_logical_height
    };
  }

  /**
   * @brief 创建编码设备：根据配置选择像素格式和色彩空间，
   * 创建AVCodec或NVENC编码设备实例。
   */
  std::unique_ptr<platf::encode_device_t> make_encode_device(platf::display_t &disp, const encoder_t &encoder, const config_t &config) {
    std::unique_ptr<platf::encode_device_t> result;

    auto colorspace = colorspace_from_client_config(config, disp.is_hdr());

    platf::pix_fmt_e pix_fmt;
    if (config.chromaSamplingType == 1) {
      // YUV 4:4:4
      if (!(encoder.flags & YUV444_SUPPORT)) {
        // Encoder can't support YUV 4:4:4 regardless of hardware capabilities
        return {};
      }
      pix_fmt = (colorspace.bit_depth == 10) ?
                  encoder.platform_formats->pix_fmt_yuv444_10bit :
                  encoder.platform_formats->pix_fmt_yuv444_8bit;
    } else {
      // YUV 4:2:0
      pix_fmt = (colorspace.bit_depth == 10) ?
                  encoder.platform_formats->pix_fmt_10bit :
                  encoder.platform_formats->pix_fmt_8bit;
    }

    {
      auto encoder_name = encoder.codec_from_config(config).name;

      BOOST_LOG(info) << "Creating encoder " << logging::bracket(encoder_name);

      auto color_coding = colorspace.colorspace == colorspace_e::bt2020    ? "HDR (Rec. 2020 + SMPTE 2084 PQ)" :
                          colorspace.colorspace == colorspace_e::rec601    ? "SDR (Rec. 601)" :
                          colorspace.colorspace == colorspace_e::rec709    ? "SDR (Rec. 709)" :
                          colorspace.colorspace == colorspace_e::bt2020sdr ? "SDR (Rec. 2020)" :
                                                                             "unknown";

      BOOST_LOG(info) << "Color coding: " << color_coding;
      BOOST_LOG(info) << "Color depth: " << colorspace.bit_depth << "-bit";
      BOOST_LOG(info) << "Color range: " << (colorspace.full_range ? "JPEG" : "MPEG");
    }

    if (dynamic_cast<const encoder_platform_formats_avcodec *>(encoder.platform_formats.get())) {
      result = disp.make_avcodec_encode_device(pix_fmt);
    } else if (dynamic_cast<const encoder_platform_formats_nvenc *>(encoder.platform_formats.get())) {
      result = disp.make_nvenc_encode_device(pix_fmt);
    }

    if (result) {
      result->colorspace = colorspace;
    }

    return result;
  }

  std::optional<sync_session_t> make_synced_session(platf::display_t *disp, const encoder_t &encoder, platf::img_t &img, sync_session_ctx_t &ctx) {
    sync_session_t encode_session;

    encode_session.ctx = &ctx;

    auto encode_device = make_encode_device(*disp, encoder, ctx.config);
    if (!encode_device) {
      return std::nullopt;
    }

    // absolute mouse coordinates require that the dimensions of the screen are known
    ctx.touch_port_events->raise(make_port(disp, ctx.config));

    // Update client with our current HDR display state
    hdr_info_t hdr_info = std::make_unique<hdr_info_raw_t>(false);
    if (colorspace_is_hdr(encode_device->colorspace)) {
      if (disp->get_hdr_metadata(hdr_info->metadata)) {
        hdr_info->enabled = true;
      } else {
        BOOST_LOG(error) << "Couldn't get display hdr metadata when colorspace selection indicates it should have one";
      }
    }
    ctx.hdr_events->raise(std::move(hdr_info));

    auto session = make_encode_session(disp, encoder, ctx.config, img.width, img.height, std::move(encode_device));
    if (!session) {
      return std::nullopt;
    }

    // Load the initial image to prepare for encoding
    if (session->convert(img)) {
      BOOST_LOG(error) << "Could not convert initial image"sv;
      return std::nullopt;
    }

    encode_session.session = std::move(session);

    return encode_session;
  }

  encode_e encode_run_sync(
    std::vector<std::unique_ptr<sync_session_ctx_t>> &synced_session_ctxs,
    encode_session_ctx_queue_t &encode_session_ctx_queue,
    std::vector<std::string> &display_names,
    int &display_p
  ) {
    const auto &encoder = *chosen_encoder;

    std::shared_ptr<platf::display_t> disp;

    auto switch_display_event = mail::man->event<int>(mail::switch_display);

    if (synced_session_ctxs.empty()) {
      auto ctx = encode_session_ctx_queue.pop();
      if (!ctx) {
        return encode_e::ok;
      }

      synced_session_ctxs.emplace_back(std::make_unique<sync_session_ctx_t>(std::move(*ctx)));
    }

    while (encode_session_ctx_queue.running()) {
      // Refresh display names since a display removal might have caused the reinitialization
      refresh_displays(encoder.platform_formats->dev_type, display_names, display_p);

      // Process any pending display switch with the new list of displays
      if (switch_display_event->peek()) {
        display_p = std::clamp(*switch_display_event->pop(), 0, (int) display_names.size() - 1);
      }

      // reset_display() will sleep between retries
      reset_display(disp, encoder.platform_formats->dev_type, display_names[display_p], synced_session_ctxs.front()->config);
      if (disp) {
        break;
      }
    }

    if (!disp) {
      return encode_e::error;
    }

    auto img = disp->alloc_img();
    if (!img || disp->dummy_img(img.get())) {
      return encode_e::error;
    }

    std::vector<sync_session_t> synced_sessions;
    for (auto &ctx : synced_session_ctxs) {
      auto synced_session = make_synced_session(disp.get(), encoder, *img, *ctx);
      if (!synced_session) {
        return encode_e::error;
      }

      synced_sessions.emplace_back(std::move(*synced_session));
    }

    auto ec = platf::capture_e::ok;
    while (encode_session_ctx_queue.running()) {
      auto push_captured_image_callback = [&](std::shared_ptr<platf::img_t> &&img, bool frame_captured) -> bool {
        while (encode_session_ctx_queue.peek()) {
          auto encode_session_ctx = encode_session_ctx_queue.pop();
          if (!encode_session_ctx) {
            return false;
          }

          synced_session_ctxs.emplace_back(std::make_unique<sync_session_ctx_t>(std::move(*encode_session_ctx)));

          auto encode_session = make_synced_session(disp.get(), encoder, *img, *synced_session_ctxs.back());
          if (!encode_session) {
            ec = platf::capture_e::error;
            return false;
          }

          synced_sessions.emplace_back(std::move(*encode_session));
        }

        KITTY_WHILE_LOOP(auto pos = std::begin(synced_sessions), pos != std::end(synced_sessions), {
          auto ctx = pos->ctx;
          if (ctx->shutdown_event->peek()) {
            // Let waiting thread know it can delete shutdown_event
            ctx->join_event->raise(true);

            pos = synced_sessions.erase(pos);
            synced_session_ctxs.erase(std::find_if(std::begin(synced_session_ctxs), std::end(synced_session_ctxs), [&ctx_p = ctx](auto &ctx) {
              return ctx.get() == ctx_p;
            }));

            if (synced_sessions.empty()) {
              return false;
            }

            continue;
          }

          if (ctx->idr_events->peek()) {
            pos->session->request_idr_frame();
            ctx->idr_events->pop();
          }

          if (frame_captured && pos->session->convert(*img)) {
            BOOST_LOG(error) << "Could not convert image"sv;
            ctx->shutdown_event->raise(true);

            continue;
          }

          std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
          if (img) {
            frame_timestamp = img->frame_timestamp;
          }

          if (encode(ctx->frame_nr++, *pos->session, ctx->packets, ctx->channel_data, frame_timestamp)) {
            BOOST_LOG(error) << "Could not encode video packet"sv;
            ctx->shutdown_event->raise(true);

            continue;
          }

          pos->session->request_normal_frame();

          ++pos;
        })

        if (switch_display_event->peek()) {
          ec = platf::capture_e::reinit;
          return false;
        }

        return true;
      };

      auto pull_free_image_callback = [&img](std::shared_ptr<platf::img_t> &img_out) -> bool {
        img_out = img;
        img_out->frame_timestamp.reset();
        return true;
      };

      auto status = disp->capture(push_captured_image_callback, pull_free_image_callback, &display_cursor);
      switch (status) {
        case platf::capture_e::reinit:
        case platf::capture_e::error:
        case platf::capture_e::ok:
        case platf::capture_e::timeout:
        case platf::capture_e::interrupted:
          return ec != platf::capture_e::ok ? ec : status;
      }
    }

    return encode_e::ok;
  }

  void captureThreadSync() {
    auto ref = capture_thread_sync.ref();

    std::vector<std::unique_ptr<sync_session_ctx_t>> synced_session_ctxs;

    auto &ctx = ref->encode_session_ctx_queue;
    auto lg = util::fail_guard([&]() {
      ctx.stop();

      for (auto &ctx : synced_session_ctxs) {
        ctx->shutdown_event->raise(true);
        ctx->join_event->raise(true);
      }

      for (auto &ctx : ctx.unsafe()) {
        ctx.shutdown_event->raise(true);
        ctx.join_event->raise(true);
      }
    });

    // Encoding and capture takes place on this thread
    platf::set_thread_name("video::capture_sync");
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    std::vector<std::string> display_names;
    int display_p = -1;
    while (encode_run_sync(synced_session_ctxs, ctx, display_names, display_p) == encode_e::reinit) {}
  }

  void capture_async(
    safe::mail_t mail,
    config_t &config,
    void *channel_data
  ) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);

    auto images = std::make_shared<img_event_t::element_type>();
    auto lg = util::fail_guard([&]() {
      images->stop();
      shutdown_event->raise(true);
    });

    auto ref = capture_thread_async.ref();
    if (!ref) {
      return;
    }

    ref->capture_ctx_queue->raise(capture_ctx_t {images, config});

    if (!ref->capture_ctx_queue->running()) {
      return;
    }

    int frame_nr = 1;

    auto touch_port_event = mail->event<input::touch_port_t>(mail::touch_port);
    auto hdr_event = mail->event<hdr_info_t>(mail::hdr);

    // Encoding takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    while (!shutdown_event->peek() && images->running()) {
      // Wait for the main capture event when the display is being reinitialized
      if (ref->reinit_event.peek()) {
        std::this_thread::sleep_for(20ms);
        continue;
      }
      // Wait for the display to be ready
      std::shared_ptr<platf::display_t> display;
      {
        auto lg = ref->display_wp.lock();
        if (ref->display_wp->expired()) {
          continue;
        }

        display = ref->display_wp->lock();
      }

      auto &encoder = *chosen_encoder;

      auto encode_device = make_encode_device(*display, encoder, config);
      if (!encode_device) {
        return;
      }

      // absolute mouse coordinates require that the dimensions of the screen are known
      touch_port_event->raise(make_port(display.get(), config));

      // Update client with our current HDR display state
      hdr_info_t hdr_info = std::make_unique<hdr_info_raw_t>(false);
      if (colorspace_is_hdr(encode_device->colorspace)) {
        if (display->get_hdr_metadata(hdr_info->metadata)) {
          hdr_info->enabled = true;
        } else {
          BOOST_LOG(error) << "Couldn't get display hdr metadata when colorspace selection indicates it should have one";
        }
      }
      hdr_event->raise(std::move(hdr_info));

      encode_run(
        frame_nr,
        mail,
        images,
        config,
        display,
        std::move(encode_device),
        ref->reinit_event,
        *ref->encoder_p,
        channel_data
      );
    }
  }

  void capture(
    safe::mail_t mail,
    config_t config,
    void *channel_data
  ) {
    auto idr_events = mail->event<bool>(mail::idr);

    idr_events->raise(true);
    if (chosen_encoder->flags & PARALLEL_ENCODING) {
      capture_async(std::move(mail), config, channel_data);
    } else {
      safe::signal_t join_event;
      auto ref = capture_thread_sync.ref();
      ref->encode_session_ctx_queue.raise(sync_session_ctx_t {
        &join_event,
        mail->event<bool>(mail::shutdown),
        mail::man->queue<packet_t>(mail::video_packets),
        std::move(idr_events),
        mail->event<hdr_info_t>(mail::hdr),
        mail->event<input::touch_port_t>(mail::touch_port),
        config,
        1,
        channel_data,
      });

      // Wait for join signal
      join_event.view();
    }
  }

  enum validate_flag_e {
    VUI_PARAMS = 0x01,  ///< VUI parameters
  };

  int validate_config(std::shared_ptr<platf::display_t> disp, const encoder_t &encoder, const config_t &config) {
    auto encode_device = make_encode_device(*disp, encoder, config);
    if (!encode_device) {
      return -1;
    }

    auto session = make_encode_session(disp.get(), encoder, config, disp->width, disp->height, std::move(encode_device));
    if (!session) {
      return -1;
    }

    {
      // Image buffers are large, so we use a separate scope to free it immediately after convert()
      auto img = disp->alloc_img();
      if (!img || disp->dummy_img(img.get()) || session->convert(*img)) {
        return -1;
      }
    }

    session->request_idr_frame();

    auto packets = mail::man->queue<packet_t>(mail::video_packets);
    while (!packets->peek()) {
      if (encode(1, *session, packets, nullptr, {})) {
        return -1;
      }
    }

    auto packet = packets->pop();
    if (!packet->is_idr()) {
      BOOST_LOG(error) << "First packet type is not an IDR frame"sv;

      return -1;
    }

    int flag = 0;

    // This check only applies for H.264 and HEVC
    if (config.videoFormat <= 1) {
      if (auto packet_avcodec = dynamic_cast<packet_raw_avcodec *>(packet.get())) {
        if (cbs::validate_sps(packet_avcodec->av_packet, config.videoFormat ? AV_CODEC_ID_H265 : AV_CODEC_ID_H264)) {
          flag |= VUI_PARAMS;
        }
      } else {
        // Don't check it for non-avcodec encoders.
        flag |= VUI_PARAMS;
      }
    }

    return flag;
  }

  /**
   * @brief 验证编码器是否可用。
   * 测试流程：
   * 1. 测试H.264基本可用性（带/不带最大参考帧限制）
   * 2. 测试HEVC支持（如已启用）
   * 3. 测试AV1支持（如已启用）
   * 4. 测试HDR和YUV444支持
   * 5. 验证VUI参数正确性
   */
  bool validate_encoder(encoder_t &encoder, bool expect_failure) {
    const auto output_name {display_device::map_output_name(config::video.output_name)};
    std::shared_ptr<platf::display_t> disp;

    BOOST_LOG(info) << "Trying encoder ["sv << encoder.name << ']';
    auto fg = util::fail_guard([&]() {
      BOOST_LOG(info) << "Encoder ["sv << encoder.name << "] failed"sv;
    });

    auto test_hevc = active_hevc_mode >= 2 || (active_hevc_mode == 0 && !(encoder.flags & H264_ONLY));
    auto test_av1 = active_av1_mode >= 2 || (active_av1_mode == 0 && !(encoder.flags & H264_ONLY));

    encoder.h264.capabilities.set();
    encoder.hevc.capabilities.set();
    encoder.av1.capabilities.set();

    // First, test encoder viability
    config_t config_max_ref_frames {1920, 1080, 60, 6000, 1000, 1, 1, 1, 0, 0, 0};
    config_t config_autoselect {1920, 1080, 60, 6000, 1000, 1, 0, 1, 0, 0, 0};

    // If the encoder isn't supported at all (not even H.264), bail early
    reset_display(disp, encoder.platform_formats->dev_type, output_name, config_autoselect);
    if (!disp) {
      return false;
    }
    if (!disp->is_codec_supported(encoder.h264.name, config_autoselect)) {
      fg.disable();
      BOOST_LOG(info) << "Encoder ["sv << encoder.name << "] is not supported on this GPU"sv;
      return false;
    }

    // If we're expecting failure, use the autoselect ref config first since that will always succeed
    // if the encoder is available.
    auto max_ref_frames_h264 = expect_failure ? -1 : validate_config(disp, encoder, config_max_ref_frames);
    auto autoselect_h264 = max_ref_frames_h264 >= 0 ? max_ref_frames_h264 : validate_config(disp, encoder, config_autoselect);
    if (autoselect_h264 < 0) {
      return false;
    } else if (expect_failure) {
      // We expected failure, but actually succeeded. Do the max_ref_frames probe we skipped.
      max_ref_frames_h264 = validate_config(disp, encoder, config_max_ref_frames);
    }

    std::vector<std::pair<validate_flag_e, encoder_t::flag_e>> packet_deficiencies {
      {VUI_PARAMS, encoder_t::VUI_PARAMETERS},
    };

    for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
      encoder.h264[encoder_flag] = (max_ref_frames_h264 & validate_flag && autoselect_h264 & validate_flag);
    }

    encoder.h264[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_h264 >= 0;
    encoder.h264[encoder_t::PASSED] = true;

    if (test_hevc) {
      config_max_ref_frames.videoFormat = 1;
      config_autoselect.videoFormat = 1;

      if (disp->is_codec_supported(encoder.hevc.name, config_autoselect)) {
        auto max_ref_frames_hevc = validate_config(disp, encoder, config_max_ref_frames);

        // If H.264 succeeded with max ref frames specified, assume that we can count on
        // HEVC to also succeed with max ref frames specified if HEVC is supported.
        auto autoselect_hevc = (max_ref_frames_hevc >= 0 || max_ref_frames_h264 >= 0) ?
                                 max_ref_frames_hevc :
                                 validate_config(disp, encoder, config_autoselect);

        for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
          encoder.hevc[encoder_flag] = (max_ref_frames_hevc & validate_flag && autoselect_hevc & validate_flag);
        }

        encoder.hevc[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_hevc >= 0;
        encoder.hevc[encoder_t::PASSED] = max_ref_frames_hevc >= 0 || autoselect_hevc >= 0;
      } else {
        BOOST_LOG(info) << "Encoder ["sv << encoder.hevc.name << "] is not supported on this GPU"sv;
        encoder.hevc.capabilities.reset();
      }
    } else {
      // Clear all cap bits for HEVC if we didn't probe it
      encoder.hevc.capabilities.reset();
    }

    if (test_av1) {
      config_max_ref_frames.videoFormat = 2;
      config_autoselect.videoFormat = 2;

      if (disp->is_codec_supported(encoder.av1.name, config_autoselect)) {
        auto max_ref_frames_av1 = validate_config(disp, encoder, config_max_ref_frames);

        // If H.264 succeeded with max ref frames specified, assume that we can count on
        // AV1 to also succeed with max ref frames specified if AV1 is supported.
        auto autoselect_av1 = (max_ref_frames_av1 >= 0 || max_ref_frames_h264 >= 0) ?
                                max_ref_frames_av1 :
                                validate_config(disp, encoder, config_autoselect);

        for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
          encoder.av1[encoder_flag] = (max_ref_frames_av1 & validate_flag && autoselect_av1 & validate_flag);
        }

        encoder.av1[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_av1 >= 0;
        encoder.av1[encoder_t::PASSED] = max_ref_frames_av1 >= 0 || autoselect_av1 >= 0;
      } else {
        BOOST_LOG(info) << "Encoder ["sv << encoder.av1.name << "] is not supported on this GPU"sv;
        encoder.av1.capabilities.reset();
      }
    } else {
      // Clear all cap bits for AV1 if we didn't probe it
      encoder.av1.capabilities.reset();
    }

    // Test HDR and YUV444 support
    {
      // H.264 is special because encoders may support YUV 4:4:4 without supporting 10-bit color depth
      if (encoder.flags & YUV444_SUPPORT) {
        config_t config_h264_yuv444 {1920, 1080, 60, 6000, 1000, 1, 0, 1, 0, 0, 1};
        encoder.h264[encoder_t::YUV444] = disp->is_codec_supported(encoder.h264.name, config_h264_yuv444) &&
                                          validate_config(disp, encoder, config_h264_yuv444) >= 0;
      } else {
        encoder.h264[encoder_t::YUV444] = false;
      }

      const config_t generic_hdr_config = {1920, 1080, 60, 6000, 1000, 1, 0, 3, 1, 1, 0};

      // Reset the display since we're switching from SDR to HDR
      reset_display(disp, encoder.platform_formats->dev_type, output_name, generic_hdr_config);
      if (!disp) {
        return false;
      }

      auto test_hdr_and_yuv444 = [&](auto &flag_map, auto video_format) {
        auto config = generic_hdr_config;
        config.videoFormat = video_format;

        if (!flag_map[encoder_t::PASSED]) {
          return;
        }

        auto encoder_codec_name = encoder.codec_from_config(config).name;

        // Test 4:4:4 HDR first. If 4:4:4 is supported, 4:2:0 should also be supported.
        config.chromaSamplingType = 1;
        if ((encoder.flags & YUV444_SUPPORT) &&
            disp->is_codec_supported(encoder_codec_name, config) &&
            validate_config(disp, encoder, config) >= 0) {
          flag_map[encoder_t::DYNAMIC_RANGE] = true;
          flag_map[encoder_t::YUV444] = true;
          return;
        } else {
          flag_map[encoder_t::YUV444] = false;
        }

        // Test 4:2:0 HDR
        config.chromaSamplingType = 0;
        if (disp->is_codec_supported(encoder_codec_name, config) &&
            validate_config(disp, encoder, config) >= 0) {
          flag_map[encoder_t::DYNAMIC_RANGE] = true;
        } else {
          flag_map[encoder_t::DYNAMIC_RANGE] = false;
        }
      };

      // HDR is not supported with H.264. Don't bother even trying it.
      encoder.h264[encoder_t::DYNAMIC_RANGE] = false;

      test_hdr_and_yuv444(encoder.hevc, 1);
      test_hdr_and_yuv444(encoder.av1, 2);
    }

    encoder.h264[encoder_t::VUI_PARAMETERS] = encoder.h264[encoder_t::VUI_PARAMETERS] && !config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE];
    encoder.hevc[encoder_t::VUI_PARAMETERS] = encoder.hevc[encoder_t::VUI_PARAMETERS] && !config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE];

    if (!encoder.h264[encoder_t::VUI_PARAMETERS]) {
      BOOST_LOG(warning) << encoder.name << ": h264 missing sps->vui parameters"sv;
    }
    if (encoder.hevc[encoder_t::PASSED] && !encoder.hevc[encoder_t::VUI_PARAMETERS]) {
      BOOST_LOG(warning) << encoder.name << ": hevc missing sps->vui parameters"sv;
    }

    fg.disable();
    return true;
  }

  /**
   * @brief 探测并选择最佳编码器。
   * 策略：
   * 1. 如果用户指定了编码器，优先使用
   * 2. 否则查找满足HEVC/AV1要求的编码器
   * 3. 最后尝试所有剩余编码器，取第一个通过验证的
   * 编码器优先级：NVENC > QSV > AMF > VAAPI > VideoToolbox > 软件
   */
  int probe_encoders() {
    if (!allow_encoder_probing()) {
      // Error already logged
      return -1;
    }

    auto encoder_list = encoders;

    // 如果已有可用编码器且不需要重新探测，直接返回
    if (chosen_encoder && !(chosen_encoder->flags & ALWAYS_REPROBE) && !platf::needs_encoder_reenumeration()) {
      return 0;
    }

    // 重置编码器选择过程
    auto previous_encoder = chosen_encoder;
    chosen_encoder = nullptr;  // 清除当前选择
    active_hevc_mode = config::video.hevc_mode;  // 重置HEVC模式
    active_av1_mode = config::video.av1_mode;    // 重置AV1模式
    last_encoder_probe_supported_ref_frames_invalidation = false;

    /**
     * @brief 调整编码器约束：如果编码器不支持指定的编解码器，降级为自动选择
     */
    auto adjust_encoder_constraints = [&](encoder_t *encoder) {
      // If we can't satisfy both the encoder and codec requirement, prefer the encoder over codec support
      if (active_hevc_mode == 3 && !encoder->hevc[encoder_t::DYNAMIC_RANGE]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC Main10 on this system"sv;
        active_hevc_mode = 0;
      } else if (active_hevc_mode == 2 && !encoder->hevc[encoder_t::PASSED]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC on this system"sv;
        active_hevc_mode = 0;
      }

      if (active_av1_mode == 3 && !encoder->av1[encoder_t::DYNAMIC_RANGE]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 Main10 on this system"sv;
        active_av1_mode = 0;
      } else if (active_av1_mode == 2 && !encoder->av1[encoder_t::PASSED]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 on this system"sv;
        active_av1_mode = 0;
      }
    };

    // 如果用户指定了编码器名称，优先初始化该编码器
    if (!config::video.encoder.empty()) {
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        if (encoder->name == config::video.encoder) {
          // Remove the encoder from the list entirely if it fails validation
          if (!validate_encoder(*encoder, previous_encoder && previous_encoder != encoder)) {
            pos = encoder_list.erase(pos);
            break;
          }

          // We will return an encoder here even if it fails one of the codec requirements specified by the user
          adjust_encoder_constraints(encoder);

          chosen_encoder = encoder;
          break;
        }

        pos++;
      });

      if (chosen_encoder == nullptr) {
        BOOST_LOG(error) << "Couldn't find any working encoder matching ["sv << config::video.encoder << ']';
      }
    }

    BOOST_LOG(info) << "// Testing for available encoders, this may generate errors. You can safely ignore those errors. //"sv;

    // If we haven't found an encoder yet, but we want one with specific codec support, search for that now.
    if (chosen_encoder == nullptr && (active_hevc_mode >= 2 || active_av1_mode >= 2)) {
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        // Remove the encoder from the list entirely if it fails validation
        if (!validate_encoder(*encoder, previous_encoder && previous_encoder != encoder)) {
          pos = encoder_list.erase(pos);
          continue;
        }

        // Skip it if it doesn't support the specified codec at all
        if ((active_hevc_mode >= 2 && !encoder->hevc[encoder_t::PASSED]) ||
            (active_av1_mode >= 2 && !encoder->av1[encoder_t::PASSED])) {
          pos++;
          continue;
        }

        // Skip it if it doesn't support HDR on the specified codec
        if ((active_hevc_mode == 3 && !encoder->hevc[encoder_t::DYNAMIC_RANGE]) ||
            (active_av1_mode == 3 && !encoder->av1[encoder_t::DYNAMIC_RANGE])) {
          pos++;
          continue;
        }

        chosen_encoder = encoder;
        break;
      });

      if (chosen_encoder == nullptr) {
        BOOST_LOG(error) << "Couldn't find any working encoder that meets HEVC/AV1 requirements"sv;
      }
    }

    // If no encoder was specified or the specified encoder was unusable, keep trying
    // the remaining encoders until we find one that passes validation.
    if (chosen_encoder == nullptr) {
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        // If we've used a previous encoder and it's not this one, we expect this encoder to
        // fail to validate. It will use a slightly different order of checks to more quickly
        // eliminate failing encoders.
        if (!validate_encoder(*encoder, previous_encoder && previous_encoder != encoder)) {
          pos = encoder_list.erase(pos);
          continue;
        }

        // We will return an encoder here even if it fails one of the codec requirements specified by the user
        adjust_encoder_constraints(encoder);

        chosen_encoder = encoder;
        break;
      });
    }

    if (chosen_encoder == nullptr) {
      const auto output_name {display_device::map_output_name(config::video.output_name)};
      BOOST_LOG(fatal) << "Unable to find display or encoder during startup."sv;
      if (!config::video.adapter_name.empty() || !output_name.empty()) {
        BOOST_LOG(fatal) << "Please ensure your manually chosen GPU and monitor are connected and powered on."sv;
      } else {
        BOOST_LOG(fatal) << "Please check that a display is connected and powered on."sv;
      }
      return -1;
    }

    BOOST_LOG(info);
    BOOST_LOG(info) << "// Ignore any errors mentioned above, they are not relevant. //"sv;
    BOOST_LOG(info);

    auto &encoder = *chosen_encoder;

    last_encoder_probe_supported_ref_frames_invalidation = (encoder.flags & REF_FRAMES_INVALIDATION);
    last_encoder_probe_supported_yuv444_for_codec[0] = encoder.h264[encoder_t::PASSED] &&
                                                       encoder.h264[encoder_t::YUV444];
    last_encoder_probe_supported_yuv444_for_codec[1] = encoder.hevc[encoder_t::PASSED] &&
                                                       encoder.hevc[encoder_t::YUV444];
    last_encoder_probe_supported_yuv444_for_codec[2] = encoder.av1[encoder_t::PASSED] &&
                                                       encoder.av1[encoder_t::YUV444];

    BOOST_LOG(debug) << "------  h264 ------"sv;
    for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
      auto flag = (encoder_t::flag_e) x;
      BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.h264[flag] ? ": supported"sv : ": unsupported"sv);
    }
    BOOST_LOG(debug) << "-------------------"sv;
    BOOST_LOG(info) << "Found H.264 encoder: "sv << encoder.h264.name << " ["sv << encoder.name << ']';

    if (encoder.hevc[encoder_t::PASSED]) {
      BOOST_LOG(debug) << "------  hevc ------"sv;
      for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
        auto flag = (encoder_t::flag_e) x;
        BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.hevc[flag] ? ": supported"sv : ": unsupported"sv);
      }
      BOOST_LOG(debug) << "-------------------"sv;

      BOOST_LOG(info) << "Found HEVC encoder: "sv << encoder.hevc.name << " ["sv << encoder.name << ']';
    }

    if (encoder.av1[encoder_t::PASSED]) {
      BOOST_LOG(debug) << "------  av1 ------"sv;
      for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
        auto flag = (encoder_t::flag_e) x;
        BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.av1[flag] ? ": supported"sv : ": unsupported"sv);
      }
      BOOST_LOG(debug) << "-------------------"sv;

      BOOST_LOG(info) << "Found AV1 encoder: "sv << encoder.av1.name << " ["sv << encoder.name << ']';
    }

    if (active_hevc_mode == 0) {
      active_hevc_mode = encoder.hevc[encoder_t::PASSED] ? (encoder.hevc[encoder_t::DYNAMIC_RANGE] ? 3 : 2) : 1;
    }

    if (active_av1_mode == 0) {
      active_av1_mode = encoder.av1[encoder_t::PASSED] ? (encoder.av1[encoder_t::DYNAMIC_RANGE] ? 3 : 2) : 1;
    }

    return 0;
  }

  // Linux only declaration
  typedef int (*vaapi_init_avcodec_hardware_input_buffer_fn)(platf::avcodec_encode_device_t *encode_device, AVBufferRef **hw_device_buf);

  util::Either<avcodec_buffer_t, int> vaapi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    // If an egl hwdevice
    if (encode_device->data) {
      if (((vaapi_init_avcodec_hardware_input_buffer_fn) encode_device->data)(encode_device, &hw_device_buf)) {
        return -1;
      }

      return hw_device_buf;
    }

    auto render_device = config::video.adapter_name.empty() ? nullptr : config::video.adapter_name.c_str();

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VAAPI, render_device, nullptr, 0);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a VAAPI device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

  util::Either<avcodec_buffer_t, int> cuda_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 1 /* AV_CUDA_USE_PRIMARY_CONTEXT */);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a CUDA device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

  util::Either<avcodec_buffer_t, int> vt_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a VideoToolbox device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

#ifdef _WIN32
}

void do_nothing(void *) {
}

namespace video {
  util::Either<avcodec_buffer_t, int> dxgi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t ctx_buf {av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA)};
    auto ctx = (AVD3D11VADeviceContext *) ((AVHWDeviceContext *) ctx_buf->data)->hwctx;

    std::fill_n((std::uint8_t *) ctx, sizeof(AVD3D11VADeviceContext), 0);

    auto device = (ID3D11Device *) encode_device->data;

    device->AddRef();
    ctx->device = device;

    ctx->lock_ctx = (void *) 1;
    ctx->lock = do_nothing;
    ctx->unlock = do_nothing;

    auto err = av_hwdevice_ctx_init(ctx_buf.get());
    if (err) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Failed to create FFMpeg hardware device context: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

      return err;
    }

    return ctx_buf;
  }
#endif

  int start_capture_async(capture_thread_async_ctx_t &capture_thread_ctx) {
    capture_thread_ctx.encoder_p = chosen_encoder;
    capture_thread_ctx.reinit_event.reset();

    capture_thread_ctx.capture_ctx_queue = std::make_shared<safe::queue_t<capture_ctx_t>>(30);

    capture_thread_ctx.capture_thread = std::thread {
      captureThread,
      capture_thread_ctx.capture_ctx_queue,
      std::ref(capture_thread_ctx.display_wp),
      std::ref(capture_thread_ctx.reinit_event),
      std::ref(*capture_thread_ctx.encoder_p)
    };

    return 0;
  }

  void end_capture_async(capture_thread_async_ctx_t &capture_thread_ctx) {
    capture_thread_ctx.capture_ctx_queue->stop();

    capture_thread_ctx.capture_thread.join();
  }

  int start_capture_sync(capture_thread_sync_ctx_t &ctx) {
    std::thread {&captureThreadSync}.detach();
    return 0;
  }

  void end_capture_sync(capture_thread_sync_ctx_t &ctx) {
  }

    // 套接字类型映射: FFmpeg硬件设备类型 -> Sunshine内存类型
  platf::mem_type_e map_base_dev_type(AVHWDeviceType type) {
    switch (type) {
      case AV_HWDEVICE_TYPE_D3D11VA:
        return platf::mem_type_e::dxgi;
      case AV_HWDEVICE_TYPE_VAAPI:
        return platf::mem_type_e::vaapi;
      case AV_HWDEVICE_TYPE_CUDA:
        return platf::mem_type_e::cuda;
      case AV_HWDEVICE_TYPE_NONE:
        return platf::mem_type_e::system;
      case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
        return platf::mem_type_e::videotoolbox;
      default:
        return platf::mem_type_e::unknown;
    }

    return platf::mem_type_e::unknown;
  }

    // 像素格式映射: FFmpeg像素格式 -> Sunshine像素格式
  platf::pix_fmt_e map_pix_fmt(AVPixelFormat fmt) {
    switch (fmt) {
      case AV_PIX_FMT_VUYX:
        return platf::pix_fmt_e::ayuv;
      case AV_PIX_FMT_XV30:
        return platf::pix_fmt_e::y410;
      case AV_PIX_FMT_YUV420P10:
        return platf::pix_fmt_e::yuv420p10;
      case AV_PIX_FMT_YUV420P:
        return platf::pix_fmt_e::yuv420p;
      case AV_PIX_FMT_NV12:
        return platf::pix_fmt_e::nv12;
      case AV_PIX_FMT_P010:
        return platf::pix_fmt_e::p010;
      default:
        return platf::pix_fmt_e::unknown;
    }

    return platf::pix_fmt_e::unknown;
  }

}  // namespace video
