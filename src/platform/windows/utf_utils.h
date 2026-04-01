/**
 * @file src/platform/windows/utf_utils.h
 * @brief Windows UTF字符串转换工具声明。提供UTF-8与Wide字符串的相互转换。
 */
#pragma once

#include <string>

namespace utf_utils {
  /**
   * @brief Convert a UTF-8 string into a UTF-16 wide string.
   * @param string The UTF-8 string.
   * @return The converted UTF-16 wide string.
   */
  std::wstring from_utf8(const std::string &string);

  /**
   * @brief Convert a UTF-16 wide string into a UTF-8 string.
   * @param string The UTF-16 wide string.
   * @return The converted UTF-8 string.
   */
  std::string to_utf8(const std::wstring &string);
}  // namespace utf_utils
