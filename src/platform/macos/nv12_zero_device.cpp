/**
 * @file src/platform/macos/nv12_zero_device.cpp
 * @brief macOS NV12零拷贝设备实现。将CoreVideo像素缓冲区直接映射到FFmpeg帧。
 */
// standard includes
#include <utility>

// local includes
#include "src/platform/macos/av_img_t.h"
#include "src/platform/macos/nv12_zero_device.h"
#include "src/video.h"

extern "C" {
#include "libavutil/imgutils.h"
}

namespace platf {

  /**
   * @brief 释放AVFrame帧内存。
   *
   * 调用FFmpeg的av_frame_free释放帧资源。
   * @param frame AVFrame指针。
   */
  void free_frame(AVFrame *frame) {
    av_frame_free(&frame);
  }

  /**
   * @brief 释放CVPixelBuffer内存。
   *
   * 用于释放macOS下的CVPixelBuffer对象。
   * @param opaque 未使用。
   * @param data CVPixelBuffer指针。
   */
  void free_buffer(void *opaque, uint8_t *data) {
    CVPixelBufferRelease((CVPixelBufferRef) data);
  }

  util::safe_ptr<AVFrame, free_frame> av_frame;

  int nv12_zero_device::convert(platf::img_t &img) {
    auto *av_img = (av_img_t *) &img;

    // Release any existing CVPixelBuffer previously retained for encoding
    av_buffer_unref(&av_frame->buf[0]);

    // Attach an AVBufferRef to this frame which will retain ownership of the CVPixelBuffer
    // until av_buffer_unref() is called (above) or the frame is freed with av_frame_free().
    //
    // The presence of the AVBufferRef allows FFmpeg to simply add a reference to the buffer
    // rather than having to perform a deep copy of the data buffers in avcodec_send_frame().
    av_frame->buf[0] = av_buffer_create((uint8_t *) CFRetain(av_img->pixel_buffer->buf), 0, free_buffer, nullptr, 0);

    // Place a CVPixelBufferRef at data[3] as required by AV_PIX_FMT_VIDEOTOOLBOX
    av_frame->data[3] = (uint8_t *) av_img->pixel_buffer->buf;

    return 0;
  }

  int nv12_zero_device::set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) {
    this->frame = frame;

    av_frame.reset(frame);

    resolution_fn(this->display, frame->width, frame->height);

    return 0;
  }

  int nv12_zero_device::init(void *display, pix_fmt_e pix_fmt, resolution_fn_t resolution_fn, const pixel_format_fn_t &pixel_format_fn) {
    pixel_format_fn(display, pix_fmt == pix_fmt_e::nv12 ? kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);

    this->display = display;
    this->resolution_fn = std::move(resolution_fn);

    // we never use this pointer, but its existence is checked/used
    // by the platform independent code
    data = this;

    return 0;
  }

}  // namespace platf
