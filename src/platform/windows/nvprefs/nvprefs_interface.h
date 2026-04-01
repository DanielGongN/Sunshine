/**
 * @file src/platform/windows/nvprefs/nvprefs_interface.h
 * @brief NVIDIA偏好设置接口声明。提供从 Sunshine 调用NVIDIA驱动配置的统一接口。
 */
#pragma once

// standard includes
#include <memory>

namespace nvprefs {

  class nvprefs_interface {
  public:
    nvprefs_interface();
    ~nvprefs_interface();

    bool load();

    void unload();

    bool restore_from_and_delete_undo_file_if_exists();

    bool modify_application_profile();

    bool modify_global_profile();

    bool owning_undo_file();

    bool restore_global_profile();

  private:
    struct impl;
    std::unique_ptr<impl> pimpl;
  };

}  // namespace nvprefs
