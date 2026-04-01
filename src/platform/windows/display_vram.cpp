/**
 * @file src/platform/windows/display_vram.cpp
 * @brief 显存（VRAM）显示捕获实现。使用D3D11着色器在GPU上直接进行色彩空间转换。
 */
// standard includes
#include <cmath>

// platform includes
#include <d3dcompiler.h>
#include <DirectXMath.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_d3d11va.h>
}

// lib includes
#include <AMF/core/Factory.h>
#include <boost/algorithm/string/predicate.hpp>

// local includes
#include "display.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/nvenc/nvenc_config.h"
#include "src/nvenc/nvenc_d3d11_native.h"
#include "src/nvenc/nvenc_d3d11_on_cuda.h"
#include "src/nvenc/nvenc_utils.h"
#include "src/video.h"
#include "utf_utils.h"

#if !defined(SUNSHINE_SHADERS_DIR)  // for testing this needs to be defined in cmake as we don't do an install
  #define SUNSHINE_SHADERS_DIR SUNSHINE_ASSETS_DIR "/shaders/directx"
#endif
namespace platf {
  using namespace std::literals;
}

static void free_frame(AVFrame *frame) {
  av_frame_free(&frame);
}

using frame_t = util::safe_ptr<AVFrame, free_frame>;

namespace platf::dxgi {

