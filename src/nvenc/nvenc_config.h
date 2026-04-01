/**
 * @file src/nvenc/nvenc_config.h
 * @brief NVENC编码器配置声明。定义编码质量、多Pass、VBV缓冲、自适应量化等参数。
 */
#pragma once

namespace nvenc {

  /**
   * @brief 双Pass编码模式枚举（用于更好的码率分配和运动矢量估计）。
   */
  enum class nvenc_two_pass {
    disabled,  ///< 单Pass，最快速且不需要额外显存
    quarter_resolution,  ///< 四分之一分辨率预分析，捕获更大的运动矢量，速度较快，额外显存较少
    full_resolution,  ///< 全分辨率预分析，统计更精确，速度较慢，额外显存较多
  };

  /**
   * @brief NVENC编码器配置结构体，包含所有编码参数。
   */
  struct nvenc_config {
    // 质量预设等级1~7，数值越高编码越慢但质量越好
    int quality_preset = 1;

    // 可选的预分析Pass，用于改善运动矢量、码率分配和更严格的VBV(HRD)控制，使用CUDA核心
    nvenc_two_pass two_pass = nvenc_two_pass::quarter_resolution;

    // VBV/HRD缓冲区相对于默认单帧大小的百分比增量，允许低延迟可变码率
    int vbv_percentage_increase = 0;

    // 加权预测，改善淡入淡出场景的压缩效果，使用CUDA核心
    bool weighted_prediction = false;

    // 自适应量化，为平坦区域分配更多码率（因为平坦区域视觉上更容易察觉瑕疵），使用CUDA核心
    bool adaptive_quantization = false;

    // 启用最小QP限制，限制峰值图像质量以节省码率
    bool enable_min_qp = false;

    // H.264编码的最小QP值（当enable_min_qp启用时）
    unsigned min_qp_h264 = 19;

    // HEVC编码的最小QP值（当enable_min_qp启用时）
    unsigned min_qp_hevc = 23;

    // AV1编码的最小QP值（当enable_min_qp启用时）
    unsigned min_qp_av1 = 23;

    // 在H.264中使用CAVLC熵编码代替CABAC（历史遗留选项）
    bool h264_cavlc = false;

    // 向编码帧中插入填充数据以维持目标码率（主要用于测试）
    bool insert_filler_data = false;
  };

}  // namespace nvenc
