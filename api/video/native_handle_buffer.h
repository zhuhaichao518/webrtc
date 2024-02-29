// Copyright (C) <2023> CloudPlayPlus
#ifndef NATIVE_HANDLE_BUFFER_H_
#define NATIVE_HANDLE_BUFFER_H_
#include "api/scoped_refptr.h"
#include "api/video/video_frame_buffer.h"
#include "rtc_base/checks.h"

#ifdef WEBRTC_WIN
#include <d3d11.h>
#include <wrl/client.h>
#endif

namespace webrtc {
using namespace webrtc;
class NativeHandleBuffer : public VideoFrameBuffer {
 public:
  //TODO(Haichao): support MacOS?
#ifdef WEBRTC_WIN
  NativeHandleBuffer(/*Microsoft::WRL::ComPtr<ID3D11Device> device, 
   Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,*/rtc::scoped_refptr<NativeImage> native_image, int width, int height)
      : native_image_(native_image), width_(width), height_(height) {}
#endif
#ifdef WEBRTC_MAC
//IOSurface?
  NativeHandleBuffer(int width, int height)
      : width_(width), height_(height) {}
#endif
  Type type() const override { return Type::kNative; }
  int width() const override { return width_; }
  int height() const override { return height_; }
  rtc::scoped_refptr<I420BufferInterface> ToI420() override {
    return nullptr;
  }

//#ifdef WEBRTC_WIN
  rtc::scoped_refptr<NativeImage> GetNativeImage() override {return native_image_;}
  //Microsoft::WRL::ComPtr<ID3D11Device> GetDevice() override {return device_; }
  //Microsoft::WRL::ComPtr<ID3D11Texture2D> GetTexture() override {return texture_; }
//#endif

 private:
//#ifdef WEBRTC_WIN
  rtc::scoped_refptr<NativeImage> native_image_;
  //Microsoft::WRL::ComPtr<ID3D11Device> device_;
  //Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
//#endif
  const int width_;
  const int height_;
};
}  // namespace base
#endif  // NATIVE_HANDLE_BUFFER_H_
