/**
 * @file src/nvenc/nvenc_d3d11_on_cuda.h
 * @brief CUDA互操作 NVENC编码器声明。D3D11输入表面 + CUDA编码引擎。
 *        用于支持10位4:4:4 YUV编码（D3D11原生不支持的格式）。
 */
#pragma once
#ifdef _WIN32
  // 第三方库头文件
  #include <ffnvcodec/dynlink_cuda.h>  // CUDA动态链接API

  // 本地头文件
  #include "nvenc_d3d11.h"  // D3D11 NVENC基类

namespace nvenc {

  /**
   * @brief CUDA互操作 D3D11 NVENC编码器。
   *        输入表面为D3D11纹理，通过CUDA进行实际编码。
   *        编码前会将D3D11纹理数据拷贝到CUDA设备内存。
   */
  class nvenc_d3d11_on_cuda final: public nvenc_d3d11 {
  public:
    /**
     * @param d3d_device 创建输入纹理的D3D11设备，CUDA编码设备将从中派生。
     */
    explicit nvenc_d3d11_on_cuda(ID3D11Device *d3d_device);
    ~nvenc_d3d11_on_cuda();

    /**
     * @brief 获取D3D11输入纹理（用于拷贝捕获的屏幕图像）。
     */
    ID3D11Texture2D *get_input_texture() override;

  private:
    bool init_library() override;  ///< 加载nvcuda.dll并创建CUDA上下文

    bool create_and_register_input_buffer() override;  ///< 创建D3D11纹理和CUDA设备内存，注册为NVENC输入

    bool synchronize_input_buffer() override;  ///< 将D3D11纹理数据拷贝到CUDA设备内存

    /**
     * @brief 检查CUDA操作是否成功。
     */
    bool cuda_succeeded(CUresult result);

    /**
     * @brief 检查CUDA操作是否失败。
     */
    bool cuda_failed(CUresult result);

    /**
     * @brief CUDA上下文自动弹出的RAII包装器。
     *        构造时压入CUDA上下文，析构时自动弹出。
     */
      autopop_context(nvenc_d3d11_on_cuda &parent, CUcontext pushed_context):
          parent(parent),
          pushed_context(pushed_context) {
      }

      ~autopop_context();

      explicit operator bool() const {
        return pushed_context != nullptr;
      }

      nvenc_d3d11_on_cuda &parent;
      CUcontext pushed_context = nullptr;
    };

    autopop_context push_context();

    const ID3D11DevicePtr d3d_device;  ///< D3D11设备智能指针
    ID3D11Texture2DPtr d3d_input_texture;  ///< D3D11输入纹理智能指针

    /**
     * @brief CUDA动态加载的函数指针集合。
     *        从nvcuda.dll加载，包含上下文管理、内存分配、图形资源互操作等功能。
     */
    struct {
      tcuInit *cuInit;  ///< CUDA初始化
      tcuD3D11GetDevice *cuD3D11GetDevice;  ///< 从D3D11适配器获取CUDA设备
      tcuCtxCreate_v2 *cuCtxCreate;  ///< 创建CUDA上下文
      tcuCtxDestroy_v2 *cuCtxDestroy;  ///< 销毁CUDA上下文
      tcuCtxPushCurrent_v2 *cuCtxPushCurrent;  ///< 压入CUDA上下文
      tcuCtxPopCurrent_v2 *cuCtxPopCurrent;  ///< 弹出CUDA上下文
      tcuMemAllocPitch_v2 *cuMemAllocPitch;  ///< 分配对齐的设备内存
      tcuMemFree_v2 *cuMemFree;  ///< 释放设备内存
      tcuGraphicsD3D11RegisterResource *cuGraphicsD3D11RegisterResource;  ///< 注册D3D11资源用于CUDA互操作
      tcuGraphicsUnregisterResource *cuGraphicsUnregisterResource;  ///< 取消注册互操作资源
      tcuGraphicsMapResources *cuGraphicsMapResources;  ///< 映射图形资源到CUDA
      tcuGraphicsUnmapResources *cuGraphicsUnmapResources;  ///< 取消映射图形资源
      tcuGraphicsSubResourceGetMappedArray *cuGraphicsSubResourceGetMappedArray;  ///< 获取映射的CUDA数组
      tcuMemcpy2D_v2 *cuMemcpy2D;  ///< 2D内存拷贝（D3D11纹理→CUDA设备内存）
      HMODULE dll;  ///< nvcuda.dll句柄
    } cuda_functions = {};

    CUresult last_cuda_error = CUDA_SUCCESS;  ///< 最后一次CUDA错误代码
    CUcontext cuda_context = nullptr;  ///< CUDA上下文
    CUgraphicsResource cuda_d3d_input_texture = nullptr;  ///< D3D11纹理在CUDA中的图形资源句柄
    CUdeviceptr cuda_surface = 0;  ///< CUDA设备内存中的编码输入表面
    size_t cuda_surface_pitch = 0;  ///< CUDA表面的行对齐字节数
  };

}  // namespace nvenc
#endif
