/**
 * @file src/nvenc/nvenc_d3d11_native.h
 * @brief 原生Direct3D11 NVENC编码器声明。直接使用D3D11纹理作为编码输入。
 */
#pragma once
#ifdef _WIN32
  // 标准库头文件
  #include <comdef.h>  // COM智能指针
  #include <d3d11.h>   // Direct3D 11 API

  // 本地头文件
  #include "nvenc_d3d11.h"  // D3D11 NVENC基类

namespace nvenc {

  /**
   * @brief 原生Direct3D11 NVENC编码器。
   *        直接在D3D11设备上进行编码，不涉及CUDA互操作。
   */
  class nvenc_d3d11_native final: public nvenc_d3d11 {
  public:
    /**
     * @param d3d_device 用于编码的Direct3D11设备。
     */
    explicit nvenc_d3d11_native(ID3D11Device *d3d_device);
    ~nvenc_d3d11_native();

    /**
     * @brief 获取输入纹理（用于拷贝捕获的屏幕图像）。
     */
    ID3D11Texture2D *get_input_texture() override;

  private:
    /**
     * @brief 创建D3D11输入纹理并注册为NVENC资源。
     */
    bool create_and_register_input_buffer() override;

    const ID3D11DevicePtr d3d_device;  ///< D3D11设备智能指针
    ID3D11Texture2DPtr d3d_input_texture;  ///< 输入纹理智能指针
  };

}  // namespace nvenc
#endif
