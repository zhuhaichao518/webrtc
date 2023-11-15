// Copyright (C) <2023> CloudPlayPlus
#ifndef NATIVE_HANDLE_BUFFER_H_
#define NATIVE_HANDLE_BUFFER_H_
#include "api/scoped_refptr.h"
#include "api/video/video_frame_buffer.h"
#include "rtc_base/checks.h"

namespace webrtc {
using namespace webrtc;
class NativeHandleBuffer : public VideoFrameBuffer {
 public:
  NativeHandleBuffer(void* native_handle, int width, int height)
      : native_handle_(native_handle), width_(width), height_(height) {}
  Type type() const override { return Type::kNative; }
  int width() const override { return width_; }
  int height() const override { return height_; }
  rtc::scoped_refptr<I420BufferInterface> ToI420() override {
    return nullptr;
  }

  void* GetNative() const override{ return native_handle_; }

 private:
  void* native_handle_;
  const int width_;
  const int height_;
};
}  // namespace base
#endif  // NATIVE_HANDLE_BUFFER_H_
