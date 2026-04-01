/**
 * @file src/nvenc/nvenc_encoded_frame.h
 * @brief NVENC编码帧声明。定义编码后的帧数据结构。
 */
#pragma once

// 标准库头文件
#include <cstdint>   // 定宽整数类型
#include <vector>    // 动态数组容器

namespace nvenc {

  /**
   * @brief 编码后的帧数据结构体。
   */
  struct nvenc_encoded_frame {
    std::vector<uint8_t> data;  ///< 编码后的帧数据（H.264/HEVC/AV1 比特流）
    uint64_t frame_index = 0;  ///< 帧索引号，唯一标识该帧
    bool idr = false;  ///< 是否为IDR帧（即时解码刷新帧，完全独立的关键帧）
    bool after_ref_frame_invalidation = false;  ///< 是否在参考帧失效(RFI)之后编码的帧
  };

}  // namespace nvenc