  template<class T>
  buf_t make_buffer(device_t::pointer device, const T &t) {
    static_assert(sizeof(T) % 16 == 0, "Buffer needs to be aligned on a 16-byte alignment");

    D3D11_BUFFER_DESC buffer_desc {
      sizeof(T),
      D3D11_USAGE_IMMUTABLE,
      D3D11_BIND_CONSTANT_BUFFER
    };

    D3D11_SUBRESOURCE_DATA init_data {
      &t
    };

    buf_t::pointer buf_p;
    auto status = device->CreateBuffer(&buffer_desc, &init_data, &buf_p);
    if (status) {
      BOOST_LOG(error) << "Failed to create buffer: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    return buf_t {buf_p};
  }

  blend_t make_blend(device_t::pointer device, bool enable, bool invert) {
    D3D11_BLEND_DESC bdesc {};
    auto &rt = bdesc.RenderTarget[0];
    rt.BlendEnable = enable;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (enable) {
      rt.BlendOp = D3D11_BLEND_OP_ADD;
      rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;

      if (invert) {
        // Invert colors
        rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
        rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
      } else {
        // Regular alpha blending
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
      }

      rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
      rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    }

    blend_t blend;
    auto status = device->CreateBlendState(&bdesc, &blend);
    if (status) {
      BOOST_LOG(error) << "Failed to create blend state: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    return blend;
  }

  blob_t convert_yuv420_packed_uv_type0_ps_hlsl;
  blob_t convert_yuv420_packed_uv_type0_ps_linear_hlsl;
  blob_t convert_yuv420_packed_uv_type0_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv420_packed_uv_type0_vs_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_ps_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_ps_linear_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_vs_hlsl;
  blob_t convert_yuv420_planar_y_ps_hlsl;
  blob_t convert_yuv420_planar_y_ps_linear_hlsl;
  blob_t convert_yuv420_planar_y_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv420_planar_y_vs_hlsl;
  blob_t convert_yuv444_packed_ayuv_ps_hlsl;
  blob_t convert_yuv444_packed_ayuv_ps_linear_hlsl;
  blob_t convert_yuv444_packed_vs_hlsl;
  blob_t convert_yuv444_planar_ps_hlsl;
  blob_t convert_yuv444_planar_ps_linear_hlsl;
  blob_t convert_yuv444_planar_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv444_packed_y410_ps_hlsl;
  blob_t convert_yuv444_packed_y410_ps_linear_hlsl;
  blob_t convert_yuv444_packed_y410_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv444_planar_vs_hlsl;
  blob_t cursor_ps_hlsl;
  blob_t cursor_ps_normalize_white_hlsl;
  blob_t cursor_vs_hlsl;

  struct img_d3d_t: public platf::img_t {
    // These objects are owned by the display_t's ID3D11Device
    texture2d_t capture_texture;
    render_target_t capture_rt;
    keyed_mutex_t capture_mutex;

    // This is the shared handle used by hwdevice_t to open capture_texture
    HANDLE encoder_texture_handle = {};

    // Set to true if the image corresponds to a dummy texture used prior to
    // the first successful capture of a desktop frame
    bool dummy = false;

    // Set to true if the image is blank (contains no content at all, including a cursor)
    bool blank = true;

    // Unique identifier for this image
    uint32_t id = 0;

    // DXGI format of this image texture
    DXGI_FORMAT format;

    virtual ~img_d3d_t() override {
      if (encoder_texture_handle) {
        CloseHandle(encoder_texture_handle);
      }
    };
  };

  struct texture_lock_helper {
    keyed_mutex_t _mutex;
    bool _locked = false;

    texture_lock_helper(const texture_lock_helper &) = delete;
    texture_lock_helper &operator=(const texture_lock_helper &) = delete;

    texture_lock_helper(texture_lock_helper &&other) {
      _mutex.reset(other._mutex.release());
      _locked = other._locked;
      other._locked = false;
    }

    texture_lock_helper &operator=(texture_lock_helper &&other) {
      if (_locked) {
        _mutex->ReleaseSync(0);
      }
      _mutex.reset(other._mutex.release());
      _locked = other._locked;
      other._locked = false;
      return *this;
    }

    texture_lock_helper(IDXGIKeyedMutex *mutex):
        _mutex(mutex) {
      if (_mutex) {
        _mutex->AddRef();
      }
    }

    ~texture_lock_helper() {
      if (_locked) {
        _mutex->ReleaseSync(0);
      }
    }

    bool lock() {
      if (_locked) {
        return true;
      }
      HRESULT status = _mutex->AcquireSync(0, INFINITE);
      if (status == S_OK) {
        _locked = true;
      } else {
        BOOST_LOG(error) << "Failed to acquire texture mutex [0x"sv << util::hex(status).to_string_view() << ']';
      }
      return _locked;
    }
  };

  util::buffer_t<std::uint8_t> make_cursor_xor_image(const util::buffer_t<std::uint8_t> &img_data, DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info) {
    constexpr std::uint32_t inverted = 0xFFFFFFFF;
    constexpr std::uint32_t transparent = 0;

    switch (shape_info.Type) {
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        // This type doesn't require any XOR-blending
        return {};
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        {
          util::buffer_t<std::uint8_t> cursor_img = img_data;
          std::for_each((std::uint32_t *) std::begin(cursor_img), (std::uint32_t *) std::end(cursor_img), [](auto &pixel) {
            auto alpha = (std::uint8_t) ((pixel >> 24) & 0xFF);
            if (alpha == 0xFF) {
              // Pixels with 0xFF alpha will be XOR-blended as is.
            } else if (alpha == 0x00) {
              // Pixels with 0x00 alpha will be blended by make_cursor_alpha_image().
              // We make them transparent for the XOR-blended cursor image.
              pixel = transparent;
            } else {
              // Other alpha values are illegal in masked color cursors
              BOOST_LOG(warning) << "Illegal alpha value in masked color cursor: " << alpha;
            }
          });
          return cursor_img;
        }
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        // Monochrome is handled below
        break;
      default:
        BOOST_LOG(error) << "Invalid cursor shape type: " << shape_info.Type;
        return {};
    }

    shape_info.Height /= 2;

    util::buffer_t<std::uint8_t> cursor_img {shape_info.Width * shape_info.Height * 4};

    auto bytes = shape_info.Pitch * shape_info.Height;
    auto pixel_begin = (std::uint32_t *) std::begin(cursor_img);
    auto pixel_data = pixel_begin;
    auto and_mask = std::begin(img_data);
    auto xor_mask = std::begin(img_data) + bytes;

    for (auto x = 0; x < bytes; ++x) {
      for (auto c = 7; c >= 0 && ((std::uint8_t *) pixel_data) != std::end(cursor_img); --c) {
        auto bit = 1 << c;
        auto color_type = ((*and_mask & bit) ? 1 : 0) + ((*xor_mask & bit) ? 2 : 0);

        switch (color_type) {
          case 0:  // Opaque black (handled by alpha-blending)
          case 2:  // Opaque white (handled by alpha-blending)
          case 1:  // Color of screen (transparent)
            *pixel_data = transparent;
            break;
          case 3:  // Inverse of screen
            *pixel_data = inverted;
            break;
        }

        ++pixel_data;
      }
      ++and_mask;
      ++xor_mask;
    }

    return cursor_img;
  }

  util::buffer_t<std::uint8_t> make_cursor_alpha_image(const util::buffer_t<std::uint8_t> &img_data, DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info) {
    constexpr std::uint32_t black = 0xFF000000;
    constexpr std::uint32_t white = 0xFFFFFFFF;
    constexpr std::uint32_t transparent = 0;

    switch (shape_info.Type) {
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        {
          util::buffer_t<std::uint8_t> cursor_img = img_data;
          std::for_each((std::uint32_t *) std::begin(cursor_img), (std::uint32_t *) std::end(cursor_img), [](auto &pixel) {
            auto alpha = (std::uint8_t) ((pixel >> 24) & 0xFF);
            if (alpha == 0xFF) {
              // Pixels with 0xFF alpha will be XOR-blended by make_cursor_xor_image().
              // We make them transparent for the alpha-blended cursor image.
              pixel = transparent;
            } else if (alpha == 0x00) {
              // Pixels with 0x00 alpha will be blended as opaque with the alpha-blended image.
              pixel |= 0xFF000000;
            } else {
              // Other alpha values are illegal in masked color cursors
              BOOST_LOG(warning) << "Illegal alpha value in masked color cursor: " << alpha;
            }
          });
          return cursor_img;
        }
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        // Color cursors are just an ARGB bitmap which requires no processing.
        return img_data;
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        // Monochrome cursors are handled below.
        break;
      default:
        BOOST_LOG(error) << "Invalid cursor shape type: " << shape_info.Type;
        return {};
    }

    shape_info.Height /= 2;

    util::buffer_t<std::uint8_t> cursor_img {shape_info.Width * shape_info.Height * 4};

    auto bytes = shape_info.Pitch * shape_info.Height;
    auto pixel_begin = (std::uint32_t *) std::begin(cursor_img);
    auto pixel_data = pixel_begin;
    auto and_mask = std::begin(img_data);
    auto xor_mask = std::begin(img_data) + bytes;

    for (auto x = 0; x < bytes; ++x) {
      for (auto c = 7; c >= 0 && ((std::uint8_t *) pixel_data) != std::end(cursor_img); --c) {
        auto bit = 1 << c;
        auto color_type = ((*and_mask & bit) ? 1 : 0) + ((*xor_mask & bit) ? 2 : 0);

        switch (color_type) {
          case 0:  // Opaque black
            *pixel_data = black;
            break;
          case 2:  // Opaque white
            *pixel_data = white;
            break;
          case 3:  // Inverse of screen (handled by XOR blending)
          case 1:  // Color of screen (transparent)
            *pixel_data = transparent;
            break;
        }

        ++pixel_data;
      }
      ++and_mask;
      ++xor_mask;
    }

    return cursor_img;
  }

  blob_t compile_shader(LPCSTR file, LPCSTR entrypoint, LPCSTR shader_model) {
    blob_t::pointer msg_p = nullptr;
    blob_t::pointer compiled_p;

    DWORD flags = D3DCOMPILE_ENABLE_STRICTNESS;

#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto wFile = utf_utils::from_utf8(file);
    auto status = D3DCompileFromFile(wFile.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entrypoint, shader_model, flags, 0, &compiled_p, &msg_p);

    if (msg_p) {
      BOOST_LOG(warning) << std::string_view {(const char *) msg_p->GetBufferPointer(), msg_p->GetBufferSize() - 1};
      msg_p->Release();
    }

    if (status) {
      BOOST_LOG(error) << "Couldn't compile ["sv << file << "] [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    return blob_t {compiled_p};
  }

  blob_t compile_pixel_shader(LPCSTR file) {
    return compile_shader(file, "main_ps", "ps_5_0");
  }

  blob_t compile_vertex_shader(LPCSTR file) {
    return compile_shader(file, "main_vs", "vs_5_0");
  }

  class d3d_base_encode_device final {
  public:
    int convert(platf::img_t &img_base) {
      // 垃圾回收：清理弱引用已过期的捕获图像上下文
      for (auto it = img_ctx_map.begin(); it != img_ctx_map.end();) {
        if (it->second.img_weak.expired()) {
          it = img_ctx_map.erase(it);
        } else {
          it++;
        }
      }

      auto &img = (img_d3d_t &) img_base;
      if (!img.blank) {
        auto &img_ctx = img_ctx_map[img.id];

        // 用编码器的D3D设备打开捕获端共享纹理
        if (initialize_image_context(img, img_ctx)) {
          return -1;
        }

        // 获取编码器互斥锁，与捕获线程同步访问共享纹理
        auto status = img_ctx.encoder_mutex->AcquireSync(0, INFINITE);
        if (status != S_OK) {
          BOOST_LOG(error) << "Failed to acquire encoder mutex [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }

        // draw: GPU上执行RGB→YUV色彩转换的核心绘制lambda
        auto draw = [&](auto &input, auto &y_or_yuv_viewports, auto &uv_viewport) {
          // 绑定输入纹理（捕获的RGB帧）到像素着色器资源槽0
          device_ctx->PSSetShaderResources(0, 1, &input);

          // === 第一步：渲染Y平面（亮度）或完整YUV（打包格式）===
          device_ctx->OMSetRenderTargets(1, &out_Y_or_YUV_rtv, nullptr);
          device_ctx->VSSetShader(convert_Y_or_YUV_vs.get(), nullptr, 0);
          // HDR（fp16）输入使用专用的感知量化/线性着色器
          device_ctx->PSSetShader(img.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? convert_Y_or_YUV_fp16_ps.get() : convert_Y_or_YUV_ps.get(), nullptr, 0);
          // YUV444平面格式需要3个viewport分别渲染Y/U/V平面
          auto viewport_count = (format == DXGI_FORMAT_R16_UINT) ? 3 : 1;
          assert(viewport_count <= y_or_yuv_viewports.size());
          device_ctx->RSSetViewports(viewport_count, y_or_yuv_viewports.data());
          device_ctx->Draw(3 * viewport_count, 0);  // 顶点着色器将顶点分散到各viewport

          // === 第二步：渲染UV平面（色度），仅NV12/P010半平面格式需要 ===
          if (out_UV_rtv) {
            assert(format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010);
            device_ctx->OMSetRenderTargets(1, &out_UV_rtv, nullptr);
            device_ctx->VSSetShader(convert_UV_vs.get(), nullptr, 0);
            device_ctx->PSSetShader(img.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? convert_UV_fp16_ps.get() : convert_UV_ps.get(), nullptr, 0);
            device_ctx->RSSetViewports(1, &uv_viewport);
            device_ctx->Draw(3, 0);
          }
        };

        // 首次渲染时用黑色填充整个输出区域，使宽高比不匹配的"黑边"正确显示
        if (!rtvs_cleared) {
          auto black = create_black_texture_for_rtv_clear();
          if (black) {
            draw(black, out_Y_or_YUV_viewports_for_clear, out_UV_viewport_for_clear);
          }
          rtvs_cleared = true;
        }

        // 执行实际的RGB→YUV色彩转换绘制
        draw(img_ctx.encoder_input_res, out_Y_or_YUV_viewports, out_UV_viewport);

        // 释放编码器互斥锁，允许捕获线程继续写入此纹理
        img_ctx.encoder_mutex->ReleaseSync(0);

        // 解绑着色器资源，避免后续操作冲突
        ID3D11ShaderResourceView *emptyShaderResourceView = nullptr;
        device_ctx->PSSetShaderResources(0, 1, &emptyShaderResourceView);
      }

      return 0;
    }

    void apply_colorspace(const ::video::sunshine_colorspace_t &colorspace) {
      auto color_vectors = ::video::color_vectors_from_colorspace(colorspace, true);

      if (format == DXGI_FORMAT_AYUV ||
          format == DXGI_FORMAT_R16_UINT ||
          format == DXGI_FORMAT_Y410) {
        color_vectors = ::video::color_vectors_from_colorspace(colorspace, false);
      }

      if (!color_vectors) {
        BOOST_LOG(error) << "No vector data for colorspace"sv;
        return;
      }

      auto color_matrix = make_buffer(device.get(), *color_vectors);
      if (!color_matrix) {
        BOOST_LOG(warning) << "Failed to create color matrix"sv;
        return;
      }

      device_ctx->VSSetConstantBuffers(3, 1, &color_matrix);
      device_ctx->PSSetConstantBuffers(0, 1, &color_matrix);
      this->color_matrix = std::move(color_matrix);
    }

    int init_output(ID3D11Texture2D *frame_texture, int width, int height) {
      // 帧池拥有纹理所有权，我们必须自己增加引用计数
      frame_texture->AddRef();
      output_texture.reset(frame_texture);

      HRESULT status = S_OK;

#define create_vertex_shader_helper(x, y) \
  if (FAILED(status = device->CreateVertexShader(x->GetBufferPointer(), x->GetBufferSize(), nullptr, &y))) { \
    BOOST_LOG(error) << "Failed to create vertex shader " << #x << ": " << util::log_hex(status); \
    return -1; \
  }
#define create_pixel_shader_helper(x, y) \
  if (FAILED(status = device->CreatePixelShader(x->GetBufferPointer(), x->GetBufferSize(), nullptr, &y))) { \
    BOOST_LOG(error) << "Failed to create pixel shader " << #x << ": " << util::log_hex(status); \
    return -1; \
  }

      // 是否需要下采样（捕获分辨率 > 编码分辨率）
      const bool downscaling = display->width > width || display->height > height;

      // 根据输出像素格式选择对应的顶点/像素着色器组合
      switch (format) {
        case DXGI_FORMAT_NV12:
          // NV12: 半平面8位 YUV 4:2:0，Y和UV分开渲染
          create_vertex_shader_helper(convert_yuv420_planar_y_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv420_planar_y_ps_hlsl, convert_Y_or_YUV_ps);
          create_pixel_shader_helper(convert_yuv420_planar_y_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
          if (downscaling) {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0s_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_hlsl, convert_UV_ps);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_linear_hlsl, convert_UV_fp16_ps);
          } else {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_hlsl, convert_UV_ps);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_linear_hlsl, convert_UV_fp16_ps);
          }
          break;

        case DXGI_FORMAT_P010:
          // P010: 半平面16位 YUV 4:2:0，高10位存储值，用于HDR
          create_vertex_shader_helper(convert_yuv420_planar_y_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv420_planar_y_ps_hlsl, convert_Y_or_YUV_ps);
          if (display->is_hdr()) {
            create_pixel_shader_helper(convert_yuv420_planar_y_ps_perceptual_quantizer_hlsl, convert_Y_or_YUV_fp16_ps);
          } else {
            create_pixel_shader_helper(convert_yuv420_planar_y_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
          }
          if (downscaling) {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0s_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_hlsl, convert_UV_ps);
            if (display->is_hdr()) {
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer_hlsl, convert_UV_fp16_ps);
            } else {
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_linear_hlsl, convert_UV_fp16_ps);
            }
          } else {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_hlsl, convert_UV_ps);
            if (display->is_hdr()) {
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_perceptual_quantizer_hlsl, convert_UV_fp16_ps);
            } else {
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_linear_hlsl, convert_UV_fp16_ps);
            }
          }
          break;

        case DXGI_FORMAT_R16_UINT:
          // R16_UINT: 平面16位 YUV 4:4:4，高10位存储值，3个平面分别渲染
          create_vertex_shader_helper(convert_yuv444_planar_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv444_planar_ps_hlsl, convert_Y_or_YUV_ps);
          if (display->is_hdr()) {
            create_pixel_shader_helper(convert_yuv444_planar_ps_perceptual_quantizer_hlsl, convert_Y_or_YUV_fp16_ps);
          } else {
            create_pixel_shader_helper(convert_yuv444_planar_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
          }
          break;

        case DXGI_FORMAT_AYUV:
          // AYUV: 打包8位 YUV 4:4:4，一次渲染整个YUV
          create_vertex_shader_helper(convert_yuv444_packed_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv444_packed_ayuv_ps_hlsl, convert_Y_or_YUV_ps);
          create_pixel_shader_helper(convert_yuv444_packed_ayuv_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
          break;

        case DXGI_FORMAT_Y410:
          // Y410: 打包10位 YUV 4:4:4，一次渲染整个YUV
          create_vertex_shader_helper(convert_yuv444_packed_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv444_packed_y410_ps_hlsl, convert_Y_or_YUV_ps);
          if (display->is_hdr()) {
            create_pixel_shader_helper(convert_yuv444_packed_y410_ps_perceptual_quantizer_hlsl, convert_Y_or_YUV_fp16_ps);
          } else {
            create_pixel_shader_helper(convert_yuv444_packed_y410_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
          }
          break;

        default:
          BOOST_LOG(error) << "Unable to create shaders because of the unrecognized surface format";
          return -1;
      }

#undef create_vertex_shader_helper
#undef create_pixel_shader_helper

      auto out_width = width;
      auto out_height = height;

      float in_width = display->width;
      float in_height = display->height;

      // 计算等比缩放因子，保持捕获图像的宽高比
      auto scalar = std::fminf(out_width / in_width, out_height / in_height);
      auto out_width_f = in_width * scalar;
      auto out_height_f = in_height * scalar;

      // 计算居中偏移（用于上下/左右黑边填充）
      auto offsetX = (out_width - out_width_f) / 2;
      auto offsetY = (out_height - out_height_f) / 2;

      // 设置Y/U/V平面viewport（平面格式时3个，平面纵向排列）
      out_Y_or_YUV_viewports[0] = {offsetX, offsetY, out_width_f, out_height_f, 0.0f, 1.0f};  // Y平面
      out_Y_or_YUV_viewports[1] = out_Y_or_YUV_viewports[0];  // U平面
      out_Y_or_YUV_viewports[1].TopLeftY += out_height;
      out_Y_or_YUV_viewports[2] = out_Y_or_YUV_viewports[1];  // V平面
      out_Y_or_YUV_viewports[2].TopLeftY += out_height;

      // 用于首次清屏的全尺viewport（填充黑边）
      out_Y_or_YUV_viewports_for_clear[0] = {0, 0, (float) out_width, (float) out_height, 0.0f, 1.0f};  // Y平面
      out_Y_or_YUV_viewports_for_clear[1] = out_Y_or_YUV_viewports_for_clear[0];  // U平面
      out_Y_or_YUV_viewports_for_clear[1].TopLeftY += out_height;
      out_Y_or_YUV_viewports_for_clear[2] = out_Y_or_YUV_viewports_for_clear[1];  // V平面
      out_Y_or_YUV_viewports_for_clear[2].TopLeftY += out_height;

      // UV平面viewport（分辨率为Y平面的一半，4:2:0下采样）
      out_UV_viewport = {offsetX / 2, offsetY / 2, out_width_f / 2, out_height_f / 2, 0.0f, 1.0f};
      out_UV_viewport_for_clear = {0, 0, (float) out_width / 2, (float) out_height / 2, 0.0f, 1.0f};

      float subsample_offset_in[16 / sizeof(float)] {1.0f / (float) out_width_f, 1.0f / (float) out_height_f};  // aligned to 16-byte
      subsample_offset = make_buffer(device.get(), subsample_offset_in);

      if (!subsample_offset) {
        BOOST_LOG(error) << "Failed to create subsample offset vertex constant buffer";
        return -1;
      }
      device_ctx->VSSetConstantBuffers(0, 1, &subsample_offset);

      {
        int32_t rotation_modifier = display->display_rotation == DXGI_MODE_ROTATION_UNSPECIFIED ? 0 : display->display_rotation - 1;
        int32_t rotation_data[16 / sizeof(int32_t)] {-rotation_modifier};  // aligned to 16-byte
        auto rotation = make_buffer(device.get(), rotation_data);
        if (!rotation) {
          BOOST_LOG(error) << "Failed to create display rotation vertex constant buffer";
          return -1;
        }
        device_ctx->VSSetConstantBuffers(1, 1, &rotation);
      }

      DXGI_FORMAT rtv_Y_or_YUV_format = DXGI_FORMAT_UNKNOWN;
      DXGI_FORMAT rtv_UV_format = DXGI_FORMAT_UNKNOWN;
      bool rtv_simple_clear = false;

      switch (format) {
        case DXGI_FORMAT_NV12:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R8_UNORM;
          rtv_UV_format = DXGI_FORMAT_R8G8_UNORM;
          rtv_simple_clear = true;
          break;

        case DXGI_FORMAT_P010:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R16_UNORM;
          rtv_UV_format = DXGI_FORMAT_R16G16_UNORM;
          rtv_simple_clear = true;
          break;

        case DXGI_FORMAT_AYUV:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R8G8B8A8_UINT;
          break;

        case DXGI_FORMAT_R16_UINT:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R16_UINT;
          break;

        case DXGI_FORMAT_Y410:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R10G10B10A2_UINT;
          break;

        default:
          BOOST_LOG(error) << "Unable to create render target views because of the unrecognized surface format";
          return -1;
      }

      auto create_rtv = [&](auto &rt, DXGI_FORMAT rt_format) -> bool {
        D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = rt_format;
        rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

        auto status = device->CreateRenderTargetView(output_texture.get(), &rtv_desc, &rt);
        if (FAILED(status)) {
          BOOST_LOG(error) << "Failed to create render target view: " << util::log_hex(status);
          return false;
        }

        return true;
      };

      // Create Y/YUV render target view
      if (!create_rtv(out_Y_or_YUV_rtv, rtv_Y_or_YUV_format)) {
        return -1;
      }

      // Create UV render target view if needed
      if (rtv_UV_format != DXGI_FORMAT_UNKNOWN && !create_rtv(out_UV_rtv, rtv_UV_format)) {
        return -1;
      }

      if (rtv_simple_clear) {
        // Clear the RTVs to ensure the aspect ratio padding is black
        const float y_black[] = {0.0f, 0.0f, 0.0f, 0.0f};
        device_ctx->ClearRenderTargetView(out_Y_or_YUV_rtv.get(), y_black);
        if (out_UV_rtv) {
          const float uv_black[] = {0.5f, 0.5f, 0.5f, 0.5f};
          device_ctx->ClearRenderTargetView(out_UV_rtv.get(), uv_black);
        }
        rtvs_cleared = true;
      } else {
        // Can't use ClearRenderTargetView(), will clear on first convert()
        rtvs_cleared = false;
      }

      return 0;
    }

    int init(std::shared_ptr<platf::display_t> display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      // 将Sunshine内部像素格式映射为DXGI纹理格式
      switch (pix_fmt) {
        case pix_fmt_e::nv12:
          format = DXGI_FORMAT_NV12;
          break;

        case pix_fmt_e::p010:
          format = DXGI_FORMAT_P010;
          break;

        case pix_fmt_e::ayuv:
          format = DXGI_FORMAT_AYUV;
          break;

        case pix_fmt_e::yuv444p16:
          format = DXGI_FORMAT_R16_UINT;
          break;

        case pix_fmt_e::y410:
          format = DXGI_FORMAT_Y410;
          break;

        default:
          BOOST_LOG(error) << "D3D11 backend doesn't support pixel format: " << from_pix_fmt(pix_fmt);
          return -1;
      }

      D3D_FEATURE_LEVEL featureLevels[] {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1
      };

      // 为编码器创建独立的D3D11设备（与捕获设备分离，支持VIDEO_SUPPORT用于硬编码）
      HRESULT status = D3D11CreateDevice(
        adapter_p,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_FLAGS | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        featureLevels,
        sizeof(featureLevels) / sizeof(D3D_FEATURE_LEVEL),
        D3D11_SDK_VERSION,
        &device,
        nullptr,
        &device_ctx
      );

      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create encoder D3D11 device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // 提升编码器GPU线程优先级（最高7），减少GPU调度延迟
      dxgi::dxgi_t dxgi;
      status = device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      status = dxgi->SetGPUThreadPriority(7);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to increase encoding GPU thread priority. Please run application as administrator for optimal performance.";
      }

      // 创建默认色彩矩阵常量缓冲区（Rec.601），后续apply_colorspace()会按实际色彩空间更新
      auto default_color_vectors = ::video::color_vectors_from_colorspace({::video::colorspace_e::rec601, false, 8}, true);
      if (!default_color_vectors) {
        BOOST_LOG(error) << "Missing color vectors for Rec. 601"sv;
        return -1;
      }

      color_matrix = make_buffer(device.get(), *default_color_vectors);
      if (!color_matrix) {
        BOOST_LOG(error) << "Failed to create color matrix buffer"sv;
        return -1;
      }
      device_ctx->VSSetConstantBuffers(3, 1, &color_matrix);
      device_ctx->PSSetConstantBuffers(0, 1, &color_matrix);

      this->display = std::dynamic_pointer_cast<display_base_t>(display);
      if (!this->display) {
        return -1;
      }
      display = nullptr;

      // 创建禁用混合的混合状态（色彩转换时不需要alpha混合）
      blend_disable = make_blend(device.get(), false, false);
      if (!blend_disable) {
        return -1;
      }

      // 创建线性采样器：用于纹理缩放时的双线性插值
      D3D11_SAMPLER_DESC sampler_desc {};
      sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
      sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
      sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
      sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
      sampler_desc.MinLOD = 0;
      sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

      status = device->CreateSamplerState(&sampler_desc, &sampler_linear);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create point sampler state [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // 设置渲染管线初始状态：禁用混合、绑定线性采样器、使用三角形列表拓扑
      device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);
      device_ctx->PSSetSamplers(0, 1, &sampler_linear);
      device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

      return 0;
    }

    struct encoder_img_ctx_t {
      // Used to determine if the underlying texture changes.
      // Not safe for actual use by the encoder!
      texture2d_t::const_pointer capture_texture_p;

      texture2d_t encoder_texture;
      shader_res_t encoder_input_res;
      keyed_mutex_t encoder_mutex;

      std::weak_ptr<const platf::img_t> img_weak;

      void reset() {
        capture_texture_p = nullptr;
        encoder_texture.reset();
        encoder_input_res.reset();
        encoder_mutex.reset();
        img_weak.reset();
      }
    };

    int initialize_image_context(const img_d3d_t &img, encoder_img_ctx_t &img_ctx) {
      // If we've already opened the shared texture, we're done
      if (img_ctx.encoder_texture && img.capture_texture.get() == img_ctx.capture_texture_p) {
        return 0;
      }

      // Reset this image context in case it was used before with a different texture.
      // Textures can change when transitioning from a dummy image to a real image.
      img_ctx.reset();

      device1_t device1;
      auto status = device->QueryInterface(__uuidof(ID3D11Device1), (void **) &device1);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to query ID3D11Device1 [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Open a handle to the shared texture
      status = device1->OpenSharedResource1(img.encoder_texture_handle, __uuidof(ID3D11Texture2D), (void **) &img_ctx.encoder_texture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to open shared image texture [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Get the keyed mutex to synchronize with the capture code
      status = img_ctx.encoder_texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &img_ctx.encoder_mutex);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to query IDXGIKeyedMutex [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Create the SRV for the encoder texture
      status = device->CreateShaderResourceView(img_ctx.encoder_texture.get(), nullptr, &img_ctx.encoder_input_res);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create shader resource view for encoding [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      img_ctx.capture_texture_p = img.capture_texture.get();

      img_ctx.img_weak = img.weak_from_this();

      return 0;
    }

    shader_res_t create_black_texture_for_rtv_clear() {
      constexpr auto width = 32;
      constexpr auto height = 32;

      D3D11_TEXTURE2D_DESC texture_desc = {};
      texture_desc.Width = width;
      texture_desc.Height = height;
      texture_desc.MipLevels = 1;
      texture_desc.ArraySize = 1;
      texture_desc.SampleDesc.Count = 1;
      texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
      texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

      std::vector<uint8_t> mem(4 * width * height, 0);
      D3D11_SUBRESOURCE_DATA texture_data = {mem.data(), 4 * width, 0};

      texture2d_t texture;
      auto status = device->CreateTexture2D(&texture_desc, &texture_data, &texture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create black texture: " << util::log_hex(status);
        return {};
      }

      shader_res_t resource_view;
      status = device->CreateShaderResourceView(texture.get(), nullptr, &resource_view);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create black texture resource view: " << util::log_hex(status);
        return {};
      }

      return resource_view;
    }

    ::video::color_t *color_p;

    buf_t subsample_offset;
    buf_t color_matrix;

    blend_t blend_disable;
    sampler_state_t sampler_linear;

    render_target_t out_Y_or_YUV_rtv;
    render_target_t out_UV_rtv;
    bool rtvs_cleared = false;

    // d3d_img_t::id -> encoder_img_ctx_t
    // These store the encoder textures for each img_t that passes through
    // convert(). We can't store them in the img_t itself because it is shared
    // amongst multiple hwdevice_t objects (and therefore multiple ID3D11Devices).
    std::map<uint32_t, encoder_img_ctx_t> img_ctx_map;

    std::shared_ptr<display_base_t> display;

    vs_t convert_Y_or_YUV_vs;
    ps_t convert_Y_or_YUV_ps;
    ps_t convert_Y_or_YUV_fp16_ps;

    vs_t convert_UV_vs;
    ps_t convert_UV_ps;
    ps_t convert_UV_fp16_ps;

    std::array<D3D11_VIEWPORT, 3> out_Y_or_YUV_viewports;
    std::array<D3D11_VIEWPORT, 3> out_Y_or_YUV_viewports_for_clear;
    D3D11_VIEWPORT out_UV_viewport;
    D3D11_VIEWPORT out_UV_viewport_for_clear;

    DXGI_FORMAT format;

    device_t device;
    device_ctx_t device_ctx;

    texture2d_t output_texture;
  };

  class d3d_avcodec_encode_device_t: public avcodec_encode_device_t {
  public:
    int init(std::shared_ptr<platf::display_t> display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      int result = base.init(display, adapter_p, pix_fmt);
      data = base.device.get();
      return result;
    }

    int convert(platf::img_t &img_base) override {
      return base.convert(img_base);
    }

    void apply_colorspace() override {
      base.apply_colorspace(colorspace);
    }

    void init_hwframes(AVHWFramesContext *frames) override {
      // We may be called with a QSV or D3D11VA context
      if (frames->device_ctx->type == AV_HWDEVICE_TYPE_D3D11VA) {
        auto d3d11_frames = (AVD3D11VAFramesContext *) frames->hwctx;

        // The encoder requires textures with D3D11_BIND_RENDER_TARGET set
        d3d11_frames->BindFlags = D3D11_BIND_RENDER_TARGET;
        d3d11_frames->MiscFlags = 0;
      }

      // We require a single texture
      frames->initial_pool_size = 1;
    }

    int prepare_to_derive_context(int hw_device_type) override {
      // QuickSync requires our device to be multithread-protected
      if (hw_device_type == AV_HWDEVICE_TYPE_QSV) {
        multithread_t mt;

        auto status = base.device->QueryInterface(IID_ID3D11Multithread, (void **) &mt);
        if (FAILED(status)) {
          BOOST_LOG(warning) << "Failed to query ID3D11Multithread interface from device [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }

        mt->SetMultithreadProtected(TRUE);
      }

      return 0;
    }

    int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) override {
      this->hwframe.reset(frame);
      this->frame = frame;

      // Populate this frame with a hardware buffer if one isn't there already
      if (!frame->buf[0]) {
        auto err = av_hwframe_get_buffer(hw_frames_ctx, frame, 0);
        if (err) {
          char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
          BOOST_LOG(error) << "Failed to get hwframe buffer: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);
          return -1;
        }
      }

      // If this is a frame from a derived context, we'll need to map it to D3D11
      ID3D11Texture2D *frame_texture;
      if (frame->format != AV_PIX_FMT_D3D11) {
        frame_t d3d11_frame {av_frame_alloc()};

        d3d11_frame->format = AV_PIX_FMT_D3D11;

        auto err = av_hwframe_map(d3d11_frame.get(), frame, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE);
        if (err) {
          char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
          BOOST_LOG(error) << "Failed to map D3D11 frame: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);
          return -1;
        }

        // Get the texture from the mapped frame
        frame_texture = (ID3D11Texture2D *) d3d11_frame->data[0];
      } else {
        // Otherwise, we can just use the texture inside the original frame
        frame_texture = (ID3D11Texture2D *) frame->data[0];
      }

      return base.init_output(frame_texture, frame->width, frame->height);
    }

  private:
    d3d_base_encode_device base;
    frame_t hwframe;
  };

  class d3d_nvenc_encode_device_t: public nvenc_encode_device_t {
  public:
    /**
     * @brief 初始化NVENC编码设备：创建D3D11设备→初始化GPU转换→创建NVENC会话
     */
    bool init_device(std::shared_ptr<platf::display_t> display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      buffer_format = nvenc::nvenc_format_from_sunshine_format(pix_fmt);
      if (buffer_format == NV_ENC_BUFFER_FORMAT_UNDEFINED) {
        BOOST_LOG(error) << "Unexpected pixel format for NvENC ["sv << from_pix_fmt(pix_fmt) << ']';
        return false;
      }

      if (base.init(display, adapter_p, pix_fmt)) {
        return false;
      }

      if (pix_fmt == pix_fmt_e::yuv444p16) {
        nvenc_d3d = std::make_unique<nvenc::nvenc_d3d11_on_cuda>(base.device.get());
      } else {
        nvenc_d3d = std::make_unique<nvenc::nvenc_d3d11_native>(base.device.get());
      }
      nvenc = nvenc_d3d.get();

      return true;
    }

    bool init_encoder(const ::video::config_t &client_config, const ::video::sunshine_colorspace_t &colorspace) override {
      if (!nvenc_d3d) {
        return false;
      }

      auto nvenc_colorspace = nvenc::nvenc_colorspace_from_sunshine_colorspace(colorspace);
      if (!nvenc_d3d->create_encoder(config::video.nv, client_config, nvenc_colorspace, buffer_format)) {
        return false;
      }

      base.apply_colorspace(colorspace);
      return base.init_output(nvenc_d3d->get_input_texture(), client_config.width, client_config.height) == 0;
    }

    int convert(platf::img_t &img_base) override {
      return base.convert(img_base);
    }

  private:
    d3d_base_encode_device base;
    std::unique_ptr<nvenc::nvenc_d3d11> nvenc_d3d;
    NV_ENC_BUFFER_FORMAT buffer_format = NV_ENC_BUFFER_FORMAT_UNDEFINED;
  };

  bool set_cursor_texture(device_t::pointer device, gpu_cursor_t &cursor, util::buffer_t<std::uint8_t> &&cursor_img, DXGI_OUTDUPL_POINTER_SHAPE_INFO &shape_info) {
    // This cursor image may not be used
    if (cursor_img.size() == 0) {
      cursor.input_res.reset();
      cursor.set_texture(0, 0, nullptr);
      return true;
    }

    D3D11_SUBRESOURCE_DATA data {
      std::begin(cursor_img),
      4 * shape_info.Width,
      0
    };

    // Create texture for cursor
    D3D11_TEXTURE2D_DESC t {};
    t.Width = shape_info.Width;
    t.Height = cursor_img.size() / data.SysMemPitch;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_IMMUTABLE;
    t.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    texture2d_t texture;
    auto status = device->CreateTexture2D(&t, &data, &texture);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create mouse texture [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }

    // Free resources before allocating on the next line.
    cursor.input_res.reset();
    status = device->CreateShaderResourceView(texture.get(), nullptr, &cursor.input_res);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create cursor shader resource view [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }

    cursor.set_texture(t.Width, t.Height, std::move(texture));
    return true;
  }

  /**
   * @brief VRAM捕获截图（DDA方式）：获取桌面帧→GPU上色彩转换→游标合成
   */
  capture_e display_ddup_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    DXGI_OUTDUPL_FRAME_INFO frame_info;

    resource_t::pointer res_p {};
    // 从DDA获取下一帧桌面图像
    auto capture_status = dup.next_frame(frame_info, timeout, &res_p);
    resource_t res {res_p};

    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    // 检查是否有鼠标更新或新帧
    const bool mouse_update_flag = frame_info.LastMouseUpdateTime.QuadPart != 0 || frame_info.PointerShapeBufferSize > 0;
    const bool frame_update_flag = frame_info.LastPresentTime.QuadPart != 0;
    const bool update_flag = mouse_update_flag || frame_update_flag;

    if (!update_flag) {
      return capture_e::timeout;
    }

    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
    if (auto qpc_displayed = std::max(frame_info.LastPresentTime.QuadPart, frame_info.LastMouseUpdateTime.QuadPart)) {
      // 将QPC时间戳转换为steady_clock时间点
      frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), qpc_displayed);
    }

    // === 处理鼠标光标形状更新 ===
    if (frame_info.PointerShapeBufferSize > 0) {
      DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info {};

      util::buffer_t<std::uint8_t> img_data {frame_info.PointerShapeBufferSize};

      UINT dummy;
      status = dup.dup->GetFramePointerShape(img_data.size(), std::begin(img_data), &dummy, &shape_info);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to get new pointer shape [0x"sv << util::hex(status).to_string_view() << ']';

        return capture_e::error;
      }

      // 生成alpha混合和XOR混合两种光标纹理
      auto alpha_cursor_img = make_cursor_alpha_image(img_data, shape_info);
      auto xor_cursor_img = make_cursor_xor_image(img_data, shape_info);

      if (!set_cursor_texture(device.get(), cursor_alpha, std::move(alpha_cursor_img), shape_info) ||
          !set_cursor_texture(device.get(), cursor_xor, std::move(xor_cursor_img), shape_info)) {
        return capture_e::error;
      }
    }

    // === 更新光标位置 ===
    if (frame_info.LastMouseUpdateTime.QuadPart) {
      cursor_alpha.set_pos(frame_info.PointerPosition.Position.x, frame_info.PointerPosition.Position.y, width, height, display_rotation, frame_info.PointerPosition.Visible);

      cursor_xor.set_pos(frame_info.PointerPosition.Position.x, frame_info.PointerPosition.Position.y, width, height, display_rotation, frame_info.PointerPosition.Visible);
    }

    // 是否需要将鼠标光标混合到图像上
    const bool blend_mouse_cursor_flag = (cursor_alpha.visible || cursor_xor.visible) && cursor_visible;

    // === 获取新帧纹理并检查分辨率/格式变化 ===
    texture2d_t src {};
    if (frame_update_flag) {
      // 从DXGI资源获取D3D11纹理对象
      status = res->QueryInterface(IID_ID3D11Texture2D, (void **) &src);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't query interface [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }

      D3D11_TEXTURE2D_DESC desc;
      src->GetDesc(&desc);

      // 检查分辨率是否变化（可能与模式切换竞争）
      if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
        BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
        return capture_e::reinit;
      }

      // 首次捕获时记录像素格式
      if (capture_format == DXGI_FORMAT_UNKNOWN) {
        capture_format = desc.Format;
        BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';
      }

      // 检查像素格式是否变化，变化则重新初始化
      if (capture_format != desc.Format) {
        BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
        return capture_e::reinit;
      }
    }

    // === 帧操作决策逻辑：根据是否有新帧和是否需要光标混合来决定如何处理 ===
    enum class lfa {
      nothing,                      // 不做任何事
      replace_surface_with_img,     // 中间表面→图像（光标消失时释放表面内存）
      replace_img_with_surface,     // 图像→中间表面（光标出现时需要可重复混合的表面）
      copy_src_to_img,              // 新帧→图像（无光标时直接复制）
      copy_src_to_surface,          // 新帧→中间表面（有光标时保存副本以便多次混合）
    };

    enum class ofa {
      forward_last_img,                    // 直接转发上次的图像（避免复制）
      copy_last_surface_and_blend_cursor,  // 从中间表面复制并混合光标
      dummy_fallback,                      // 输出黑色占位图（格式未知时）
    };

    auto last_frame_action = lfa::nothing;
    auto out_frame_action = ofa::dummy_fallback;

    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      // 像素格式未知，输出黑色占位帧
      last_frame_action = lfa::nothing;
      out_frame_action = ofa::dummy_fallback;
    } else {
      if (src) {
        // 从DDA获取到新帧...
        if (blend_mouse_cursor_flag) {
          // ...且需要混合光标：复制到中间表面以便后续无新帧时也能混合光标
          last_frame_action = lfa::copy_src_to_surface;
          out_frame_action = ofa::copy_last_surface_and_blend_cursor;
        } else {
          // ...且不需光标：直接复制到输出图像，保存共享指针以备后用
          last_frame_action = lfa::copy_src_to_img;
          out_frame_action = ofa::forward_last_img;
        }
      } else if (!std::holds_alternative<std::monostate>(last_frame_variant)) {
        // 没有新帧，但有之前的缓存帧...
        if (blend_mouse_cursor_flag) {
          // ...且需要混合光标
          if (std::holds_alternative<std::shared_ptr<platf::img_t>>(last_frame_variant)) {
            // 有上次的图像共享指针，将其替换为中间表面以便反复混合光标
            last_frame_action = lfa::replace_img_with_surface;
          }
          // 从中间表面复制并混合光标
          out_frame_action = ofa::copy_last_surface_and_blend_cursor;
        } else {
          // ...且不需光标（光标消失或禁用）
          if (std::holds_alternative<texture2d_t>(last_frame_variant)) {
            // 有中间表面，替换为图像以释放表面内存
            last_frame_action = lfa::replace_surface_with_img;
          }
          // 直接转发上次图像
          out_frame_action = ofa::forward_last_img;
        }
      }
    }

    auto create_surface = [&](texture2d_t &surface) -> bool {
      // Try to reuse the old surface if it hasn't been destroyed yet.
      if (old_surface_delayed_destruction) {
        surface.reset(old_surface_delayed_destruction.release());
        return true;
      }

      // Otherwise create a new surface.
      D3D11_TEXTURE2D_DESC t {};
      t.Width = width_before_rotation;
      t.Height = height_before_rotation;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_DEFAULT;
      t.Format = capture_format;
      t.BindFlags = 0;
      status = device->CreateTexture2D(&t, nullptr, &surface);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create frame copy texture [0x"sv << util::hex(status).to_string_view() << ']';
        return false;
      }

      return true;
    };

    auto get_locked_d3d_img = [&](std::shared_ptr<platf::img_t> &img, bool dummy = false) -> std::tuple<std::shared_ptr<img_d3d_t>, texture_lock_helper> {
      auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);

      // Finish creating the image (if it hasn't happened already),
      // also creates synchronization primitives for shared access from multiple direct3d devices.
      if (complete_img(d3d_img.get(), dummy)) {
        return {nullptr, nullptr};
      }

      // This image is shared between capture direct3d device and encoders direct3d devices,
      // we must acquire lock before doing anything to it.
      texture_lock_helper lock_helper(d3d_img->capture_mutex.get());
      if (!lock_helper.lock()) {
        BOOST_LOG(error) << "Failed to lock capture texture";
        return {nullptr, nullptr};
      }

      // Clear the blank flag now that we're ready to capture into the image
      d3d_img->blank = false;

      return {std::move(d3d_img), std::move(lock_helper)};
    };

    switch (last_frame_action) {
      case lfa::nothing:
        {
          break;
        }

      case lfa::replace_surface_with_img:
        {
          auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
          if (!p_surface) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::error;
          }

          std::shared_ptr<platf::img_t> img;
          if (!pull_free_image_cb(img)) {
            return capture_e::interrupted;
          }

          auto [d3d_img, lock] = get_locked_d3d_img(img);
          if (!d3d_img) {
            return capture_e::error;
          }

          device_ctx->CopyResource(d3d_img->capture_texture.get(), p_surface->get());

          // We delay the destruction of intermediate surface in case the mouse cursor reappears shortly.
          old_surface_delayed_destruction.reset(p_surface->release());
          old_surface_timestamp = std::chrono::steady_clock::now();

          last_frame_variant = img;
          break;
        }

      case lfa::replace_img_with_surface:
        {
          auto p_img = std::get_if<std::shared_ptr<platf::img_t>>(&last_frame_variant);
          if (!p_img) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::error;
          }
          auto [d3d_img, lock] = get_locked_d3d_img(*p_img);
          if (!d3d_img) {
            return capture_e::error;
          }

          p_img = nullptr;
          last_frame_variant = texture2d_t {};
          auto &surface = std::get<texture2d_t>(last_frame_variant);
          if (!create_surface(surface)) {
            return capture_e::error;
          }

          device_ctx->CopyResource(surface.get(), d3d_img->capture_texture.get());
          break;
        }

      case lfa::copy_src_to_img:
        {
          last_frame_variant = {};

          std::shared_ptr<platf::img_t> img;
          if (!pull_free_image_cb(img)) {
            return capture_e::interrupted;
          }

          auto [d3d_img, lock] = get_locked_d3d_img(img);
          if (!d3d_img) {
            return capture_e::error;
          }

          device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
          last_frame_variant = img;
          break;
        }

      case lfa::copy_src_to_surface:
        {
          auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
          if (!p_surface) {
            last_frame_variant = texture2d_t {};
            p_surface = std::get_if<texture2d_t>(&last_frame_variant);
            if (!create_surface(*p_surface)) {
              return capture_e::error;
            }
          }
          device_ctx->CopyResource(p_surface->get(), src.get());
          break;
        }
    }

    // === 光标混合lambda：将alpha和XOR光标纹理叠加到捕获图像上 ===
    auto blend_cursor = [&](img_d3d_t &d3d_img) {
      device_ctx->VSSetShader(cursor_vs.get(), nullptr, 0);
      device_ctx->PSSetShader(cursor_ps.get(), nullptr, 0);
      device_ctx->OMSetRenderTargets(1, &d3d_img.capture_rt, nullptr);

      // alpha混合：渲染半透明光标
      if (cursor_alpha.texture.get()) {
        // Perform an alpha blending operation
        device_ctx->OMSetBlendState(blend_alpha.get(), nullptr, 0xFFFFFFFFu);

        device_ctx->PSSetShaderResources(0, 1, &cursor_alpha.input_res);
        device_ctx->RSSetViewports(1, &cursor_alpha.cursor_view);
        device_ctx->Draw(3, 0);
      }

      // XOR混合：渲染反色光标（单色/掩码光标的反转部分）
      if (cursor_xor.texture.get()) {
        // Perform an invert blending without touching alpha values
        device_ctx->OMSetBlendState(blend_invert.get(), nullptr, 0x00FFFFFFu);

        device_ctx->PSSetShaderResources(0, 1, &cursor_xor.input_res);
        device_ctx->RSSetViewports(1, &cursor_xor.cursor_view);
        device_ctx->Draw(3, 0);
      }

      device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);

      ID3D11RenderTargetView *emptyRenderTarget = nullptr;
      device_ctx->OMSetRenderTargets(1, &emptyRenderTarget, nullptr);
      device_ctx->RSSetViewports(0, nullptr);
      ID3D11ShaderResourceView *emptyShaderResourceView = nullptr;
      device_ctx->PSSetShaderResources(0, 1, &emptyShaderResourceView);
    };

    switch (out_frame_action) {
      case ofa::forward_last_img:
        {
          auto p_img = std::get_if<std::shared_ptr<platf::img_t>>(&last_frame_variant);
          if (!p_img) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::error;
          }
          img_out = *p_img;
          break;
        }

      case ofa::copy_last_surface_and_blend_cursor:
        {
          auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
          if (!p_surface) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::error;
          }
          if (!blend_mouse_cursor_flag) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::error;
          }

          if (!pull_free_image_cb(img_out)) {
            return capture_e::interrupted;
          }

          auto [d3d_img, lock] = get_locked_d3d_img(img_out);
          if (!d3d_img) {
            return capture_e::error;
          }

          device_ctx->CopyResource(d3d_img->capture_texture.get(), p_surface->get());
          blend_cursor(*d3d_img);
          break;
        }

      case ofa::dummy_fallback:
        {
          if (!pull_free_image_cb(img_out)) {
            return capture_e::interrupted;
          }

          // Clear the image if it has been used as a dummy.
          // It can have the mouse cursor blended onto it.
          auto old_d3d_img = (img_d3d_t *) img_out.get();
          bool reclear_dummy = !old_d3d_img->blank && old_d3d_img->capture_texture;

          auto [d3d_img, lock] = get_locked_d3d_img(img_out, true);
          if (!d3d_img) {
            return capture_e::error;
          }

          if (reclear_dummy) {
            const float rgb_black[] = {0.0f, 0.0f, 0.0f, 0.0f};
            device_ctx->ClearRenderTargetView(d3d_img->capture_rt.get(), rgb_black);
          }

          if (blend_mouse_cursor_flag) {
            blend_cursor(*d3d_img);
          }

          break;
        }
    }

    // Perform delayed destruction of the unused surface if the time is due.
    if (old_surface_delayed_destruction && old_surface_timestamp + 10s < std::chrono::steady_clock::now()) {
      old_surface_delayed_destruction.reset();
    }

    if (img_out) {
      img_out->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_ddup_vram_t::release_snapshot() {
    return dup.release_frame();
  }

  int display_ddup_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    // 先初始化基类（D3D设备、输出枚举）和DDA复制接口
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    // 创建线性纹理采样器（用于光标纹理缩放）
    D3D11_SAMPLER_DESC sampler_desc {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    auto status = device->CreateSamplerState(&sampler_desc, &sampler_linear);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create point sampler state [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // 创建光标顶点着色器（处理光标位置和旋转）
    status = device->CreateVertexShader(cursor_vs_hlsl->GetBufferPointer(), cursor_vs_hlsl->GetBufferSize(), nullptr, &cursor_vs);
    if (status) {
      BOOST_LOG(error) << "Failed to create scene vertex shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    {
      // 创建显示旋转常量缓冲区，供光标顶点着色器补偿屏幕旋转
      int32_t rotation_modifier = display_rotation == DXGI_MODE_ROTATION_UNSPECIFIED ? 0 : display_rotation - 1;
      int32_t rotation_data[16 / sizeof(int32_t)] {rotation_modifier};  // aligned to 16-byte
      auto rotation = make_buffer(device.get(), rotation_data);
      if (!rotation) {
        BOOST_LOG(error) << "Failed to create display rotation vertex constant buffer";
        return -1;
      }
      device_ctx->VSSetConstantBuffers(2, 1, &rotation);
    }

    if (config.dynamicRange && is_hdr()) {
      // HDR模式下使用白点归一化像素着色器，将scRGB白色映射到用户定义的SDR白亮度
      status = device->CreatePixelShader(cursor_ps_normalize_white_hlsl->GetBufferPointer(), cursor_ps_normalize_white_hlsl->GetBufferSize(), nullptr, &cursor_ps);
      if (status) {
        BOOST_LOG(error) << "Failed to create cursor blending (normalized white) pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // 目标SDR白色设为300nit（理想情况下应获取用户SDR白点设置，但Win32无此API）
      // Use a 300 nit target for the mouse cursor. We should really get
      // the user's SDR white level in nits, but there is no API that
      // provides that information to Win32 apps.
      float white_multiplier_data[16 / sizeof(float)] {300.0f / 80.f};  // aligned to 16-byte
      auto white_multiplier = make_buffer(device.get(), white_multiplier_data);
      if (!white_multiplier) {
        BOOST_LOG(warning) << "Failed to create cursor blending (normalized white) white multiplier constant buffer";
        return -1;
      }

      device_ctx->PSSetConstantBuffers(1, 1, &white_multiplier);
    } else {
      status = device->CreatePixelShader(cursor_ps_hlsl->GetBufferPointer(), cursor_ps_hlsl->GetBufferSize(), nullptr, &cursor_ps);
      if (status) {
        BOOST_LOG(error) << "Failed to create cursor blending pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    }

    // 创建alpha混合、反色混合、禁用混合三种混合状态（分别用于普通光标、反色光标、色彩转换）
    blend_alpha = make_blend(device.get(), true, false);
    blend_invert = make_blend(device.get(), true, true);
    blend_disable = make_blend(device.get(), false, false);

    if (!blend_disable || !blend_alpha || !blend_invert) {
      return -1;
    }

    // 设置捕获端渲染管线初始状态
    device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);
    device_ctx->PSSetSamplers(0, 1, &sampler_linear);
    device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    return 0;
  }

  /**
   * Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   * @brief WGC方式VRAM截图：从图形捕获API获取帧并复制到共享纹理
   * @param pull_free_image_cb call this to get a new free image from the video subsystem.
   * @param img_out the captured frame is returned here
   * @param timeout how long to wait for the next frame
   * @param cursor_visible
   */
  capture_e display_wgc_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    texture2d_t src;
    uint64_t frame_qpc;
    // WGC API可以原生控制光标可见性（与DDA不同，无需手动混合）
    dup.set_cursor_visible(cursor_visible);
    // 获取下一帧截图和QPC时间戳
    auto capture_status = dup.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    // 将QPC时间戳转换为steady_clock时间点
    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // 检查分辨率是否变化（与模式切换竞争）
    if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
      BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }

    // 检查捕获格式是否变化
    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    // 从空闲池获取一个图像对象
    std::shared_ptr<platf::img_t> img;
    if (!pull_free_image_cb(img)) {
      return capture_e::interrupted;
    }

    auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);
    d3d_img->blank = false;  // 标记图像已有内容
    // 完成图像初始化（创建纹理/互斥/共享句柄），然后加锁并复制帧数据
    if (complete_img(d3d_img.get(), false) == 0) {
      texture_lock_helper lock_helper(d3d_img->capture_mutex.get());
      if (lock_helper.lock()) {
        device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
      } else {
        BOOST_LOG(error) << "Failed to lock capture texture";
        return capture_e::error;
      }
    } else {
      return capture_e::error;
    }
    img_out = img;
    if (img_out) {
      img_out->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_wgc_vram_t::release_snapshot() {
    return dup.release_frame();
  }

  int display_wgc_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    return 0;
  }

  std::shared_ptr<platf::img_t> display_vram_t::alloc_img() {
    auto img = std::make_shared<img_d3d_t>();

    // 初始化与格式无关的基本字段（尺寸用旋转前的值，分配ID用于编码器端上下文跟踪）
    img->width = width_before_rotation;
    img->height = height_before_rotation;
    img->id = next_image_id++;
    img->blank = true;

    return img;
  }

  // 不能使用ID3D11DeviceContext，因为编码线程可能并发调用此函数
  int display_vram_t::complete_img(platf::img_t *img_base, bool dummy) {
    auto img = (img_d3d_t *) img_base;

    // 如果已有捕获纹理且dummy状态未变，无需重新创建
    if (img->capture_texture && img->dummy == dummy) {
      return 0;
    }

    // 非dummy图像必须已知捕获格式（等待第一帧DDA/WGC回报格式）
    if (!dummy && capture_format == DXGI_FORMAT_UNKNOWN) {
      BOOST_LOG(error) << "display_vram_t::complete_img() called with unknown capture format!";
      return -1;
    }

    // 重置图像资源（可能从dummy切换为真实图像）
    img->capture_texture.reset();
    img->capture_rt.reset();
    img->capture_mutex.reset();
    img->data = nullptr;
    if (img->encoder_texture_handle) {
      CloseHandle(img->encoder_texture_handle);
      img->encoder_texture_handle = nullptr;
    }

    // 初始化格式相关字段
    img->pixel_pitch = get_pixel_pitch();
    img->row_pitch = img->pixel_pitch * img->width;
    img->dummy = dummy;
    // dummy用BGRA8格式（黑色占位图），真实图像用实际捕获格式
    img->format = (capture_format == DXGI_FORMAT_UNKNOWN) ? DXGI_FORMAT_B8G8R8A8_UNORM : capture_format;

    // 创建共享D3D11纹理：支持SRV+RTV绑定，并启用KeyedMutex跨设备共享
    D3D11_TEXTURE2D_DESC t {};
    t.Width = img->width;
    t.Height = img->height;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.Format = img->format;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    // SHARED_NTHANDLE + SHARED_KEYEDMUTEX 启用跨D3D设备共享和同步
    t.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    auto status = device->CreateTexture2D(&t, nullptr, &img->capture_texture);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create img buf texture [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // 创建渲染目标视图（用于光标混合绘制）
    status = device->CreateRenderTargetView(img->capture_texture.get(), nullptr, &img->capture_rt);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create render target view [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // 获取KeyedMutex用于捕获端和编码端之间的同步
    // Get the keyed mutex to synchronize with the encoding code
    status = img->capture_texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &img->capture_mutex);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIKeyedMutex [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    resource1_t resource;
    status = img->capture_texture->QueryInterface(__uuidof(IDXGIResource1), (void **) &resource);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIResource1 [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // 创建NT共享句柄，编码器D3D设备通过此句柄打开同一纹理
    // Create a handle for the encoder device to use to open this texture
    status = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &img->encoder_texture_handle);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create shared texture handle [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // data指向纹理对象（不是内存映射，仅作为标识用于编码器端AVFrame）
    img->data = (std::uint8_t *) img->capture_texture.get();

    return 0;
  }

  // This cannot use ID3D11DeviceContext because it can be called concurrently by the encoding thread
  /**
   * @memberof platf::dxgi::display_vram_t
   */
  int display_vram_t::dummy_img(platf::img_t *img_base) {
    return complete_img(img_base, true);
  }

  std::vector<DXGI_FORMAT> display_vram_t::get_supported_capture_formats() {
    return {
      // scRGB FP16 is the ideal format for Wide Color Gamut and Advanced Color
      // displays (both SDR and HDR). This format uses linear gamma, so we will
      // use a linear->PQ shader for HDR and a linear->sRGB shader for SDR.
      DXGI_FORMAT_R16G16B16A16_FLOAT,

      // DXGI_FORMAT_R10G10B10A2_UNORM seems like it might give us frames already
      // converted to SMPTE 2084 PQ, however it seems to actually just clamp the
      // scRGB FP16 values that DWM is using when the desktop format is scRGB FP16.
      //
      // If there is a case where the desktop format is really SMPTE 2084 PQ, it
      // might make sense to support capturing it without conversion to scRGB,
      // but we avoid it for now.

      // We include the 8-bit modes too for when the display is in SDR mode,
      // while the client stream is HDR-capable. These UNORM formats can
      // use our normal pixel shaders that expect sRGB input.
      DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_B8G8R8X8_UNORM,
      DXGI_FORMAT_R8G8B8A8_UNORM,
    };
  }

  /**
   * @brief Check that a given codec is supported by the display device.
   * @param name The FFmpeg codec name (or similar for non-FFmpeg codecs).
   * @param config The codec configuration.
   * @return `true` if supported, `false` otherwise.
   */
  bool display_vram_t::is_codec_supported(std::string_view name, const ::video::config_t &config) {
    // 获取GPU厂商ID，根据厂商匹配对应的硬件编码器
    DXGI_ADAPTER_DESC adapter_desc;
    adapter->GetDesc(&adapter_desc);

    if (adapter_desc.VendorId == 0x1002) {  // AMD显卡
      // AMD只支持AMF编码器
      if (!boost::algorithm::ends_with(name, "_amf")) {
        return false;
      }

      // 检查AMF运行时版本，旧版本可能崩溃或不支持某些编码模式
      // Perform AMF version checks if we're using an AMD GPU. This check is placed in display_vram_t
      // to avoid hitting the display_ram_t path which uses software encoding and doesn't touch AMF.
      HMODULE amfrt = LoadLibraryW(AMF_DLL_NAME);
      if (amfrt) {
        auto unload_amfrt = util::fail_guard([amfrt]() {
          FreeLibrary(amfrt);
        });

        auto fnAMFQueryVersion = (AMFQueryVersion_Fn) GetProcAddress(amfrt, AMF_QUERY_VERSION_FUNCTION_NAME);
        if (fnAMFQueryVersion) {
          amf_uint64 version;
          auto result = fnAMFQueryVersion(&version);
          if (result == AMF_OK) {
            if (config.videoFormat == 2 && version < AMF_MAKE_FULL_VERSION(1, 4, 30, 0)) {
              // AMF 1.4.30新增AV1超低延迟模式，对应驱动版本23.5.2+
              // AMF 1.4.30 adds ultra low latency mode for AV1. Don't use AV1 on earlier versions.
              // This corresponds to driver version 23.5.2 (23.10.01.45) or newer.
              BOOST_LOG(warning) << "AV1 encoding is disabled on AMF version "sv
                                 << AMF_GET_MAJOR_VERSION(version) << '.'
                                 << AMF_GET_MINOR_VERSION(version) << '.'
                                 << AMF_GET_SUBMINOR_VERSION(version) << '.'
                                 << AMF_GET_BUILD_VERSION(version);
              BOOST_LOG(warning) << "If your AMD GPU supports AV1 encoding, update your graphics drivers!"sv;
              return false;
            } else if (config.dynamicRange && version < AMF_MAKE_FULL_VERSION(1, 4, 23, 0)) {
              // AMF 1.4.23引入HEVC Main10编码，旧版本输入P010表面可能崩溃
              // Older versions of the AMD AMF runtime can crash when fed P010 surfaces.
              // Fail if AMF version is below 1.4.23 where HEVC Main10 encoding was introduced.
              // AMF 1.4.23 corresponds to driver version 21.12.1 (21.40.11.03) or newer.
              BOOST_LOG(warning) << "HDR encoding is disabled on AMF version "sv
                                 << AMF_GET_MAJOR_VERSION(version) << '.'
                                 << AMF_GET_MINOR_VERSION(version) << '.'
                                 << AMF_GET_SUBMINOR_VERSION(version) << '.'
                                 << AMF_GET_BUILD_VERSION(version);
              BOOST_LOG(warning) << "If your AMD GPU supports HEVC Main10 encoding, update your graphics drivers!"sv;
              return false;
            }
          } else {
            BOOST_LOG(warning) << "AMFQueryVersion() failed: "sv << result;
          }
        } else {
          BOOST_LOG(warning) << "AMF DLL missing export: "sv << AMF_QUERY_VERSION_FUNCTION_NAME;
        }
      } else {
        BOOST_LOG(warning) << "Detected AMD GPU but AMF failed to load"sv;
      }
    } else if (adapter_desc.VendorId == 0x8086) {  // Intel显卡
      // Intel只支持QSV编码器
      if (!boost::algorithm::ends_with(name, "_qsv")) {
        return false;
      }
      if (config.chromaSamplingType == 1) {
        if (config.videoFormat == 0 || config.videoFormat == 2) {
          // QSV不支持H.264或AV1的4:4:4色度采样
          return false;
        }
        // TODO: Blacklist HEVC 4:4:4 based on adapter model
      }
    } else if (adapter_desc.VendorId == 0x10de) {  // Nvidia显卡
      // Nvidia只支持NVENC编码器
      if (!boost::algorithm::ends_with(name, "_nvenc")) {
        return false;
      }
    } else if (adapter_desc.VendorId == 0x4D4F4351 ||  // 高通 (QCOM的ASCII反转)
               adapter_desc.VendorId == 0x5143) {  // 高通备用ID
      // 高通只支持MediaFoundation编码器
      if (!boost::algorithm::ends_with(name, "_mf")) {
        return false;
      }
    } else {
      BOOST_LOG(warning) << "Unknown GPU vendor ID: " << util::hex(adapter_desc.VendorId).to_string_view();
    }

    return true;
  }

  std::unique_ptr<avcodec_encode_device_t> display_vram_t::make_avcodec_encode_device(pix_fmt_e pix_fmt) {
    auto device = std::make_unique<d3d_avcodec_encode_device_t>();
    if (device->init(shared_from_this(), adapter.get(), pix_fmt) != 0) {
      return nullptr;
    }
    return device;
  }

  std::unique_ptr<nvenc_encode_device_t> display_vram_t::make_nvenc_encode_device(pix_fmt_e pix_fmt) {
    auto device = std::make_unique<d3d_nvenc_encode_device_t>();
    if (!device->init_device(shared_from_this(), adapter.get(), pix_fmt)) {
      return nullptr;
    }
    return device;
  }

  /**
   * @brief 编译所有GPU着色器（色彩转换、游标混合等）
   */
  int init() {
    BOOST_LOG(info) << "Compiling shaders..."sv;

#define compile_vertex_shader_helper(x) \
  if (!(x##_hlsl = compile_vertex_shader(SUNSHINE_SHADERS_DIR "/" #x ".hlsl"))) \
    return -1;
#define compile_pixel_shader_helper(x) \
  if (!(x##_hlsl = compile_pixel_shader(SUNSHINE_SHADERS_DIR "/" #x ".hlsl"))) \
    return -1;

    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_perceptual_quantizer);
    compile_vertex_shader_helper(convert_yuv420_packed_uv_type0_vs);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer);
    compile_vertex_shader_helper(convert_yuv420_packed_uv_type0s_vs);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps_perceptual_quantizer);
    compile_vertex_shader_helper(convert_yuv420_planar_y_vs);
    compile_pixel_shader_helper(convert_yuv444_packed_ayuv_ps);
    compile_pixel_shader_helper(convert_yuv444_packed_ayuv_ps_linear);
    compile_vertex_shader_helper(convert_yuv444_packed_vs);
    compile_pixel_shader_helper(convert_yuv444_planar_ps);
    compile_pixel_shader_helper(convert_yuv444_planar_ps_linear);
    compile_pixel_shader_helper(convert_yuv444_planar_ps_perceptual_quantizer);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps_linear);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps_perceptual_quantizer);
    compile_vertex_shader_helper(convert_yuv444_planar_vs);
    compile_pixel_shader_helper(cursor_ps);
    compile_pixel_shader_helper(cursor_ps_normalize_white);
    compile_vertex_shader_helper(cursor_vs);

    BOOST_LOG(info) << "Compiled shaders"sv;

#undef compile_vertex_shader_helper
#undef compile_pixel_shader_helper

    return 0;
  }
}  // namespace platf::dxgi
