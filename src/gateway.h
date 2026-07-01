/**
 * @file src/gateway.h
 * @brief 进程内端口复用网关，实现 Sunshine 单端口对外访问。
 *
 * 在 0.0.0.0:<base> 上同时监听 TCP+UDP。按首字节分流：
 *   0x16 (TLS ClientHello) → 原始 TCP 隧道转发至 127.0.0.1:<base+100> (nvhttp)
 *   0x01–0x05            → 网关协议 [id:1B][len:2B BE][payload]
 */
#pragma once

#include "platform/common.h"

namespace gateway {

  // === 流映射 (Gateway Protocol Stream Mapping) ===
  //
  // 帧格式: [Stream ID:1B] [Length(BE uint16):2B] [Payload:LengthB]
  // 协议对称: 客户端→网关 和 网关→客户端 使用相同的包头格式
  //
  // | Stream ID | 协议 | 目标地址                              | 用途                  |
  // |-----------|------|--------------------------------------|-----------------------|
  // | 0x01      | TCP  | 127.0.0.1:{base + 100}              | GameStream HTTPS      |
  // | 0x02      | UDP  | 127.0.0.1:{video_port}              | Video Stream          |
  // | 0x03      | UDP  | 127.0.0.1:{control_port}            | Control               |
  // | 0x04      | UDP  | 127.0.0.1:{audio_port}              | Audio Stream          |
  // | 0x05      | TCP  | 127.0.0.1:{rtsp_port}               | RTSP Setup            |
  // | 其他      | -    | -                                    | DROP + WARN 日志      |
  //
  // 注: HTTPS 目标使用 {base+100} 偏移而非 base，是因为 Windows 不允许
  //     两个 TCP 监听器同时绑定同一端口（即使是不同 IP 地址）。
  //     Gateway 绑定 0.0.0.0:base，nvhttp 绑定 127.0.0.1:base+100。

  constexpr std::uint8_t STREAM_HTTPS  = 0x01;
  constexpr std::uint8_t STREAM_VIDEO  = 0x02;
  constexpr std::uint8_t STREAM_CONTROL = 0x03;
  constexpr std::uint8_t STREAM_AUDIO  = 0x04;
  constexpr std::uint8_t STREAM_RTSP   = 0x05;

  // TLS ClientHello 的首字节魔数
  constexpr std::uint8_t TLS_MAGIC = 0x16;

  // 内部 HTTPS 端口偏移量（nvhttp 绑定到 base + 100）
  constexpr int INTERNAL_HTTPS_OFFSET = 100;
  constexpr int UDP_FWD_OFFSET = 200;

  /**
   * @brief 启动进程内网关。
   * @return deinit 守卫对象，析构时自动停止网关。
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> start();

  /**
   * @brief 获取第一个外部客户端的 IP 地址（串流时用于 direct UDP）。
   * @return IP 字符串，如 "101.204.17.16"；无客户端时返回空字符串。
   */
  std::string get_client_ip();

  /**
   * @brief 获取客户端 UDP 端口（由 do_udp_receive 记录的 Moonlight 真实端口）。
   * @param stream_id 流 ID（0x02 视频, 0x04 音频）
   * @return 端口号；未记录时返回 0。
   */
  std::uint16_t get_client_port(std::uint8_t stream_id);

}  // namespace gateway
