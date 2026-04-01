/**
 * @file src/platform/common.h
 * @brief 平台公共工具声明。定义跨平台的显示捕获、音频捕获、输入处理、
 *        编码设备、网络发送、手柄反馈等平台抽象层接口。
 */
#pragma once

// 标准库头文件
#include <bitset>       // 位集合
#include <cstdint>      // 定宽整数类型
#include <filesystem>   // 文件系统操作
#include <functional>   // 函数对象和回调
#include <mutex>        // 互斥锁
#include <string>       // 字符串

// 第三方库头文件
#include <boost/core/noncopyable.hpp>  // 不可复制基类
#ifndef _WIN32
  #include <boost/asio.hpp>         // 异步I/O（非Windows平台）
  #include <boost/process/v1.hpp>   // 进程管理（非Windows平台）
#endif

// 本地头文件
#include "src/config.h"  // 配置系统
#include "src/logging.h"  // 日志系统
#include "src/thread_safe.h"  // 线程安全队列/邮箱
#include "src/utility.h"  // 工具函数
#include "src/video_colorspace.h"  // 视频色彩空间

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
}

using namespace std::literals;

// 前向声明外部类型
struct sockaddr;        // 网络地址结构
struct AVFrame;         // FFmpeg视频帧
struct AVBufferRef;     // FFmpeg缓冲区引用
struct AVHWFramesContext;  // FFmpeg硬件帧上下文
struct AVCodecContext;  // FFmpeg编解码器上下文
struct AVDictionary;    // FFmpeg字典（键值对）

#ifdef _WIN32
// Forward declarations of boost classes to avoid having to include boost headers
// here, which results in issues with Windows.h and WinSock2.h include order.
namespace boost {
  namespace asio {
    namespace ip {
      class address;
    }  // namespace ip
  }  // namespace asio

  namespace filesystem {
    class path;
  }

  namespace process::v1 {
    class child;
    class group;
    template<typename Char>
    class basic_environment;
    typedef basic_environment<char> environment;
  }  // namespace process::v1
}  // namespace boost
#endif
namespace video {
  struct config_t;
}  // namespace video

namespace nvenc {
  class nvenc_base;
}

namespace platf {
  // 最多支持的16个手柄，受activeGamepadMask位数限制
  constexpr auto MAX_GAMEPADS = 16;

  // 手柄按键位掩码定义（每个按键对应一个位）
  constexpr std::uint32_t DPAD_UP = 0x0001;       ///< 方向键上
  constexpr std::uint32_t DPAD_DOWN = 0x0002;     ///< 方向键下
  constexpr std::uint32_t DPAD_LEFT = 0x0004;     ///< 方向键左
  constexpr std::uint32_t DPAD_RIGHT = 0x0008;    ///< 方向键右
  constexpr std::uint32_t START = 0x0010;          ///< 开始按键
  constexpr std::uint32_t BACK = 0x0020;           ///< 返回按键
  constexpr std::uint32_t LEFT_STICK = 0x0040;     ///< 左摇杆按下
  constexpr std::uint32_t RIGHT_STICK = 0x0080;    ///< 右摇杆按下
  constexpr std::uint32_t LEFT_BUTTON = 0x0100;    ///< 左肩按键(LB)
  constexpr std::uint32_t RIGHT_BUTTON = 0x0200;   ///< 右肩按键(RB)
  constexpr std::uint32_t HOME = 0x0400;           ///< 主页按键
  constexpr std::uint32_t A = 0x1000;              ///< A按键
  constexpr std::uint32_t B = 0x2000;              ///< B按键
  constexpr std::uint32_t X = 0x4000;              ///< X按键
  constexpr std::uint32_t Y = 0x8000;              ///< Y按键
  constexpr std::uint32_t PADDLE1 = 0x010000;      ///< 背部拨片1
  constexpr std::uint32_t PADDLE2 = 0x020000;      ///< 背部拨片2
  constexpr std::uint32_t PADDLE3 = 0x040000;      ///< 背部拨片3
  constexpr std::uint32_t PADDLE4 = 0x080000;      ///< 背部拨片4
  constexpr std::uint32_t TOUCHPAD_BUTTON = 0x100000;  ///< 触摸板按下
  constexpr std::uint32_t MISC_BUTTON = 0x200000;  ///< 其他按键

