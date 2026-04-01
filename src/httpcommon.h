/**
 * @file src/httpcommon.h
 * @brief HTTP公共接口声明
 * 提供HTTP服务初始化、用户凭证管理、文件下载、URL处理等功能
 */
#pragma once

// 第三方库头文件
#include <curl/curl.h> // libcurl HTTP客户端库

// 本地项目头文件
#include "network.h"     // 网络工具
#include "thread_safe.h" // 线程安全工具

namespace http {

  int init(); // 初始化HTTP服务器（加载证书、凭证等）

  /**
   * @brief 创建并保存SSL证书和私钥文件
   */
  int create_creds(const std::string &pkey, const std::string &cert);

  /**
   * @brief 保存Web管理界面的用户名和密码（bcrypt哈希存储）
   */
  int save_user_creds(
    const std::string &file,
    const std::string &username,
    const std::string &password,
    bool run_our_mouth = false // 是否输出详细日志
  );

  int reload_user_creds(const std::string &file); // 重新加载用户凭证文件

  /**
   * @brief 下载文件（通过libcurl实现HTTPS下载）
   */
  bool download_file(const std::string &url, const std::string &file, long ssl_version = CURL_SSLVERSION_TLSv1_2);

  std::string url_escape(const std::string &url);    // URL编码转义
  std::string url_get_host(const std::string &url);  // 从URL中提取主机名

  extern std::string unique_id;               // Sunshine实例的唯一标识符
  extern net::net_e origin_web_ui_allowed;    // Web UI允许访问的网络范围（PC/LAN/WAN）

}  // namespace http
