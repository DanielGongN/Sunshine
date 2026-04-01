/**
 * @file src/nvenc/nvenc_base.h
 * @brief NVENC编码器平台无关抽象基类声明。提供编码器创建、销毁、帧编码和参考帧失效(RFI)功能。
 */
#pragma once

// 第三方库头文件
#include <ffnvcodec/nvEncodeAPI.h>  // NVIDIA Video Codec SDK 编码API

// 本地头文件
#include "nvenc_colorspace.h"  // NVENC色彩空间配置
#include "nvenc_config.h"  // NVENC编码器配置
#include "nvenc_encoded_frame.h"  // 编码后帧数据结构
#include "src/logging.h"  // 日志系统
#include "src/video.h"  // 视频配置

/**
 * @brief 独立NVENC编码器命名空间。
 */
namespace nvenc {

  /**
   * @brief NVENC编码器平台无关抽象基类。
   *        派生类负责实现平台特定的操作（如D3D11、CUDA等）。
   */
  class nvenc_base {
  public:
    /**
     * @param device_type 派生类使用的底层设备类型（D3D11或CUDA）。
     */
    explicit nvenc_base(NV_ENC_DEVICE_TYPE device_type);
    virtual ~nvenc_base();

    nvenc_base(const nvenc_base &) = delete;
    nvenc_base &operator=(const nvenc_base &) = delete;

    /**
     * @brief 创建编码器实例。
     * @param config NVENC编码器配置（质量、双Pass、VBV等）。
     * @param client_config 客户端请求的流配置（分辨率、帧率、码率等）。
     * @param colorspace YUV色彩空间配置。
     * @param buffer_format 平台无关的输入表面格式。
     * @return 成功返回`true`，失败返回`false`
     */
    bool create_encoder(const nvenc_config &config, const video::config_t &client_config, const nvenc_colorspace_t &colorspace, NV_ENC_BUFFER_FORMAT buffer_format);

    /**
     * @brief 销毁编码器实例。派生类在析构函数中调用。
     */
    void destroy_encoder();

    /**
     * @brief 使用平台特定的输入表面编码下一帧。
     * @param frame_index 帧索引号，唯一标识该帧，后续可用于`invalidate_ref_frames()`。
     *        首帧索引无限制，但后续帧索引必须连续递增。
     * @param force_idr 是否强制将该帧编码为IDR帧。
     * @return 编码后的帧数据。
     */
    nvenc_encoded_frame encode_frame(uint64_t frame_index, bool force_idr);

    /**
     * @brief 执行参考帧失效(RFI)操作，用于丢包恢复。
     * @param first_frame 失效范围的起始帧索引。
     * @param last_frame 失效范围的结束帧索引。
     * @return 成功返回`true`，失败返回`false`。
     *         失败后下一帧必须使用`force_idr = true`编码。
     */
    bool invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame);

  protected:
    /**
     * @brief 必须实现。用于加载NvEnc库并通过`NvEncodeAPICreateInstance()`设置`nvenc`变量。
     *        在`create_encoder()`期间调用（如果`nvenc`变量未初始化）。
     * @return 成功返回`true`，失败返回`false`
     */
    virtual bool init_library() = 0;

    /**
     * @brief 必须实现。用于创建外部输入表面，
     *        并通过`nvenc->nvEncRegisterResource()`注册该表面，设置`registered_input_buffer`变量。
     *        在`create_encoder()`期间调用。
     * @return 成功返回`true`，失败返回`false`
     */
    virtual bool create_and_register_input_buffer() = 0;

    /**
     * @brief 可选重写。如果需要在`encode_frame()`开头对注册的输入表面执行额外操作，
     *        通常用于互操作拷贝（如D3D11到CUDA的数据传输）。
     * @return 成功返回`true`，失败返回`false`
     */
    virtual bool synchronize_input_buffer() {
      return true;
    }

    /**
     * @brief 可选重写。如果要创建异步模式的编码器，必须同时设置`async_event_handle`变量。
     * @param timeout_ms 等待超时时间（毫秒）
     * @return 成功返回`true`，超时或失败返回`false`
     */
    virtual bool wait_for_async_event(uint32_t timeout_ms) {
      return false;
    }

    /**
     * @brief 检查NVENC操作是否失败，并保存错误信息。
     */
    bool nvenc_failed(NVENCSTATUS status);

    /**
     * @brief 返回与当前编解码器所需最低API版本对应的结构体版本号。
     * @details 降低结构体版本可以最大化驱动程序兼容性，避免不必要的API断裂。
     * @param version 来自`NVENCAPI_STRUCT_VERSION()`的原始结构体版本。
     * @param v11_struct_version 可选，v11 SDK主版本使用的结构体版本。
     * @param v12_struct_version 可选，v12 SDK主版本使用的结构体版本。
     * @return 适合当前编解码器的结构体版本号。
     */
    uint32_t min_struct_version(uint32_t version, uint32_t v11_struct_version = 0, uint32_t v12_struct_version = 0);

    const NV_ENC_DEVICE_TYPE device_type;  ///< 编码设备类型

    void *encoder = nullptr;  ///< NVENC编码器实例句柄

    struct {
      uint32_t width = 0;  ///< 编码宽度
      uint32_t height = 0;  ///< 编码高度
      NV_ENC_BUFFER_FORMAT buffer_format = NV_ENC_BUFFER_FORMAT_UNDEFINED;  ///< 输入缓冲区像素格式
      uint32_t ref_frames_in_dpb = 0;  ///< 解码图片缓冲区中的参考帧数量
      bool rfi = false;  ///< 是否支持参考帧失效(RFI)
    } encoder_params;  ///< 编码器参数

    std::string last_nvenc_error_string;  ///< 最后一次NVENC错误的描述字符串

    // 派生类需要设置的变量
    void *device = nullptr;  ///< 平台特定的编码设备句柄。应在构造函数或`init_library()`中设置。
    std::shared_ptr<NV_ENCODE_API_FUNCTION_LIST> nvenc;  ///< `NvEncodeAPICreateInstance()`生成的函数指针列表。应在`init_library()`中设置。
    NV_ENC_REGISTERED_PTR registered_input_buffer = nullptr;  ///< 通过`NvEncRegisterResource()`注册的平台特定输入表面。应在`create_and_register_input_buffer()`中设置。
    void *async_event_handle = nullptr;  ///< （可选）平台特定的异步事件句柄。可在构造函数或`init_library()`中设置，必须重写`wait_for_async_event()`。

  private:
    NV_ENC_OUTPUT_PTR output_bitstream = nullptr;  ///< 输出比特流缓冲区句柄
    uint32_t minimum_api_version = 0;  ///< 当前编解码器所需的最低NVENC API版本

    struct {
      uint64_t last_encoded_frame_index = 0;  ///< 最后编码的帧索引
      bool rfi_needs_confirmation = false;  ///< RFI请求是否等待确认
      std::pair<uint64_t, uint64_t> last_rfi_range;  ///< 最后一次RFI的帧范围
      logging::min_max_avg_periodic_logger<double> frame_size_logger = {debug, "NvEnc: encoded frame sizes in kB", ""};  ///< 编码帧大小统计日志记录器
    } encoder_state;  ///< 编码器运行时状态
  };

}  // namespace nvenc
