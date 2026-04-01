/**
 * @file file_handler.h
 * @brief 文件操作工具函数的声明
 * 提供目录创建、文件读写等基础文件系统操作
 */
#pragma once

#include <string>

/**
 * @brief 文件操作工具命名空间
 */
namespace file_handler {
  /**
   * @brief 获取文件或目录的父目录路径
   * @param path 文件或目录路径
   * @return 父目录路径字符串
   */
  std::string get_parent_directory(const std::string &path);

  /**
   * @brief 创建目录（支持递归创建多级目录）
   * @param path 目录路径
   * @return true=成功, false=失败
   */
  bool make_directory(const std::string &path);

  /**
   * @brief 读取整个文件内容到字符串
   * @param path 文件路径
   * @return 文件内容字符串（文件不存在返回空字符串）
   */
  std::string read_file(const char *path);

  /**
   * @brief 将字符串内容写入文件
   * @param path 文件路径
   * @param contents 要写入的内容
   * @return 0=成功, -1=失败
   */
  int write_file(const char *path, const std::string_view &contents);
}  // namespace file_handler
