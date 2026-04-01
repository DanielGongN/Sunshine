/**
 * @file src/nvenc/nvenc_d3d11.h
 * @brief Direct3D11 NVENC编码器抽象基类声明。封装D3D11特定的库加载和异步事件处理。
 */
#pragma once
#ifdef _WIN32

  // 标准库头文件
  #include <comdef.h>  // COM智能指针定义
  #include <d3d11.h>   // Direct3D 11 API

  // 本地头文件
  #include "nvenc_base.h"  // NVENC编码器基类

namespace nvenc {

  // COM智能指针类型定义（自动管理COM对象生命周期）
  _COM_SMARTPTR_TYPEDEF(ID3D11Device, IID_ID3D11Device);       ///< D3D11设备智能指针
  _COM_SMARTPTR_TYPEDEF(ID3D11Texture2D, IID_ID3D11Texture2D); ///< D3D11 2D纹理智能指针
  _COM_SMARTPTR_TYPEDEF(IDXGIDevice, IID_IDXGIDevice);         ///< DXGI设备智能指针
  _COM_SMARTPTR_TYPEDEF(IDXGIAdapter, IID_IDXGIAdapter);       ///< DXGI适配器智能指针

  /**
   * @brief Direct3D11 NVENC编码器抽象基类。
   *        封装了原生D3D11和CUDA互操作实现的公共代码。
   */
  class nvenc_d3d11: public nvenc_base {
  public:
    explicit nvenc_d3d11(NV_ENC_DEVICE_TYPE device_type);
    ~nvenc_d3d11();

    /**
     * @brief 获取输入表面纹理（用于将捕获的屏幕图像复制到编码器输入）。
     * @return 输入表面纹理指针。
     */
    virtual ID3D11Texture2D *get_input_texture() = 0;

  protected:
    /**
     * @brief 加载NVENC动态链接库(nvEncodeAPI64.dll)并初始化函数指针。
     */
    bool init_library() override;

    /**
     * @brief 等待Windows异步事件（用于异步编码模式）。
     */
    bool wait_for_async_event(uint32_t timeout_ms) override;

  private:
    HMODULE dll = nullptr;  ///< NVENC动态链接库句柄
  };

}  // namespace nvenc
#endif
