/**
 * @file src/input.h
 * @brief 游戏手柄、键盘和鼠标输入处理的声明
 * 接收Moonlight客户端的输入事件，转换为本地操作系统输入
 */
#pragma once

#include <functional>

// 本地项目头文件
#include "platform/common.h" // 平台公共接口
#include "thread_safe.h"     // 线程安全工具

namespace input {
  struct input_t; // 输入上下文前向声明

  void print(void *input);            // 打印输入信息（调试用）
  void reset(std::shared_ptr<input_t> &input); // 重置输入状态
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data); // 传递输入数据包

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init(); // 初始化输入子系统

  bool probe_gamepads(); // 探测游戏手柄是否可用

  std::shared_ptr<input_t> alloc(safe::mail_t mail); // 分配输入上下文

  /**
   * @brief 触摸端口信息结构体
   * 包含客户端和服务器的屏幕尺寸、偏移量和缩放系数
   * 用于将客户端触摸坐标正确映射到服务器屏幕
   */
  struct touch_port_t: public platf::touch_port_t {
    int env_width;     // 服务器环境宽度
    int env_height;    // 服务器环境高度

    // 客户端坐标偏移量
    float client_offsetX;
    float client_offsetY;

    float scalar_inv;       // 反向缩放系数
    float scalar_tpcoords;  // 触摸坐标缩放系数

    int env_logical_width;   // 逻辑宽度
    int env_logical_height;  // 逻辑高度

    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief 根据触摸区域大小缩放椭圆接触面积
   * @param val 长轴和短轴对
   * @param rotation 触摸/触控笔事件的旋转值
   * @param scalar 笛卡尔坐标缩放对
   * @return 缩放后的长轴和短轴对
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
