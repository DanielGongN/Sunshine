/**
 * @file file_handler.cpp
 * @brief 文件操作工具函数的实现
 */

// 标准库头文件
#include <filesystem> // 文件系统操作
#include <fstream>    // 文件读写流

// 本地项目头文件
#include "file_handler.h"
#include "logging.h"

namespace file_handler {
  /**
   * @brief 获取父目录路径
   * 先去除尾部的路径分隔符，然后返回父目录
   */
  std::string get_parent_directory(const std::string &path) {
    // 移除末尾的路径分隔符
    std::string trimmed_path = path;
    while (!trimmed_path.empty() && trimmed_path.back() == '/') {
      trimmed_path.pop_back();
    }

    std::filesystem::path p(trimmed_path);
    return p.parent_path().string(); // 返回父路径
  }

  /**
   * @brief 创建目录（已存在则直接返回true）
   */
  bool make_directory(const std::string &path) {
    if (std::filesystem::exists(path)) {
      return true; // 目录已存在，无需创建
    }

    return std::filesystem::create_directories(path); // 递归创建目录
  }

  /**
   * @brief 读取整个文件到字符串
   * 文件不存在时输出debug日志并返回空字符串
   */
  std::string read_file(const char *path) {
    if (!std::filesystem::exists(path)) {
      BOOST_LOG(debug) << "Missing file: " << path;
      return {};
    }

    std::ifstream in(path);
    return std::string {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()}; // 一次性读取整个文件
  }

  /**
   * @brief 将内容写入文件
   * 打开失败返回-1
   */
  int write_file(const char *path, const std::string_view &contents) {
    std::ofstream out(path);

    if (!out.is_open()) {
      return -1; // 文件打开失败
    }

    out << contents; // 写入内容

    return 0;
  }
}  // namespace file_handler
