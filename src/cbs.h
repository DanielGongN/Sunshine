/**
 * @file src/cbs.h
 * @brief FFmpeg编码比特流API的声明
 * 用于解析和修改视频编码的NAL单元（序列参数集SPS、视频参数集VPS等）
 */
#pragma once

#include "utility.h"

struct AVPacket;          // FFmpeg视频数据包
 struct AVCodecContext;    // FFmpeg编解码器上下文

namespace cbs {

  /**
   * @brief NAL单元结构体（包含新旧两版本的参数集数据）
   */
  struct nal_t {
    util::buffer_t<std::uint8_t> _new; // 新的参数集数据
    util::buffer_t<std::uint8_t> old;  // 原始参数集数据
  };

  /**
   * @brief HEVC(H.265)参数集结构体
   */
  struct hevc_t {
    nal_t vps; // VPS（视频参数集）
    nal_t sps; // SPS（序列参数集）
  };

  /**
   * @brief H.264参数集结构体
   */
  struct h264_t {
    nal_t sps; // SPS（序列参数集）
  };

  hevc_t make_sps_hevc(const AVCodecContext *ctx, const AVPacket *packet); // 生成HEVC参数集
  h264_t make_sps_h264(const AVCodecContext *ctx, const AVPacket *packet); // 生成H.264参数集

  /**
   * @brief Validates the Sequence Parameter Set (SPS) of a given packet.
   * @param packet The packet to validate.
   * @param codec_id The ID of the codec used (either AV_CODEC_ID_H264 or AV_CODEC_ID_H265).
   * @return True if the SPS->VUI is present in the active SPS of the packet, false otherwise.
   */
  bool validate_sps(const AVPacket *packet, int codec_id);
}  // namespace cbs