  /**
   * @brief 支持的手柄信息结构体。
   */
  struct supported_gamepad_t {
    std::string name;  ///< 手柄名称
    bool is_enabled;  ///< 是否启用
    std::string reason_disabled;  ///< 禁用原因
  };

  /**
   * @brief 手柄反馈类型枚举（从服务端发向客户端）。
   */
  enum class gamepad_feedback_e {
    rumble,  ///< 震动反馈
    rumble_triggers,  ///< 扩展触发器震动
    set_motion_event_state,  ///< 设置运动事件状态
    set_rgb_led,  ///< 设置RGB LED灯光
    set_adaptive_triggers,  ///< 设置自适应扩展触发器（PS5 DualSense）
  };

  struct gamepad_feedback_msg_t {
    static gamepad_feedback_msg_t make_rumble(std::uint16_t id, std::uint16_t lowfreq, std::uint16_t highfreq) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::rumble;
      msg.id = id;
      msg.data.rumble = {lowfreq, highfreq};
      return msg;
    }

    static gamepad_feedback_msg_t make_rumble_triggers(std::uint16_t id, std::uint16_t left, std::uint16_t right) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::rumble_triggers;
      msg.id = id;
      msg.data.rumble_triggers = {left, right};
      return msg;
    }

    static gamepad_feedback_msg_t make_motion_event_state(std::uint16_t id, std::uint8_t motion_type, std::uint16_t report_rate) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::set_motion_event_state;
      msg.id = id;
      msg.data.motion_event_state.motion_type = motion_type;
      msg.data.motion_event_state.report_rate = report_rate;
      return msg;
    }

    static gamepad_feedback_msg_t make_rgb_led(std::uint16_t id, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::set_rgb_led;
      msg.id = id;
      msg.data.rgb_led = {r, g, b};
      return msg;
    }

    static gamepad_feedback_msg_t make_adaptive_triggers(std::uint16_t id, uint8_t event_flags, uint8_t type_left, uint8_t type_right, const std::array<uint8_t, 10> &left, const std::array<uint8_t, 10> &right) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::set_adaptive_triggers;
      msg.id = id;
      msg.data.adaptive_triggers = {.event_flags = event_flags, .type_left = type_left, .type_right = type_right, .left = left, .right = right};
      return msg;
    }

    gamepad_feedback_e type;
    std::uint16_t id;

    union {
      struct {
        std::uint16_t lowfreq;
        std::uint16_t highfreq;
      } rumble;

      struct {
        std::uint16_t left_trigger;
        std::uint16_t right_trigger;
      } rumble_triggers;

      struct {
        std::uint16_t report_rate;
        std::uint8_t motion_type;
      } motion_event_state;

      struct {
        std::uint8_t r;
        std::uint8_t g;
        std::uint8_t b;
      } rgb_led;

      struct {
        uint16_t controllerNumber;
        uint8_t event_flags;
        uint8_t type_left;
        uint8_t type_right;
        std::array<uint8_t, 10> left;
        std::array<uint8_t, 10> right;
      } adaptive_triggers;
    } data;
  };

  using feedback_queue_t = safe::mail_raw_t::queue_t<gamepad_feedback_msg_t>;

  /**
   * @brief 扬声器通道布局命名空间。定义立体声通道映射。
   */
  namespace speaker {
    enum speaker_e {
      FRONT_LEFT,  ///< 前左
      FRONT_RIGHT,  ///< 前右
      FRONT_CENTER,  ///< 前中
      LOW_FREQUENCY,  ///< 低频/重低音
      BACK_LEFT,  ///< 后左
      BACK_RIGHT,  ///< 后右
      SIDE_LEFT,  ///< 侧左
      SIDE_RIGHT,  ///< 侧右
      MAX_SPEAKERS,  ///< 最大扬声器数
    };

    // 立体声通道布局映射——2声道
    constexpr std::uint8_t map_stereo[] {
      FRONT_LEFT,
      FRONT_RIGHT
    };
    // 立体声通道布局映射——5.1声道
    constexpr std::uint8_t map_surround51[] {
      FRONT_LEFT,
      FRONT_RIGHT,
      FRONT_CENTER,
      LOW_FREQUENCY,
      BACK_LEFT,
      BACK_RIGHT,
    };
    // 立体声通道布局映射——7.1声道
    constexpr std::uint8_t map_surround71[] {
      FRONT_LEFT,
      FRONT_RIGHT,
      FRONT_CENTER,
      LOW_FREQUENCY,
      BACK_LEFT,
      BACK_RIGHT,
      SIDE_LEFT,
      SIDE_RIGHT,
    };
  }  // namespace speaker

  /**
   * @brief 内存类型枚举（编码设备使用的内存类型）。
   */
  enum class mem_type_e {
    system,  ///< 系统内存（软件编码）
    vaapi,  ///< VA-API（Linux硬件加速）
    dxgi,  ///< DXGI（Windows Direct3D）
    cuda,  ///< CUDA（NVIDIA GPU）
    videotoolbox,  ///< VideoToolbox（macOS硬件加速）
    unknown  ///< 未知
  };

  /**
   * @brief 像素格式枚举（编码输入支持的像素格式）。
   */
  enum class pix_fmt_e {
    yuv420p,  ///< YUV 4:2:0 平面格式
    yuv420p10,  ///< YUV 4:2:0 10位平面格式
    nv12,  ///< NV12（半平面YUV 4:2:0）
    p010,  ///< P010（半平面YUV 4:2:0 10位）
    ayuv,  ///< AYUV（打包YUV 4:4:4）
    yuv444p16,  ///< 平面10位（左移到16位）YUV 4:4:4
    y410,  ///< Y410（打包YUV 4:4:4 10位）
    unknown  ///< 未知
  };

  inline std::string_view from_pix_fmt(pix_fmt_e pix_fmt) {
    using namespace std::literals;
#define _CONVERT(x) \
  case pix_fmt_e::x: \
    return #x##sv
    switch (pix_fmt) {
      _CONVERT(yuv420p);
      _CONVERT(yuv420p10);
      _CONVERT(nv12);
      _CONVERT(p010);
      _CONVERT(ayuv);
      _CONVERT(yuv444p16);
      _CONVERT(y410);
      _CONVERT(unknown);
    }
#undef _CONVERT

    return "unknown"sv;
  }

  // 触摸屏输入的尺寸/偏移信息（用于坐标转换）
  struct touch_port_t {
    int offset_x;  ///< X偏移
    int offset_y;  ///< Y偏移
    int width;  ///< 宽度
    int height;  ///< 高度
    int logical_width;  ///< 逻辑宽度
    int logical_height;  ///< 逻辑高度
  };

  // 平台能力标志（这些值必须与Limelight-internal.h的SS_FF_*常量匹配！）
  namespace platform_caps {
    typedef uint32_t caps_t;

    constexpr caps_t pen_touch = 0x01;  ///< 支持触笔和触摸事件
    constexpr caps_t controller_touch = 0x02;  ///< 支持手柄触摸事件
  };  // namespace platform_caps

  /**
   * @brief 手柄状态结构体（按键、摇杆、扩展触发器）。
   */
  struct gamepad_state_t {
    std::uint32_t buttonFlags;  ///< 按键位掩码
    std::uint8_t lt;  ///< 左扩展触发器（0-255）
    std::uint8_t rt;  ///< 右扩展触发器（0-255）
    std::int16_t lsX;  ///< 左摇杆X轴
    std::int16_t lsY;  ///< 左摇杆Y轴
    std::int16_t rsX;  ///< 右摇杆X轴
    std::int16_t rsY;  ///< 右摇杆Y轴
  };

  /**
   * @brief 手柄ID结构体。
   */
  struct gamepad_id_t {
    // 全局索引，用于在平台手柄数组中查找。在所有客户端中唯一标识手柄。
    int globalIndex;

    // 客户端相对索引，客户端报告的控制器编号。
    // 用于通过输入反馈队列回复客户端时必须使用。
    std::uint8_t clientRelativeIndex;
  };

  /**
   * @brief 手柄接入信息结构体。
   */
  struct gamepad_arrival_t {
    std::uint8_t type;  ///< 手柄类型
    std::uint16_t capabilities;  ///< 手柄能力标志
    std::uint32_t supportedButtons;  ///< 支持的按键位掩码
  };

  struct gamepad_touch_t {
    gamepad_id_t id;
    std::uint8_t eventType;
    std::uint32_t pointerId;
    float x;
    float y;
    float pressure;
  };

  struct gamepad_motion_t {
    gamepad_id_t id;
    std::uint8_t motionType;

    // Accel: m/s^2
    // Gyro: deg/s
    float x;
    float y;
    float z;
  };

  struct gamepad_battery_t {
    gamepad_id_t id;
    std::uint8_t state;
    std::uint8_t percentage;
  };

  struct touch_input_t {
    std::uint8_t eventType;
    std::uint16_t rotation;  // Degrees (0..360) or LI_ROT_UNKNOWN
    std::uint32_t pointerId;
    float x;
    float y;
    float pressureOrDistance;  // Distance for hover and pressure for contact
    float contactAreaMajor;
    float contactAreaMinor;
  };

  struct pen_input_t {
    std::uint8_t eventType;
    std::uint8_t toolType;
    std::uint8_t penButtons;
    std::uint8_t tilt;  // Degrees (0..90) or LI_TILT_UNKNOWN
    std::uint16_t rotation;  // Degrees (0..360) or LI_ROT_UNKNOWN
    float x;
    float y;
    float pressureOrDistance;  // Distance for hover and pressure for contact
    float contactAreaMajor;
    float contactAreaMinor;
  };

  /**
   * @brief 解初始化/清理的基类（RAII模式）。
   */
  class deinit_t {
  public:
    virtual ~deinit_t() = default;
  };

  /**
   * @brief 图像帧结构体（捕获的屏幕图像）。
   */
  struct img_t: std::enable_shared_from_this<img_t> {
  public:
    img_t() = default;

    img_t(img_t &&) = delete;
    img_t(const img_t &) = delete;
    img_t &operator=(img_t &&) = delete;
    img_t &operator=(const img_t &) = delete;

    std::uint8_t *data {};  ///< 原始像素数据
    std::int32_t width {};  ///< 图像宽度
    std::int32_t height {};  ///< 图像高度
    std::int32_t pixel_pitch {};  ///< 每像素字节数
    std::int32_t row_pitch {};  ///< 每行字节数

    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;  ///< 帧时间戳

    virtual ~img_t() = default;
  };

  /**
   * @brief 音频输出设备（sink）信息结构体。
   */
  struct sink_t {
    // 主机播放设备名称
    std::string host;

    // 虚拟音频设备（在macOS和Windows上无法创建虚拟 sink，因此为可选）
    struct null_t {
      std::string stereo;  ///< 立体声虚拟设备
      std::string surround51;  ///< 5.1环绕声虚拟设备
      std::string surround71;  ///< 7.1环绕声虚拟设备
    };

    std::optional<null_t> null;
  };

  /**
   * @brief 编码设备基类（将捕获的图像转换为编码器需要的格式）。
   */
  struct encode_device_t {
    virtual ~encode_device_t() = default;

    virtual int convert(platf::img_t &img) = 0;

    video::sunshine_colorspace_t colorspace;
  };

  /**
   * @brief 基于FFmpeg AVCodec的编码设备（支持硬件加速和软件编码）。
   */
  struct avcodec_encode_device_t: encode_device_t {
    void *data {};
    AVFrame *frame {};

    int convert(platf::img_t &img) override {
      return -1;
    }

    virtual void apply_colorspace() {
    }

    /**
     * @brief Set the frame to be encoded.
     * @note Implementations must take ownership of 'frame'.
     */
    virtual int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) {
      BOOST_LOG(error) << "Illegal call to hwdevice_t::set_frame(). Did you forget to override it?";
      return -1;
    };

    /**
     * @brief Initialize the hwframes context.
     * @note Implementations may set parameters during initialization of the hwframes context.
     */
    virtual void init_hwframes(AVHWFramesContext *frames) {};

    /**
     * @brief Provides a hook for allow platform-specific code to adjust codec options.
     * @note Implementations may set or modify codec options prior to codec initialization.
     */
    virtual void init_codec_options(AVCodecContext *ctx, AVDictionary **options) {};

    /**
     * @brief Prepare to derive a context.
     * @note Implementations may make modifications required before context derivation
     */
    virtual int prepare_to_derive_context(int hw_device_type) {
      return 0;
    };
  };

  /**
   * @brief 基于NVENC的编码设备（使用NVIDIA编码器）。
   */
  struct nvenc_encode_device_t: encode_device_t {
    virtual bool init_encoder(const video::config_t &client_config, const video::sunshine_colorspace_t &colorspace) = 0;

    nvenc::nvenc_base *nvenc = nullptr;
  };

  /**
   * @brief 捕获结果枚举。
   */
  enum class capture_e : int {
    ok,  ///< 成功
    reinit,  ///< 需要重新初始化
    timeout,  ///< 超时
    interrupted,  ///< 捕获被中断
    error  ///< 错误
  };

  /**
   * @brief 显示捕获抽象基类（屏幕捕获后端）。
   *        提供帧捕获、图像分配、编码设备创建、HDR支持等接口。
   */
  class display_t {
  public:
    /**
     * @brief 新图像就绪时的回调。
     * 当显示有新图像就绪或发生超时时，会调用此回调。
     * 如果捕获了帧，frame_captured为true。如果超时，则为false。
     * @retval true 成功
     * @retval false 请求中断
     */
    using push_captured_image_cb_t = std::function<bool(std::shared_ptr<img_t> &&img, bool frame_captured)>;

    /**
     * @brief 从池中获取空闲图像。
     * 调用必须同步。阻塞直到池中有空闲图像或捕获被中断。
     * @retval true 成功，img_out包含空闲图像
     * @retval false 捕获已被中断，img_out包含nullptr
     */
    using pull_free_image_cb_t = std::function<bool(std::shared_ptr<img_t> &img_out)>;

    display_t() noexcept:
        offset_x {0},
        offset_y {0} {
    }

    /**
     * @brief 捕获一帧屏幕图像。
     * @param push_captured_image_cb 捕获到图像时的回调，必须从capture()的同一线程调用。
     * @param pull_free_image_cb 后端调用此回调从池中获取空白图像。
     * 如果后端使用多线程，对此回调的调用必须同步。
     * @param cursor 指向标志的指针，指示是否应同时捕获鼠标光标。
     * @retval capture_e::ok 停止时
     * @retval capture_e::error 出错时
     * @retval capture_e::reinit 需要重新初始化时
     */
    virtual capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) = 0;

    virtual std::shared_ptr<img_t> alloc_img() = 0;

    virtual int dummy_img(img_t *img) = 0;

    virtual std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) {
      return nullptr;
    }

    virtual std::unique_ptr<nvenc_encode_device_t> make_nvenc_encode_device(pix_fmt_e pix_fmt) {
      return nullptr;
    }

    virtual bool is_hdr() {
      return false;
    }

    virtual bool get_hdr_metadata(SS_HDR_METADATA &metadata) {
      std::memset(&metadata, 0, sizeof(metadata));
      return false;
    }

    /**
     * @brief Check that a given codec is supported by the display device.
     * @param name The FFmpeg codec name (or similar for non-FFmpeg codecs).
     * @param config The codec configuration.
     * @return `true` if supported, `false` otherwise.
     */
    virtual bool is_codec_supported(std::string_view name, const ::video::config_t &config) {
      return true;
    }

    virtual bool is_event_driven() {
      return false;
    }

    virtual ~display_t() = default;

    // 流式传输时的显示尺寸和偏移信息
    int offset_x;  ///< X偏移（用于指定监视器子区域）
    int offset_y;  ///< Y偏移
    int env_width;  ///< 环境/桌面宽度
    int env_height;  ///< 环境/桌面高度
    int env_logical_width;  ///< 环境逻辑宽度
    int env_logical_height;  ///< 环境逻辑高度
    int width;  ///< 捕获宽度
    int height;  ///< 捕获高度
    int logical_width;  ///< 逻辑宽度
    int logical_height;  ///< 逻辑高度

  protected:
    // 收集捕获延迟数据（debug日志级别）
    logging::time_delta_periodic_logger sleep_overshoot_logger = {debug, "Frame capture sleep overshoot"};
  };

  /**
   * @brief 麦克风捕获抽象基类。
   */
  class mic_t {
  public:
    /**
     * @brief 采样一帧音频数据。
     */
    virtual capture_e sample(std::vector<float> &frame_buffer) = 0;

    virtual ~mic_t() = default;
  };

  /**
   * @brief 音频控制抽象基类（管理音频设备和麦克风）。
   */
  class audio_control_t {
  public:
    virtual int set_sink(const std::string &sink) = 0;

    virtual std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous, [[maybe_unused]] bool host_audio_enabled) = 0;

  /**
   * @brief 检查指定的音频设备是否在系统中可用。
   * @param sink 要检查的设备名称。
   * @returns 可用返回true，否则返回false。
   */
    virtual bool is_sink_available(const std::string &sink) = 0;

    virtual std::optional<sink_t> sink_info() = 0;

    virtual ~audio_control_t() = default;
  };

  void freeInput(void *);  ///< 释放输入上下文

  using input_t = util::safe_ptr<void, freeInput>;  ///< 平台输入上下文智能指针

  std::filesystem::path appdata();  ///< 获取应用程序数据目录路径

  std::string get_mac_address(const std::string_view &address);  ///< 获取MAC地址

  std::string from_sockaddr(const sockaddr *const);  ///< 将sockaddr转换为字符串
  std::pair<std::uint16_t, std::string> from_sockaddr_ex(const sockaddr *const);  ///< 将sockaddr转换为端口+地址

  std::unique_ptr<audio_control_t> audio_control();  ///< 创建平台特定的音频控制实例

  /**
   * @brief 获取指定硬件设备类型的显示捕获实例。
   * 如果display_name为空，使用第一个兼容的监视器。
   * @param display_name 要显示的监视器名称。
   * @param config 流配置。
   * @return 基于硬件设备类型的display_t实例。
   */
  std::shared_ptr<display_t> display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  // 获取指定内存类型支持的显示名称列表
  std::vector<std::string> display_names(mem_type_e hwdevice_type);

  /**
   * @brief 检查GPU/驱动程序自上次调用以来是否发生了变化。
   * @return 如果发生了变化或无法确定，返回`true`。
   */
  bool needs_encoder_reenumeration();

  boost::process::v1::child run_command(bool elevated, bool interactive, const std::string &cmd, boost::filesystem::path &working_dir, const boost::process::v1::environment &env, FILE *file, std::error_code &ec, boost::process::v1::group *group);

  /**
   * @brief 线程优先级枚举。
   */
  enum class thread_priority_e : int {
    low,  ///< 低优先级
    normal,  ///< 普通优先级
    high,  ///< 高优先级
    critical  ///< 关键优先级
  };
  void adjust_thread_priority(thread_priority_e priority);  ///< 调整当前线程优先级

  /**
   * @brief 为开发工具命名当前线程。
   * @note 在Linux上会截断为15个字符。
   */
  void set_thread_name(const std::string &name);

  void enable_mouse_keys();  ///< 启用鼠标键功能

  // 允许在流式传输前/后执行操作系统特定的准备/清理操作
  void streaming_will_start();  ///< 流式传输即将开始
  void streaming_will_stop();  ///< 流式传输即将停止

  void restart();  ///< 重启Sunshine服务

  /**
   * @brief Set an environment variable.
   * @param name The name of the environment variable.
   * @param value The value to set the environment variable to.
   * @return 0 on success, non-zero on failure.
   */
  int set_env(const std::string &name, const std::string &value);

  /**
   * @brief Unset an environment variable.
   * @param name The name of the environment variable.
   * @return 0 on success, non-zero on failure.
   */
  int unset_env(const std::string &name);

  struct buffer_descriptor_t {
    const char *buffer;
    size_t size;

    // Constructors required for emplace_back() prior to C++20
    buffer_descriptor_t(const char *buffer, size_t size):
        buffer(buffer),
        size(size) {
    }

    buffer_descriptor_t():
        buffer(nullptr),
        size(0) {
    }
  };

  struct batched_send_info_t {
    // Optional headers to be prepended to each packet
    const char *headers;
    size_t header_size;

    // One or more data buffers to use for the payloads
    //
    // NB: Data buffers must be aligned to payload size!
    std::vector<buffer_descriptor_t> &payload_buffers;
    size_t payload_size;

    // The offset (in header+payload message blocks) in the header and payload
    // buffers to begin sending messages from
    size_t block_offset;

    // The number of header+payload message blocks to send
    size_t block_count;

    std::uintptr_t native_socket;
    boost::asio::ip::address &target_address;
    uint16_t target_port;
    boost::asio::ip::address &source_address;

    /**
     * @brief Returns a payload buffer descriptor for the given payload offset.
     * @param offset The offset in the total payload data (bytes).
     * @return Buffer descriptor describing the region at the given offset.
     */
    buffer_descriptor_t buffer_for_payload_offset(ptrdiff_t offset) {
      for (const auto &desc : payload_buffers) {
        if (offset < desc.size) {
          return {
            desc.buffer + offset,
            desc.size - offset,
          };
        } else {
          offset -= desc.size;
        }
      }
      return {};
    }
  };

  bool send_batch(batched_send_info_t &send_info);

  struct send_info_t {
    const char *header;
    size_t header_size;
    const char *payload;
    size_t payload_size;

    std::uintptr_t native_socket;
    boost::asio::ip::address &target_address;
    uint16_t target_port;
    boost::asio::ip::address &source_address;
  };

  bool send(send_info_t &send_info);

  /**
   * @brief QoS数据类型枚举（网络服务质量标记）。
   */
  enum class qos_data_type_e : int {
    audio,  ///< 音频数据
    video  ///< 视频数据
  };

  /**
   * @brief 在给定套接字上启用QoS服务质量标记。
   * @param native_socket 原生套接字句柄。
   * @param address 目标地址。
   * @param port 目标端口。
   * @param data_type 流量类型（音频/视频）。
   * @param dscp_tagging 是否启用DSCP标记。
   */
  std::unique_ptr<deinit_t> enable_socket_qos(uintptr_t native_socket, boost::asio::ip::address &address, uint16_t port, qos_data_type_e data_type, bool dscp_tagging);

  /**
   * @brief 在默认浏览器中打开URL。
   * @param url 要打开的URL。
   */
  void open_url(const std::string &url);

  /**
   * @brief 尝试优雅地终止进程组。
   * @param native_handle 进程组的原生句柄。
   * @return 如果成功请求终止，返回`true`。
   */
  bool request_process_group_exit(std::uintptr_t native_handle);

  /**
   * @brief 检查进程组是否仍有运行中的子进程。
   * @param native_handle 进程组的原生句柄。
   * @return 如果仍有进程运行，返回`true`。
   */
  bool process_group_running(std::uintptr_t native_handle);

  input_t input();  ///< 创建平台输入上下文
  /**
   * @brief 获取当前鼠标在屏幕上的位置。
   * @param input 平台输入上下文。
   * @return 鼠标的屏幕坐标。
   */
  util::point_t get_mouse_loc(input_t &input);  ///< 获取鼠标位置
  void move_mouse(input_t &input, int deltaX, int deltaY);  ///< 移动鼠标（相对偏移）
  void abs_mouse(input_t &input, const touch_port_t &touch_port, float x, float y);  ///< 移动鼠标（绝对位置）
  void button_mouse(input_t &input, int button, bool release);  ///< 鼠标按键事件
  void scroll(input_t &input, int distance);  ///< 鼠标滚轮滨动
  void hscroll(input_t &input, int distance);  ///< 鼠标水平滚动
  void keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags);  ///< 键盘按键事件
  void gamepad_update(input_t &input, int nr, const gamepad_state_t &gamepad_state);  ///< 手柄状态更新
  void unicode(input_t &input, char *utf8, int size);  ///< Unicode字符输入

  typedef deinit_t client_input_t;

  /**
   * @brief 分配每个客户端的输入数据上下文。
   * @param input 全局输入上下文。
   * @return 指向客户端输入数据上下文的唯一指针。
   */
  std::unique_ptr<client_input_t> allocate_client_input_context(input_t &input);

  /**
   * @brief 向操作系统发送触摸事件。
   * @param input 客户端特定输入上下文。
   * @param touch_port 当前视口（用于坐标转换）。
   * @param touch 触摸事件数据。
   */
  void touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch);

  /**
   * @brief 向操作系统发送触笔事件。
   * @param input 客户端特定输入上下文。
   * @param touch_port 当前视口（用于坐标转换）。
   * @param pen 触笔事件数据。
   */
  void pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen);

  /**
   * @brief Send a gamepad touch event to the OS.
   * @param input The global input context.
   * @param touch The touch event.
   */
  void gamepad_touch(input_t &input, const gamepad_touch_t &touch);

  /**
   * @brief Send a gamepad motion event to the OS.
   * @param input The global input context.
   * @param motion The motion event.
   */
  void gamepad_motion(input_t &input, const gamepad_motion_t &motion);

  /**
   * @brief Send a gamepad battery event to the OS.
   * @param input The global input context.
   * @param battery The battery event.
   */
  void gamepad_battery(input_t &input, const gamepad_battery_t &battery);

  /**
   * @brief 创建新的虚拟手柄。
   * @param input 全局输入上下文。
   * @param id 手柄ID。
   * @param metadata 客户端提供的手柄元数据（可能为空）。
   * @param feedback_queue 用于向客户端发回消息的队列。
   * @return 成功返回0。
   */
  int alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue);
  void free_gamepad(input_t &input, int nr);

  /**
   * @brief 获取平台支持的能力标志（通告客户端）。
   * @return 能力标志。
   */
  platform_caps::caps_t get_capabilities();

  constexpr auto SERVICE_NAME = "Sunshine";  ///< mDNS服务名称
  constexpr auto SERVICE_TYPE = "_nvstream._tcp";  ///< mDNS服务类型（NVStream协议）

  /**
   * @brief mDNS服务发布命名空间。
   */
  namespace publish {
    [[nodiscard]] std::unique_ptr<deinit_t> start();  ///< 启动mDNS服务发布
  }

  [[nodiscard]] std::unique_ptr<deinit_t> init();  ///< 初始化平台子系统

  /**
   * @brief 返回当前计算机名称（UTF-8编码）。
   * @return 计算机名称，失败时返回占位符。
   */
  std::string get_host_name();

  /**
   * @brief 获取当前平台后端支持的手柄类型列表。
   * @details 可能在`platf::input()`之前被调用！
   * @param input 平台的`input_t`指针或`nullptr`。
   * @return 手柄选项和状态向量。
   */
  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input);

  /**
   * @brief 高精度定时器抽象基类（用于精确帧间隔控制）。
   */
  struct high_precision_timer: private boost::noncopyable {
    virtual ~high_precision_timer() = default;

    /**
     * @brief 休眠指定时间。
     * @param duration 休眠时长。
     */
    virtual void sleep_for(const std::chrono::nanoseconds &duration) = 0;

    /**
     * @brief 检查平台特定的定时器后端是否已成功初始化。
     * @return 成功返回`true`，失败返回`false`
     */
    virtual operator bool() = 0;
  };

  /**
   * @brief 创建平台特定的高精度定时器。
   * @return 定时器的唯一指针。
   */
  std::unique_ptr<high_precision_timer> create_high_precision_timer();

}  // namespace platf
