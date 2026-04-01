/**
 * @file src/nvenc/nvenc_d3d11_native.cpp
 * @brief 原生Direct3D11 NVENC编码器实现。
 *        包括构造/析构、输入纹理创建和NVENC资源注册。
 */
#ifdef _WIN32
  // 本模块头文件
  #include "nvenc_d3d11_native.h"

  // 本地头文件
  #include "nvenc_utils.h"  // 像素格式转换工具

namespace nvenc {

  nvenc_d3d11_native::nvenc_d3d11_native(ID3D11Device *d3d_device):
      nvenc_d3d11(NV_ENC_DEVICE_TYPE_DIRECTX),
      d3d_device(d3d_device) {
    device = d3d_device;
  }

  nvenc_d3d11_native::~nvenc_d3d11_native() {
    if (encoder) {
      destroy_encoder();
    }
  }

  ID3D11Texture2D *
    nvenc_d3d11_native::get_input_texture() {
    return d3d_input_texture.GetInterfacePtr();
  }

  /**
   * @brief 创建并注册D3D11原生输入纹理缓冲区（不支持10bit 4:4:4，需用CUDA互操作）
   */
  bool nvenc_d3d11_native::create_and_register_input_buffer() {
    if (encoder_params.buffer_format == NV_ENC_BUFFER_FORMAT_YUV444_10BIT) {
      BOOST_LOG(error) << "NvEnc: 10-bit 4:4:4 encoding is incompatible with D3D11 surface formats, use CUDA interop";
      return false;
    }

    if (!d3d_input_texture) {
      D3D11_TEXTURE2D_DESC desc = {};
      desc.Width = encoder_params.width;
      desc.Height = encoder_params.height;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = dxgi_format_from_nvenc_format(encoder_params.buffer_format);
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET;
      if (d3d_device->CreateTexture2D(&desc, nullptr, &d3d_input_texture) != S_OK) {
        BOOST_LOG(error) << "NvEnc: couldn't create input texture";
        return false;
      }
    }

    if (!registered_input_buffer) {
      NV_ENC_REGISTER_RESOURCE register_resource = {min_struct_version(NV_ENC_REGISTER_RESOURCE_VER, 3, 4)};
      register_resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
      register_resource.width = encoder_params.width;
      register_resource.height = encoder_params.height;
      register_resource.resourceToRegister = d3d_input_texture.GetInterfacePtr();
      register_resource.bufferFormat = encoder_params.buffer_format;
      register_resource.bufferUsage = NV_ENC_INPUT_IMAGE;

      if (nvenc_failed(nvenc->nvEncRegisterResource(encoder, &register_resource))) {
        BOOST_LOG(error) << "NvEnc: NvEncRegisterResource() failed: " << last_nvenc_error_string;
        return false;
      }

      registered_input_buffer = register_resource.registeredResource;
    }

    return true;
  }

}  // namespace nvenc
#endif
