/**
 * @file src/platform/windows/utf_utils.cpp
 * @brief Windows UTF字符串转换工具实现。使用Win32 API进行UTF-8和UTF-16的转换。
 */
#include "utf_utils.h"

#include "src/logging.h"

#include <string>
#include <Windows.h>

using namespace std::literals;

namespace utf_utils {
  /**
   * @brief 将UTF-8字符串转换为UTF-16宽字符串（使用Win32 MultiByteToWideChar API）
   */
  std::wstring from_utf8(const std::string &string) {
    // 空字符串无需转换
    if (string.empty()) {
      return {};
    }

    // 第一次调用：传入输出缓冲区大小为0，获取所需的宽字符数量
    // MB_ERR_INVALID_CHARS 标志在遇到无效UTF-8序列时返回错误而非静默替换
    auto output_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, string.data(), string.size(), nullptr, 0);
    if (output_size == 0) {
      auto winerr = GetLastError();
      BOOST_LOG(error) << "Failed to get UTF-16 buffer size: "sv << winerr;
      return {};
    }

    // 第二次调用：分配足够空间后执行实际的UTF-8→UTF-16转换
    std::wstring output(output_size, L'\0');
    output_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, string.data(), string.size(), output.data(), output.size());
    if (output_size == 0) {
      auto winerr = GetLastError();
      BOOST_LOG(error) << "Failed to convert string to UTF-16: "sv << winerr;
      return {};
    }

    return output;
  }

  /**
   * @brief 将UTF-16宽字符串转换为UTF-8字符串（使用Win32 WideCharToMultiByte API）
   */
  std::string to_utf8(const std::wstring &string) {
    // 空字符串无需转换
    if (string.empty()) {
      return {};
    }

    // 第一次调用：传入输出缓冲区大小为0，获取所需的UTF-8字节数
    // WC_ERR_INVALID_CHARS 标志在遇到无效UTF-16代理对时返回错误
    auto output_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, string.data(), string.size(), nullptr, 0, nullptr, nullptr);
    if (output_size == 0) {
      auto winerr = GetLastError();
      BOOST_LOG(error) << "Failed to get UTF-8 buffer size: "sv << winerr;
      return {};
    }

    // 第二次调用：分配足够空间后执行实际的UTF-16→UTF-8转换
    std::string output(output_size, '\0');
    output_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, string.data(), string.size(), output.data(), output.size(), nullptr, nullptr);
    if (output_size == 0) {
      auto winerr = GetLastError();
      BOOST_LOG(error) << "Failed to convert string to UTF-8: "sv << winerr;
      return {};
    }

    return output;
  }
}  // namespace utf_utils
