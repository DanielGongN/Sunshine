/**
 * @file src/nvenc/nvenc_d3d11.cpp
 * @brief Direct3D11 NVENC编码器抽象基类实现。
 *        包括异步事件创建、nvEncodeAPI64.dll加载、API函数指针初始化、异步等待。
 */
// 本地头文件
#include "src/logging.h"  // 日志系统

#ifdef _WIN32
  #include "nvenc_d3d11.h"

namespace nvenc {

  nvenc_d3d11::nvenc_d3d11(NV_ENC_DEVICE_TYPE device_type):
      nvenc_base(device_type) {
    async_event_handle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  }

  nvenc_d3d11::~nvenc_d3d11() {
    if (dll) {
      FreeLibrary(dll);
      dll = nullptr;
    }
    if (async_event_handle) {
      CloseHandle(async_event_handle);
    }
  }

  /**
   * @brief 加载nvEncodeAPI DLL并初始化NvEncode函数指针
   */
  bool nvenc_d3d11::init_library() {
    if (dll) {
      return true;
    }

  #ifdef _WIN64
    constexpr auto dll_name = "nvEncodeAPI64.dll";
  #else
    constexpr auto dll_name = "nvEncodeAPI.dll";
  #endif

    if ((dll = LoadLibraryEx(dll_name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))) {
      if (auto create_instance = (decltype(NvEncodeAPICreateInstance) *) GetProcAddress(dll, "NvEncodeAPICreateInstance")) {
        auto new_nvenc = std::make_unique<NV_ENCODE_API_FUNCTION_LIST>();
        new_nvenc->version = min_struct_version(NV_ENCODE_API_FUNCTION_LIST_VER);
        if (nvenc_failed(create_instance(new_nvenc.get()))) {
          BOOST_LOG(error) << "NvEnc: NvEncodeAPICreateInstance() failed: " << last_nvenc_error_string;
        } else {
          nvenc = std::move(new_nvenc);
          return true;
        }
      } else {
        BOOST_LOG(error) << "NvEnc: No NvEncodeAPICreateInstance() in " << dll_name;
      }
    } else {
      BOOST_LOG(debug) << "NvEnc: Couldn't load NvEnc library " << dll_name;
    }

    if (dll) {
      FreeLibrary(dll);
      dll = nullptr;
    }

    return false;
  }

  /**
   * @brief 等待NVENC异步编码完成事件
   */
  bool nvenc_d3d11::wait_for_async_event(uint32_t timeout_ms) {
    return WaitForSingleObject(async_event_handle, timeout_ms) == WAIT_OBJECT_0;
  }

}  // namespace nvenc
#endif
