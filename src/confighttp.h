/**
 * @file src/confighttp.h
 * @brief Web管理界面HTTP服务器的声明
 * 提供基于Web的配置管理界面，包含用户认证、CSRF保护、文件浏览等功能
 */
#pragma once

// 标准库头文件
#include <filesystem>
#include <memory>
#include <string>

// 第三方库头文件
#include <nlohmann/json.hpp>                    // JSON处理
#include <Simple-Web-Server/server_https.hpp>  // HTTPS服务器

// 本地项目头文件
#include "thread_safe.h"

#define WEB_DIR SUNSHINE_ASSETS_DIR "/web/" // Web静态资源目录

namespace confighttp {
  constexpr auto PORT_HTTPS = 1; // HTTPS端口偏移量（基础端口+1）

  // HTTPS服务器组件类型别名
  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  void start(); // 启动Web配置HTTPS服务器

  void print_req(const req_https_t &request);                                 // 打印请求日志
  void send_response(const resp_https_t &response, const nlohmann::json &output_tree); // 发送JSON响应
  void send_unauthorized(const resp_https_t &response, const req_https_t &request);     // 发送401未授权响应
  void send_redirect(const resp_https_t &response, const req_https_t &request, const char *path); // 发送重定向
  bool authenticate(const resp_https_t &response, const req_https_t &request);  // 验证用户身份（Basic Auth）
  void not_found(const resp_https_t &response, const req_https_t &request, const std::string &error_message = "Not Found"); // 404响应
  void bad_request(const resp_https_t &response, const req_https_t &request, const std::string &error_message = "Bad Request"); // 400响应
  bool check_content_type(const resp_https_t &response, const req_https_t &request, const std::string_view &contentType); // 检查Content-Type
  std::string generate_csrf_token(const std::string &client_id);  // 生成CSRF保护令牌
  bool validate_csrf_token(const resp_https_t &response, const req_https_t &request, const std::string &client_id); // 验证CSRF令牌
  std::string get_client_id(const req_https_t &request);          // 获取客户端标识
  bool check_app_index(const resp_https_t &response, const req_https_t &request, int index); // 检查应用索引有效性
  void getPage(const resp_https_t &response, const req_https_t &request, const char *html_file, bool require_auth = true, bool redirect_if_username = false); // 获取页面
  void getAsset(const resp_https_t &response, const req_https_t &request);    // 获取静态资源
  void browseDirectory(const resp_https_t &response, const req_https_t &request); // 浏览目录
  void getLocale(const resp_https_t &response, const req_https_t &request);   // 获取本地化文件
  void getCSRFToken(const resp_https_t &response, const req_https_t &request); // 获取CSRF令牌

  // Browse helper functions (also exposed for unit testing)
  /**
   * @brief 检查目录条目是否为可执行文件（用于文件浏览API）
   */
  bool is_browsable_executable(const std::filesystem::directory_entry &entry, const std::filesystem::file_status &status);

  /**
   * @brief 构建目录列表JSON数据（支持按类型过滤：目录/可执行文件/文件/全部）
   */
  nlohmann::json build_browse_entries(const std::filesystem::path &dir_path, const std::string &type_str);

#ifdef _WIN32
  /**
   * @brief 获取Windows系统可用的驱动器盘符列表（用于文件浏览API）
   */
  nlohmann::json get_windows_drives();
#endif
}  // namespace confighttp

// MIME类型映射表（文件扩展名 -> Content-Type）
const std::map<std::string, std::string> mime_types = {
  {"css", "text/css"},
  {"gif", "image/gif"},
  {"htm", "text/html"},
  {"html", "text/html"},
  {"ico", "image/x-icon"},
  {"jpeg", "image/jpeg"},
  {"jpg", "image/jpeg"},
  {"js", "application/javascript"},
  {"json", "application/json"},
  {"png", "image/png"},
  {"svg", "image/svg+xml"},
  {"ttf", "font/ttf"},
  {"txt", "text/plain"},
  {"woff2", "font/woff2"},
  {"xml", "text/xml"},
};
