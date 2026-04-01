/**
 * @file src/nvenc/nvenc_utils.h
 * @brief NVENC工具函数声明。提供像素格式转换和色彩空间转换工具。
 */
#pragma once

// 平台特定头文件
#ifdef _WIN32
  #include <dxgiformat.h>  // DXGI像素格式定义
#endif

// 第三方库头文件
#include <ffnvcodec/nvEncodeAPI.h>  // NVIDIA编码API

// 本地头文件
#include "nvenc_colorspace.h"  // NVENC色彩空间结构体
#include "src/platform/common.h"  // 平台像素格式枚举
#include "src/video_colorspace.h"  // Sunshine色彩空间定义

namespace nvenc {

#ifdef _WIN32
  /**
   * @brief 将NVENC缓冲区格式转换为DXGI像素格式（用于D3D11表面创建）。
   */
  DXGI_FORMAT dxgi_format_from_nvenc_format(NV_ENC_BUFFER_FORMAT format);
#endif

  /**
   * @brief 将Sunshine平台像素格式转换为NVENC缓冲区格式。
   */
  NV_ENC_BUFFER_FORMAT nvenc_format_from_sunshine_format(platf::pix_fmt_e format);

  /**
   * @brief 将Sunshine色彩空间转换为NVENC色彩空间配置。
   */
  nvenc_colorspace_t nvenc_colorspace_from_sunshine_colorspace(const video::sunshine_colorspace_t &sunshine_colorspace);

}  // namespace nvenc
