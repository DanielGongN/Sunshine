/**
 * @file src/upnp.h
 * @brief UPnP端口映射的声明
 * 自动在路由器上映射所需端口，让外网客户端可以连接到Sunshine
 */
#pragma once

// 第三方库头文件
#include <miniupnpc/miniupnpc.h> // miniUPnP客户端库

// 本地项目头文件
#include "platform/common.h"

/**
 * @brief UPnP端口映射命名空间
 * 自动发现路由器的Internet网关设备(IGD)并配置端口转发规则
 */
namespace upnp {
  constexpr auto INET6_ADDRESS_STRLEN = 46;       // IPv6地址字符串最大长度
  constexpr auto IPv4 = 0;                         // IPv4协议标识
  constexpr auto IPv6 = 1;                         // IPv6协议标识
  constexpr auto PORT_MAPPING_LIFETIME = 3600s;   // 端口映射有效期（1小时）
  constexpr auto REFRESH_INTERVAL = 120s;          // 映射刷新间隔（2分钟）

  using device_t = util::safe_ptr<UPNPDev, freeUPNPDevlist>; // UPnP设备列表智能指针

  KITTY_USING_MOVE_T(urls_t, UPNPUrls, , {
    FreeUPNPUrls(&el);
  });

  /**
   * @brief 获取有效的Internet网关设备(IGD)状态
   * @return 0=未找到IGD, 1=已连接的有效IGD, 2=有效但未连接的IGD, 3=找到UPnP设备但不是IGD
   */
  int UPNP_GetValidIGDStatus(device_t &device, urls_t *urls, IGDdatas *data, std::array<char, INET6_ADDRESS_STRLEN> &lan_addr);

  /**
   * @brief 启动UPnP端口映射服务（在后台周期性刷新映射规则）
   * @returns RAII守卫对象，销毁时自动删除端口映射
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> start();
}  // namespace upnp
