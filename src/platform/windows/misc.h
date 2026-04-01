/**
 * @file src/platform/windows/misc.h
 * @brief Windows平台杂项声明。包括服务管理、文件系统、安全、网络等工具函数。
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

// platform includes
#include <Windows.h>
#include <winnt.h>

namespace platf {
  void print_status(const std::string_view &prefix, HRESULT status);
  HDESK syncThreadDesktop();

  int64_t qpc_counter();

  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Get file version information from a Windows executable or driver file.
   * @param file_path Path to the file to query.
   * @param version_str Output parameter for version string in format "major.minor.build.revision".
   * @return true if version info was successfully extracted, false otherwise.
   */
  bool getFileVersionInfo(const std::filesystem::path &file_path, std::string &version_str);
}  // namespace platf
