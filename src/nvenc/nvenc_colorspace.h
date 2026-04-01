/**
 * @file src/nvenc/nvenc_colorspace.h
 * @brief NVENC YUV色彩空间声明。定义色彩原色、传递函数、矩阵系数和色彩范围。
 */
#pragma once

// 第三方库头文件
#include <ffnvcodec/nvEncodeAPI.h>  // NVIDIA Video Codec SDK 编码API

namespace nvenc {

  /**
   * @brief YUV色彩空间和色彩范围配置结构体。
   */
  struct nvenc_colorspace_t {
    NV_ENC_VUI_COLOR_PRIMARIES primaries;  ///< 色彩原色（如BT.709、BT.2020等）
    NV_ENC_VUI_TRANSFER_CHARACTERISTIC tranfer_function;  ///< 传递特性/伽马曲线（如SDR、PQ HDR等）
    NV_ENC_VUI_MATRIX_COEFFS matrix;  ///< 颜色矩阵系数（RGB到YUV的转换矩阵）
    bool full_range;  ///< 是否使用全范围色彩（0-255 vs 16-235）
  };

}  // namespace nvenc
