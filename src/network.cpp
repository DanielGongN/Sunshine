/**
 * @file src/network.cpp
 * @brief 网络工具函数的实现
 * 包含端口映射、IP地址分类（PC/LAN/WAN）、地址规范化等
 */
// 标准库头文件
#include <algorithm>
#include <sstream>

// 本地项目头文件
#include "config.h"  // 配置管理
#include "logging.h" // 日志系统
#include "network.h" // 本文件头文件
#include "utility.h" // 工具函数

using namespace std::literals;

namespace ip = boost::asio::ip;

namespace net {
  // 本机回环地址范围（127.0.0.0/8）
  std::vector<ip::network_v4> pc_ips_v4 {
    ip::make_network_v4("127.0.0.0/8"sv),
  };
  // 局域网私有地址范围（RFC 1918 + CGNAT + 链路本地）
  std::vector<ip::network_v4> lan_ips_v4 {
    ip::make_network_v4("192.168.0.0/16"sv),  // C类私有地址
    ip::make_network_v4("172.16.0.0/12"sv),   // B类私有地址
    ip::make_network_v4("10.0.0.0/8"sv),      // A类私有地址
    ip::make_network_v4("100.64.0.0/10"sv),   // CGNAT地址（运营商级NAT）
    ip::make_network_v4("169.254.0.0/16"sv),  // 链路本地地址（无DHCP时自动分配）
  };

  std::vector<ip::network_v6> pc_ips_v6 {
    ip::make_network_v6("::1/128"sv),
  };
  std::vector<ip::network_v6> lan_ips_v6 {
    ip::make_network_v6("fc00::/7"sv),
    ip::make_network_v6("fe80::/64"sv),
  };

  /**
   * @brief 从字符串转换为网络类型枚举（wan/lan/pc）
   */
  net_e from_enum_string(const std::string_view &view) {
    if (view == "wan") {
      return WAN;
    }
    if (view == "lan") {
      return LAN;
    }

    return PC;
  }

  /**
   * @brief 从IP地址判断网络类型（本机/局域网/广域网）
   */
  net_e from_address(const std::string_view &view) {
    auto addr = normalize_address(ip::make_address(view));

    if (addr.is_v6()) {
      for (auto &range : pc_ips_v6) {
        if (range.hosts().find(addr.to_v6()) != range.hosts().end()) {
          return PC;
        }
      }

      for (auto &range : lan_ips_v6) {
        if (range.hosts().find(addr.to_v6()) != range.hosts().end()) {
          return LAN;
        }
      }
    } else {
      for (auto &range : pc_ips_v4) {
        if (range.hosts().find(addr.to_v4()) != range.hosts().end()) {
          return PC;
        }
      }

      for (auto &range : lan_ips_v4) {
        if (range.hosts().find(addr.to_v4()) != range.hosts().end()) {
          return LAN;
        }
      }
    }

    return WAN;
  }

  /**
   * @brief 将网络类型枚举转换为字符串表示
   */
  std::string_view to_enum_string(net_e net) {
    switch (net) {
      case PC:
        return "pc"sv;
      case LAN:
        return "lan"sv;
      case WAN:
        return "wan"sv;
    }

    // avoid warning
    return "wan"sv;
  }

  /**
   * @brief 从字符串解析地址族枚举（ipv4/both）
   */
  af_e af_from_enum_string(const std::string_view &view) {
    if (view == "ipv4") {
      return IPV4;
    }
    if (view == "both") {
      return BOTH;
    }

    // avoid warning
    return BOTH;
  }

  /**
   * @brief 根据地址族返回通配监听地址字符串
   */
  std::string_view af_to_any_address_string(const af_e af) {
    switch (af) {
      case IPV4:
        return "0.0.0.0"sv;
      case BOTH:
        return "::"sv;
    }

    // avoid warning
    return "::"sv;
  }

  /**
   * @brief 获取绑定地址，优先使用配置文件中的bind_address
   */
  std::string get_bind_address(const af_e af) {
    // If bind_address is configured, use it
    if (!config::sunshine.bind_address.empty()) {
      return config::sunshine.bind_address;
    }

    // Otherwise use the wildcard address for the given address family
    return std::string(af_to_any_address_string(af));
  }

