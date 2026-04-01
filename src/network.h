/**
 * @file src/network.h
 * @brief 网络相关函数的声明
 * 提供端口映射、地址解析、ENet网络库封装等网络工具函数
 */
#pragma once

// 标准库头文件
#include <tuple>
#include <utility>

// 第三方库头文件
#include <boost/asio.hpp> // Boost.Asio异步I/O库
#include <enet/enet.h>     // ENet可靠UDP网络库（游戏串流传输用）

// 本地项目头文件
#include "utility.h"

namespace net {
  void free_host(ENetHost *host); // ENet主机对象的自定义释放函数

  /**
   * @brief 根据基础端口计算映射端口
   * Sunshine使用基础端口+偏移的方式管理多个服务端口
   * @param port 相对于基础端口的偏移值
   * @return 计算后的实际端口号
   */
  std::uint16_t map_port(int port);

  // ENet网络对象的智能指针类型
  using host_t = util::safe_ptr<ENetHost, free_host>;     // ENet主机（自动释放）
  using peer_t = ENetPeer *;                                // ENet对等体（连接的客户端）
  using packet_t = util::safe_ptr<ENetPacket, enet_packet_destroy>; // ENet数据包

  /**
   * @brief 网络类型枚举（用于确定加密级别和访问控制）
   */
  enum net_e : int {
    PC,  ///< 本机访问
    LAN,  ///< 局域网访问
    WAN  ///< 广域网访问（互联网）
  };

  /**
   * @brief 地址族枚举（IPv4/双栈）
   */
  enum af_e : int {
    IPV4,  ///< 仅IPv4
    BOTH  ///< IPv4和IPv6双栈
  };

  net_e from_enum_string(const std::string_view &view);  // 从字符串解析网络类型
  std::string_view to_enum_string(net_e net);              // 网络类型转字符串

  net_e from_address(const std::string_view &view);        // 根据地址判断网络类型（LAN/WAN）

  host_t host_create(af_e af, ENetAddress &addr, std::uint16_t port); // 创建ENet主机（指定地址族、绑定地址和端口）

  /**
   * @brief 从配置字符串解析地址族枚举值
   */
  af_e af_from_enum_string(const std::string_view &view);

  /**
   * @brief 获取指定地址族的通配符绑定地址（如 "0.0.0.0" 或 "::" ）
   */
  std::string_view af_to_any_address_string(af_e af);

  /**
   * @brief 根据配置获取实际绑定地址（未配置则返回通配符地址）
   */
  std::string get_bind_address(af_e af);

  /**
   * @brief 地址规范化：将IPv4映射的IPv6地址转换为纯IPv4地址
   */
  boost::asio::ip::address normalize_address(boost::asio::ip::address address);

  /**
   * @brief 将地址规范化后转为字符串
   */
  std::string addr_to_normalized_string(boost::asio::ip::address address);

  /**
   * @brief 将地址规范化后转为URL安全的字符串（IPv6地址加方括号）
   */
  std::string addr_to_url_escaped_string(boost::asio::ip::address address);

  /**
   * @brief 根据远程地址获取加密模式（WAN和LAN可能使用不同的加密级别）
   */
  int encryption_mode_for_address(boost::asio::ip::address address);

  /**
   * @brief 生成mDNS服务发现的实例名称
   * @param hostname 主机名
   * @return 基于主机名的实例名，无效主机名返回"Sunshine"
   */
  std::string mdns_instance_name(const std::string_view &hostname);
}  // namespace net