  /**
   * @brief 将IPv6映射的IPv4地址转换为普通IPv4地址
   */
  boost::asio::ip::address normalize_address(boost::asio::ip::address address) {
    // Convert IPv6-mapped IPv4 addresses into regular IPv4 addresses
    if (address.is_v6()) {
      auto v6 = address.to_v6();
      if (v6.is_v4_mapped()) {
        return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6);
      }
    }

    return address;
  }

  /**
   * @brief 将IP地址规范化后转为字符串
   */
  std::string addr_to_normalized_string(boost::asio::ip::address address) {
    return normalize_address(address).to_string();
  }

  /**
   * @brief 将IP地址转换为URL安全的字符串（IPv6加方括号）
   */
  std::string addr_to_url_escaped_string(boost::asio::ip::address address) {
    address = normalize_address(address);
    if (address.is_v6()) {
      std::stringstream ss;
      ss << '[' << address.to_string() << ']';
      return ss.str();
    } else {
      return address.to_string();
    }
  }

  /**
   * @brief 根据IP地址类型（局域网/广域网）返回对应的加密模式
   */
  int encryption_mode_for_address(boost::asio::ip::address address) {
    auto nettype = net::from_address(address.to_string());
    if (nettype == net::net_e::PC || nettype == net::net_e::LAN) {
      return config::stream.lan_encryption_mode;
    } else {
      return config::stream.wan_encryption_mode;
    }
  }

  /**
   * @brief 创建ENet网络主机实例，绑定端口并启用QoS标记
   */
  host_t host_create(af_e af, ENetAddress &addr, std::uint16_t port) {
    static std::once_flag enet_init_flag;
    std::call_once(enet_init_flag, []() {
      enet_initialize();
    });

    const auto bind_addr = net::get_bind_address(af);
    enet_address_set_host(&addr, bind_addr.c_str());
    enet_address_set_port(&addr, port);

    // Maximum of 128 clients, which should be enough for anyone
    auto host = host_t {enet_host_create(af == IPV4 ? AF_INET : AF_INET6, &addr, 128, 0, 0, 0)};

    // Enable opportunistic QoS tagging (automatically disables if the network appears to drop tagged packets)
    enet_socket_set_option(host->socket, ENET_SOCKOPT_QOS, 1);

    return host;
  }

  /**
   * @brief 释放ENet主机：断开所有连接的对等点后销毁主机
   */
  void free_host(ENetHost *host) {
    std::for_each(host->peers, host->peers + host->peerCount, [](ENetPeer &peer_ref) {
      ENetPeer *peer = &peer_ref;

      if (peer) {
        enet_peer_disconnect_now(peer, 0);
      }
    });

    enet_host_destroy(host);
  }

  /**
   * @brief 根据配置基础端口计算端口映射，确保端口在有效范围内
   */
  std::uint16_t map_port(int port) {
    // calculate the port from the config port
    auto mapped_port = (std::uint16_t) ((int) config::sunshine.port + port);

    // Ensure port is in the range of 1024-65535
    if (mapped_port < 1024 || mapped_port > 65535) {
      BOOST_LOG(warning) << "Port out of range: "sv << mapped_port;
    }

    return mapped_port;
  }

  /**
   * @brief Returns a string for use as the instance name for mDNS.
   * @param hostname The hostname to use for instance name generation.
   * @return Hostname-based instance name or "Sunshine" if hostname is invalid.
   */
  std::string mdns_instance_name(const std::string_view &hostname) {
    // Start with the unmodified hostname
    std::string instancename {hostname.data(), hostname.size()};

    // Truncate to 63 characters per RFC 6763 section 7.2.
    if (instancename.size() > 63) {
      instancename.resize(63);
    }

    for (auto i = 0; i < instancename.size(); i++) {
      // Replace any spaces with dashes
      if (instancename[i] == ' ') {
        instancename[i] = '-';
      } else if (!std::isalnum(instancename[i]) && instancename[i] != '-') {
        // Stop at the first invalid character
        instancename.resize(i);
        break;
      }
    }

    return !instancename.empty() ? instancename : "Sunshine";
  }
}  // namespace net
