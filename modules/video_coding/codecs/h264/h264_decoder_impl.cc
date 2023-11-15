/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 *
 */

// Everything declared/defined in this header is only required when WebRTC is
// build with H264 support, please do not move anything out of the
// #ifdef unless needed and tested.
#ifdef WEBRTC_USE_H264

#include "modules/video_coding/codecs/h264/h264_decoder_impl.h"

#include <algorithm>
#include <limits>
#include <memory>

extern "C" {
#include "third_party/ffmpeg/libavcodec/avcodec.h"
#include "third_party/ffmpeg/libavformat/avformat.h"
#include "third_party/ffmpeg/libavutil/imgutils.h"
#include "third_party/ffmpeg/libavutil/hwcontext_d3d11va.h"
#include "third_party/ffmpeg/libavutil/hwcontext.h"
}  // extern "C"

#include "api/video/color_space.h"
#include "api/video/i010_buffer.h"
#include "api/video/i420_buffer.h"
#include "api/video/native_handle_buffer.h"
#include "common_video/include/video_frame_buffer.h"
#include "modules/video_coding/codecs/h264/h264_color_space.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/metrics.h"

namespace webrtc {

namespace {

constexpr std::array<AVPixelFormat, 11> kPixelFormatsSupported = {
    AV_PIX_FMT_YUV420P,     AV_PIX_FMT_YUV422P,     AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUVJ420P,    AV_PIX_FMT_YUVJ422P,    AV_PIX_FMT_YUVJ444P,
    AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_YUV444P10LE,
    AV_PIX_FMT_NV12,        AV_PIX_FMT_D3D11};
const size_t kYPlaneIndex = 0;
const size_t kUPlaneIndex = 1;
const size_t kVPlaneIndex = 2;

// Used by histograms. Values of entries should not be changed.
enum H264DecoderImplEvent {
  kH264DecoderEventInit = 0,
  kH264DecoderEventError = 1,
  kH264DecoderEventMax = 16,
};

struct ScopedPtrAVFreePacket {
  void operator()(AVPacket* packet) { av_packet_free(&packet); }
};
typedef std::unique_ptr<AVPacket, ScopedPtrAVFreePacket> ScopedAVPacket;

ScopedAVPacket MakeScopedAVPacket() {
  ScopedAVPacket packet(av_packet_alloc());
  return packet;
}

}  // namespace

int H264DecoderImpl::AVGetBuffer2(AVCodecContext* context,
                                  AVFrame* av_frame,
                                  int flags) {
  // Set in `Configure`.
  H264DecoderImpl* decoder = static_cast<H264DecoderImpl*>(context->opaque);
  // DCHECK values set in `Configure`.
  RTC_DCHECK(decoder);
  // Necessary capability to be allowed to provide our own buffers.
  RTC_DCHECK(context->codec->capabilities | AV_CODEC_CAP_DR1);

  auto pixelFormatSupported = std::find_if(
      kPixelFormatsSupported.begin(), kPixelFormatsSupported.end(),
      [context](AVPixelFormat format) { return context->pix_fmt == format; });

  RTC_CHECK(pixelFormatSupported != kPixelFormatsSupported.end());

  // `av_frame->width` and `av_frame->height` are set by FFmpeg. These are the
  // actual image's dimensions and may be different from `context->width` and
  // `context->coded_width` due to reordering.
  int width = av_frame->width;
  int height = av_frame->height;
  // See `lowres`, if used the decoder scales the image by 1/2^(lowres). This
  // has implications on which resolutions are valid, but we don't use it.
  RTC_CHECK_EQ(context->lowres, 0);
  // Adjust the `width` and `height` to values acceptable by the decoder.
  // Without this, FFmpeg may overflow the buffer. If modified, `width` and/or
  // `height` are larger than the actual image and the image has to be cropped
  // (top-left corner) after decoding to avoid visible borders to the right and
  // bottom of the actual image.
  avcodec_align_dimensions(context, &width, &height);

  RTC_CHECK_GE(width, 0);
  RTC_CHECK_GE(height, 0);
  int ret = av_image_check_size(static_cast<unsigned int>(width),
                                static_cast<unsigned int>(height), 0, nullptr);
  if (ret < 0) {
    RTC_LOG(LS_ERROR) << "Invalid picture size " << width << "x" << height;
    decoder->ReportError();
    return ret;
  }

  // The video frame is stored in `frame_buffer`. `av_frame` is FFmpeg's version
  // of a video frame and will be set up to reference `frame_buffer`'s data.

  // FFmpeg expects the initial allocation to be zero-initialized according to
  // http://crbug.com/390941. Our pool is set up to zero-initialize new buffers.
  // TODO(https://crbug.com/390941): Delete that feature from the video pool,
  // instead add an explicit call to InitializeData here.
  rtc::scoped_refptr<ChromaBuffer> frame_buffer;
  rtc::scoped_refptr<I444Buffer> i444_buffer;
  rtc::scoped_refptr<I420Buffer> i420_buffer;
  rtc::scoped_refptr<I422Buffer> i422_buffer;
  rtc::scoped_refptr<I010Buffer> i010_buffer;
  rtc::scoped_refptr<I210Buffer> i210_buffer;
  rtc::scoped_refptr<I410Buffer> i410_buffer;
  rtc::scoped_refptr<NV12Buffer> nv12_buffer;
  int bytes_per_pixel = 1;
  switch (context->pix_fmt) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
      i420_buffer =
          decoder->ffmpeg_buffer_pool_.CreateI420Buffer(width, height);
      // Set `av_frame` members as required by FFmpeg.
      av_frame->data[kYPlaneIndex] = i420_buffer->MutableDataY();
      av_frame->linesize[kYPlaneIndex] = i420_buffer->StrideY();
      av_frame->data[kUPlaneIndex] = i420_buffer->MutableDataU();
      av_frame->linesize[kUPlaneIndex] = i420_buffer->StrideU();
      av_frame->data[kVPlaneIndex] = i420_buffer->MutableDataV();
      av_frame->linesize[kVPlaneIndex] = i420_buffer->StrideV();
      RTC_DCHECK_EQ(av_frame->extended_data, av_frame->data);
      frame_buffer = i420_buffer;
      break;
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ444P:
      i444_buffer =
          decoder->ffmpeg_buffer_pool_.CreateI444Buffer(width, height);
      // Set `av_frame` members as required by FFmpeg.
      av_frame->data[kYPlaneIndex] = i444_buffer->MutableDataY();
      av_frame->linesize[kYPlaneIndex] = i444_buffer->StrideY();
      av_frame->data[kUPlaneIndex] = i444_buffer->MutableDataU();
      av_frame->linesize[kUPlaneIndex] = i444_buffer->StrideU();
      av_frame->data[kVPlaneIndex] = i444_buffer->MutableDataV();
      av_frame->linesize[kVPlaneIndex] = i444_buffer->StrideV();
      frame_buffer = i444_buffer;
      break;
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P:
      i422_buffer =
          decoder->ffmpeg_buffer_pool_.CreateI422Buffer(width, height);
      // Set `av_frame` members as required by FFmpeg.
      av_frame->data[kYPlaneIndex] = i422_buffer->MutableDataY();
      av_frame->linesize[kYPlaneIndex] = i422_buffer->StrideY();
      av_frame->data[kUPlaneIndex] = i422_buffer->MutableDataU();
      av_frame->linesize[kUPlaneIndex] = i422_buffer->StrideU();
      av_frame->data[kVPlaneIndex] = i422_buffer->MutableDataV();
      av_frame->linesize[kVPlaneIndex] = i422_buffer->StrideV();
      frame_buffer = i422_buffer;
      break;
    case AV_PIX_FMT_YUV420P10LE:
      i010_buffer =
          decoder->ffmpeg_buffer_pool_.CreateI010Buffer(width, height);
      // Set `av_frame` members as required by FFmpeg.
      av_frame->data[kYPlaneIndex] =
          reinterpret_cast<uint8_t*>(i010_buffer->MutableDataY());
      av_frame->linesize[kYPlaneIndex] = i010_buffer->StrideY() * 2;
      av_frame->data[kUPlaneIndex] =
          reinterpret_cast<uint8_t*>(i010_buffer->MutableDataU());
      av_frame->linesize[kUPlaneIndex] = i010_buffer->StrideU() * 2;
      av_frame->data[kVPlaneIndex] =
          reinterpret_cast<uint8_t*>(i010_buffer->MutableDataV());
      av_frame->linesize[kVPlaneIndex] = i010_buffer->StrideV() * 2;
      frame_buffer = i010_buffer;
      bytes_per_pixel = 2;
      break;
    case AV_PIX_FMT_YUV422P10LE:
      i210_buffer =
          decoder->ffmpeg_buffer_pool_.CreateI210Buffer(width, height);
      // Set `av_frame` members as required by FFmpeg.
      av_frame->data[kYPlaneIndex] =
          reinterpret_cast<uint8_t*>(i210_buffer->MutableDataY());
      av_frame->linesize[kYPlaneIndex] = i210_buffer->StrideY() * 2;
      av_frame->data[kUPlaneIndex] =
          reinterpret_cast<uint8_t*>(i210_buffer->MutableDataU());
      av_frame->linesize[kUPlaneIndex] = i210_buffer->StrideU() * 2;
      av_frame->data[kVPlaneIndex] =
          reinterpret_cast<uint8_t*>(i210_buffer->MutableDataV());
      av_frame->linesize[kVPlaneIndex] = i210_buffer->StrideV() * 2;
      frame_buffer = i210_buffer;
      bytes_per_pixel = 2;
      break;
    case AV_PIX_FMT_YUV444P10LE:
      i410_buffer =
          decoder->ffmpeg_buffer_pool_.CreateI410Buffer(width, height);
      // Set `av_frame` members as required by FFmpeg.
      av_frame->data[kYPlaneIndex] =
          reinterpret_cast<uint8_t*>(i410_buffer->MutableDataY());
      av_frame->linesize[kYPlaneIndex] = i410_buffer->StrideY() * 2;
      av_frame->data[kUPlaneIndex] =
          reinterpret_cast<uint8_t*>(i410_buffer->MutableDataU());
      av_frame->linesize[kUPlaneIndex] = i410_buffer->StrideU() * 2;
      av_frame->data[kVPlaneIndex] =
          reinterpret_cast<uint8_t*>(i410_buffer->MutableDataV());
      av_frame->linesize[kVPlaneIndex] = i410_buffer->StrideV() * 2;
      frame_buffer = i410_buffer;
      bytes_per_pixel = 2;
      break;
    case AV_PIX_FMT_NV12:
      //todo(haichao):make this correct
      nv12_buffer =
          decoder->ffmpeg_buffer_pool_.CreateNV12Buffer(width, height);
      av_frame->data[0] = nv12_buffer->MutableDataY();
      av_frame->linesize[0] = nv12_buffer->StrideY();

      av_frame->data[1] = nv12_buffer->MutableDataUV();
      av_frame->linesize[1] = nv12_buffer->StrideUV();
      //RTC_DCHECK_EQ(av_frame->linesize[1], av_frame->linesize[0] * 2);

      frame_buffer = nv12_buffer;
      break;
    default:
      RTC_LOG(LS_ERROR) << "Unsupported buffer type " << context->pix_fmt
                        << ". Check supported supported pixel formats!";
      decoder->ReportError();
      return -1;
  }

  int y_size = width * height * bytes_per_pixel;
  int uv_size = frame_buffer->ChromaWidth() * frame_buffer->ChromaHeight() *
                bytes_per_pixel;
  // DCHECK that we have a continuous buffer as is required.
  RTC_DCHECK_EQ(av_frame->data[kUPlaneIndex],
                av_frame->data[kYPlaneIndex] + y_size);
  RTC_DCHECK_EQ(av_frame->data[kVPlaneIndex],
                av_frame->data[kUPlaneIndex] + uv_size);
  int total_size = y_size + 2 * uv_size;

  av_frame->format = context->pix_fmt;
  av_frame->reordered_opaque = context->reordered_opaque;

  // Create a VideoFrame object, to keep a reference to the buffer.
  // TODO(nisse): The VideoFrame's timestamp and rotation info is not used.
  // Refactor to do not use a VideoFrame object at all.
  av_frame->buf[0] = av_buffer_create(
      av_frame->data[kYPlaneIndex], total_size, AVFreeBuffer2,
      static_cast<void*>(
          std::make_unique<VideoFrame>(VideoFrame::Builder()
                                           .set_video_frame_buffer(frame_buffer)
                                           .set_rotation(kVideoRotation_0)
                                           .set_timestamp_us(0)
                                           .build())
              .release()),
      0);
  RTC_CHECK(av_frame->buf[0]);
  return 0;
}

void H264DecoderImpl::AVFreeBuffer2(void* opaque, uint8_t* data) {
  // The buffer pool recycles the buffer used by `video_frame` when there are no
  // more references to it. `video_frame` is a thin buffer holder and is not
  // recycled.
  VideoFrame* video_frame = static_cast<VideoFrame*>(opaque);
  delete video_frame;
}

H264DecoderImpl::H264DecoderImpl()
    : ffmpeg_buffer_pool_(true),
      decoded_image_callback_(nullptr),
      has_reported_init_(false),
      has_reported_error_(false) {}

H264DecoderImpl::~H264DecoderImpl() {
  Release();
}

const uint8_t k_H264TestFrame[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x20, 0xac, 0x2b, 0x40, 0x28,
    0x02, 0xdd, 0x80, 0xb5, 0x06, 0x06, 0x06, 0xa5, 0x00, 0x00, 0x03, 0x03,
    0xe8, 0x00, 0x01, 0xd4, 0xc0, 0x8f, 0x4a, 0xa0, 0x00, 0x00, 0x00, 0x01,
    0x68, 0xee, 0x3c, 0xb0, 0x00, 0x00, 0x00, 0x01, 0x65, 0xb8, 0x02, 0x01,
    0x67, 0x25, 0x1b, 0xf4, 0xfa, 0x7d, 0x40, 0x1a, 0x78, 0xe5, 0x10, 0x52,
    0xc2, 0xee, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
    0xc6, 0xef, 0xbb, 0x81, 0x85, 0x2d, 0x47, 0xda, 0xca, 0x4c, 0x00, 0x00,
    0x03, 0x00, 0x02, 0x7b, 0xcf, 0x80, 0x00, 0x45, 0x40, 0x01, 0x8d, 0xa6,
    0x00, 0x01, 0x64, 0x00, 0x0e, 0x03, 0xc8, 0x00, 0x0e, 0x10, 0x00, 0xbd,
    0xc5, 0x00, 0x01, 0x11, 0x00, 0x0e, 0xa3, 0x80, 0x00, 0x38, 0xa0, 0x00,
    0x00, 0x01, 0x65, 0x00, 0x6e, 0x2e, 0x00, 0x83, 0x7f, 0xb4, 0x8e, 0x79,
    0xa5, 0xff, 0x84, 0x3f, 0x7f, 0x34, 0x3f, 0x97, 0x1b, 0xaf, 0x31, 0x5f,
    0x6e, 0xaa, 0xb6, 0xac, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x78, 0x36, 0x9d, 0xa4, 0x65, 0xf9, 0x1e, 0x5b, 0x3a,
    0xb0, 0x40, 0x00, 0x00, 0x03, 0x00, 0x00, 0x41, 0x80, 0x00, 0xc5, 0xc8,
    0x00, 0x00, 0xfa, 0x00, 0x03, 0x24, 0x00, 0x0e, 0x20, 0x00, 0x3f, 0x80,
    0x01, 0x32, 0x00, 0x08, 0x68, 0x00, 0x3e, 0xc0, 0x03, 0x8a, 0x00, 0x00,
    0x01, 0x65, 0x00, 0x37, 0x0b, 0x80, 0x21, 0x7f, 0xe3, 0x85, 0x1c, 0xd9,
    0xff, 0xe1, 0x1b, 0x01, 0xfa, 0xc0, 0x3e, 0x11, 0x7e, 0xfe, 0x45, 0x5c,
    0x43, 0x69, 0x31, 0x4b, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x03, 0x02, 0x24, 0xae, 0x1a, 0xbb, 0xae, 0x75, 0x9e,
    0x35, 0xae, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x03, 0x64, 0x00, 0x09,
    0x98, 0x00, 0x1e, 0xc0, 0x00, 0x64, 0x80, 0x01, 0xc4, 0x00, 0x07, 0xf0,
    0x00, 0x42, 0xf0, 0x00, 0x00, 0xe1, 0x98, 0x00, 0x05, 0x4b, 0x28, 0x00,
    0x06, 0x04, 0x00, 0x00, 0x01, 0x65, 0x00, 0x14, 0xa2, 0xe0, 0x08, 0x5f,
    0xe3, 0x85, 0x1c, 0xd9, 0xff, 0xe1, 0x1b, 0x01, 0xfa, 0xc0, 0x3e, 0x11,
    0x7e, 0xfe, 0x45, 0x5c, 0x43, 0x69, 0x31, 0x4b, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x02, 0x24, 0xb9, 0x7d,
    0xb4, 0x70, 0x4d, 0x15, 0xc5, 0x0a, 0x4e, 0x60, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x26, 0xa8, 0xb0, 0x00, 0x13, 0xcd, 0xcc, 0x00, 0x07, 0x48, 0x88,
    0x00, 0x06, 0x29, 0x69, 0x00, 0x01, 0x16, 0xac, 0x80, 0x00, 0x9e, 0x4e,
    0x80, 0x00, 0x3f, 0x84, 0x20, 0x00, 0x6f, 0x41, 0xa0, 0x00, 0x20, 0x00,
    0x02, 0x16, 0xb8, 0x00, 0x08, 0x08};

ID3D11Texture2D* CreateSharedTexture(int width, int height, ID3D11Device* device) {
    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_NV12;  // Adjust format as needed
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; // Add flags based on your requirements
    textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // Flag for shared texture

    ID3D11Texture2D* d3dTexture = nullptr;
    device->CreateTexture2D(&textureDesc, nullptr, &d3dTexture);
    return d3dTexture;
}

int H264DecoderImpl::InitializeD3D11Device() {
    /* int adapterIndex;
    HRESULT hr;

    hr = CreateDXGIFactory(__uuidof(IDXGIFactory5), (void**)&m_Factory);
    if (FAILED(hr)) {
    return hr;
    }

    IDXGIAdapter* adapter;
    hr = m_Factory->EnumAdapters(adapterIndex, &adapter);
    if (FAILED(hr)) {
    return hr;
    }
    */
    if (d3dDevice_ == nullptr) {

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr /* adapter*/, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                   nullptr, 0,
        D3D11_SDK_VERSION, &d3dDevice_, &featureLevel, &d3dDeviceContext_);
    if (hr != 0)
      return -1;
    }
    return 0;
}

int H264DecoderImpl::InitializeD3D11Texture(int width, int height){
    if (d3dTexture_!=nullptr){
      d3dTexture_->Release();
    }
    d3dTexture_ = CreateSharedTexture(width, height, d3dDevice_);
    if (d3dTexture_ == nullptr){
      return -1;
    }
    return 0;
/*
    //maybe we return a shared handle instead of a texture.
    HANDLE sharedHandle = nullptr;
    hr = sharedTexture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&sharedTexture));
    CheckResult(hr, "QueryInterface");

    hr = reinterpret_cast<IDXGIResource*>(sharedTexture)->GetSharedHandle(&sharedHandle);
    CheckResult(hr, "GetSharedHandle");
*/
}

bool H264DecoderImpl::Configure(const Settings& settings) {
  ReportInit();
  if (settings.codec_type() != kVideoCodecH264) {
    ReportError();
    return false;
  }

  // Release necessary in case of re-initializing.
  int32_t ret = Release();
  if (ret != WEBRTC_VIDEO_CODEC_OK) {
    ReportError();
    return false;
  }
  RTC_DCHECK(!av_context_);

  // Initialize AVCodecContext.
  av_context_.reset(avcodec_alloc_context3(nullptr));

  av_context_->codec_type = AVMEDIA_TYPE_VIDEO;
  av_context_->codec_id = AV_CODEC_ID_H264;
  const RenderResolution& resolution = settings.max_render_resolution();
  if (resolution.Valid()) {
    av_context_->coded_width = resolution.Width();
    av_context_->coded_height = resolution.Height();
  }
  //data in webrtc does not include this.
  av_context_->extradata = nullptr;
  av_context_->extradata_size = 0;

  // If this is ever increased, look at `av_context_->thread_safe_callbacks` and
  // make it possible to disable the thread checker in the frame buffer pool.
  av_context_->thread_count = 1;
  //software rendering
  //av_context_->thread_type = FF_THREAD_SLICE;

  av_context_->flags |= AV_CODEC_FLAG_LOW_DELAY;

  // Allow display of corrupt frames and frames missing references
  av_context_->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
  av_context_->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
  // Report decoding errors to allow us to request a key frame
  //
  // With HEVC streams, FFmpeg can drop a frame (hwaccel->start_frame() fails)
  // without telling us. Since we have an infinite GOP length, this causes
  // artifacts on screen that persist for a long time. It's easy to cause this
  // condition by using NVDEC and delaying 100 ms randomly in the render path so
  // the decoder runs out of output buffers.
  av_context_->err_recognition = AV_EF_EXPLODE;

  // Function used by FFmpeg to get buffers to store decoded frames in.
  //av_context_->get_buffer2 = AVGetBuffer2;

  // `get_buffer2` is called with the context, there `opaque` can be used to get
  // a pointer `this`.
  av_context_->opaque = this;

/*
  AVBufferRef* m_HwContext;
  AVBufferRef* hw_frames_ctx = av_hwframe_ctx_alloc(codec_ctx->hw_device_ctx);
  av_hwframe_ctx_init(hw_frames_ctx);
  frame->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);
  av_buffer_unref(&hw_frames_ctx);*/
  //todo(Haichao:Set to CUDA.)
  av_context_->pix_fmt = AV_PIX_FMT_D3D11;
  av_context_->get_format =
      [](AVCodecContext * ctx,
                 const enum AVPixelFormat* pix_fmts) -> enum AVPixelFormat {
            while (*pix_fmts != AV_PIX_FMT_NONE){
        if (*pix_fmts == AV_PIX_FMT_D3D11){return AV_PIX_FMT_D3D11;}
                   pix_fmts++;
                 }
              fprintf(stderr, "Failed to get HW surface format.\n");
             return AV_PIX_FMT_NONE;
        };

  //const AVCodec* codec = avcodec_find_decoder_by_name("h264_cuvid");
  //const AVCodec* codec = avcodec_find_decoder_by_name("h264_d3d11va2");
  const AVCodec* codec = avcodec_find_decoder(av_context_->codec_id);
        /* for (int i = 0;; i++) {
    const AVCodecHWConfig* config =
                       avcodec_get_hw_config(codec, i);
    if (!config) {
      // No remaing hwaccel options
      break;
    }

    // Initialize the hardware codec and submit a test frame if the renderer
    // needs it
    for (int i = 1; i < 2; i++) {

    }
      //return true;
    }
  }*/
 
  if (!codec) {
    // Not built with CUDA decoder
    codec = avcodec_find_decoder(av_context_->codec_id);

    if (!codec){
      // This is an indication that FFmpeg has not been initialized or it has not
      // been compiled/initialized with the correct set of codecs.
      RTC_LOG(LS_ERROR) << "FFmpeg H.264 decoder not found.";
      Release();
      ReportError();
      return false;
    }
  }
  /* //use cuda for linux
  int err = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, NULL,
                                   NULL, 0);*/
  //todo(haichao):should get available hws first.
  m_HwDeviceContext = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  /* int err = av_hwdevice_ctx_create(&hw_device_ctx,
                                        AV_HWDEVICE_TYPE_D3D11VA,
                                   NULL,
                                   NULL, 0);*/
  AVHWDeviceContext* deviceContext =
      (AVHWDeviceContext*)m_HwDeviceContext->data;
  AVD3D11VADeviceContext* d3d11vaDeviceContext =
      (AVD3D11VADeviceContext*)deviceContext->hwctx;
  InitializeD3D11Device();
  // AVHWDeviceContext takes ownership of these objects
  d3d11vaDeviceContext->device = d3dDevice_;
  d3d11vaDeviceContext->device_context = d3dDeviceContext_;

  InitializeD3D11Texture(resolution.Width(), resolution.Height());

  int err = av_hwdevice_ctx_init(m_HwDeviceContext);

  m_HwFramesContext = av_hwframe_ctx_alloc(m_HwDeviceContext);
  AVHWFramesContext* framesContext =
      (AVHWFramesContext*)m_HwFramesContext->data;

  // We require NV12 or P010 textures for our shader
  //haichao:do not need this line
  framesContext->format = AV_PIX_FMT_D3D11;
  framesContext->sw_format = //(params->videoFormat & VIDEO_FORMAT_MASK_10BIT)
                              //   ? AV_PIX_FMT_P010: 
                             AV_PIX_FMT_NV12;
  // We can have up to 16 reference frames plus a working surface?
  framesContext->initial_pool_size = 17;
  AVD3D11VAFramesContext* d3d11vaFramesContext =
      (AVD3D11VAFramesContext*)framesContext->hwctx;

  d3d11vaFramesContext->BindFlags = D3D11_BIND_DECODER;
  //d3d11vaFramesContext->texture = d3dTexture_;
  framesContext->width = resolution.Width();
  framesContext->height = resolution.Height();
  err = av_hwframe_ctx_init(m_HwFramesContext);
  //prepareDecoderContext
  if (err == 0) {
    av_context_->hw_device_ctx = av_buffer_ref(m_HwDeviceContext);
  }

  int res = avcodec_open2(av_context_.get(), codec, nullptr);
  if (res < 0) {
    // CUDA runtime not found on the machine
    codec = avcodec_find_decoder(av_context_->codec_id);
    res = avcodec_open2(av_context_.get(), codec, nullptr);
    if (res < 0) {
      RTC_LOG(LS_ERROR) << "avcodec_open2 error: " << res;
      Release();
      ReportError();
      return false;
    }
    // Function used by FFmpeg to get buffers to store decoded frames in.
    av_context_->get_buffer2 = AVGetBuffer2;
  }else{
    //is_hardware_accelerated = true;
    //err = av_hwdevice_ctx_create(&m_HwContext, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
    av_context_->get_format = [](
        AVCodecContext * ctx,
               const enum AVPixelFormat* pix_fmts) -> enum AVPixelFormat {
        const enum AVPixelFormat *p;
      H264DecoderImpl* decoder = static_cast<H264DecoderImpl*>(ctx->opaque);

        for (p = pix_fmts; *p != -1;p++){
            //should be cuda -> software in linux
          if (*pix_fmts == AV_PIX_FMT_D3D11){
              ctx->hw_frames_ctx = av_buffer_ref(decoder->m_HwFramesContext);
              return AV_PIX_FMT_D3D11;
          }
            RTC_LOG(LS_WARNING) << *pix_fmts;
            pix_fmts++;
        }
        return AV_PIX_FMT_NONE;
      };
  }

  av_frame_.reset(av_frame_alloc());

  if (absl::optional<int> buffer_pool_size = settings.buffer_pool_size()) {
    if (!ffmpeg_buffer_pool_.Resize(*buffer_pool_size)) {
      return false;
    }
  }

  AVPacket* packet = av_packet_alloc();
  packet->data = (uint8_t*)k_H264TestFrame;
  packet->size = sizeof(k_H264TestFrame);
  // Some decoders won't output on the first frame, so we'll submit
  // a few test frames if we get an EAGAIN error.
  /*
  for (int retries = 0; retries < 5; retries++) {
    // Most FFmpeg decoders process input using a "push" model.
    // We'll see those fail here if the format is not supported.
    int err = avcodec_send_packet(av_context_.get(), packet);

    // A few FFmpeg decoders (h264_mmal) process here using a "pull" model.
    // Those decoders will fail here if the format is not supported.
    err = avcodec_receive_frame(av_context_.get(), av_frame_.get());
    if (err == AVERROR(EAGAIN)) {
      // Wait a little while to let the hardware work
      Sleep(100);
    } else {
      // Done!
      break;
    }
  }*/
  av_packet_free(&packet);

  return true;
}

int32_t H264DecoderImpl::Release() {
  if (d3dDevice_){
    d3dDevice_->Release();
  }
  if (d3dDeviceContext_){
    d3dDeviceContext_->Release();
  }
  if (d3dTexture_){
    d3dTexture_->Release();
  }

  if (m_HwFramesContext != nullptr) {
    av_buffer_unref(&m_HwFramesContext);
  }

  if (m_HwDeviceContext != nullptr) {
    // This will release m_Device and m_DeviceContext too
    av_buffer_unref(&m_HwDeviceContext);
  }
  av_context_.reset();
  av_frame_.reset();
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t H264DecoderImpl::RegisterDecodeCompleteCallback(
    DecodedImageCallback* callback) {
  decoded_image_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t H264DecoderImpl::Decode(const EncodedImage& input_image,
                                bool /*missing_frames*/,
                                int64_t /*render_time_ms*/) {
  if (!IsInitialized()) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (!decoded_image_callback_) {
    RTC_LOG(LS_WARNING)
        << "Configure() has been called, but a callback function "
           "has not been set with RegisterDecodeCompleteCallback()";
    ReportError();
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (!input_image.data() || !input_image.size()) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  ScopedAVPacket packet = MakeScopedAVPacket();
  if (!packet) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  // packet.data has a non-const type, but isn't modified by
  // avcodec_send_packet.
  packet->data = const_cast<uint8_t*>(input_image.data());
  if (input_image.size() >
      static_cast<size_t>(std::numeric_limits<int>::max())) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  packet->size = static_cast<int>(input_image.size());
  int64_t frame_timestamp_us = input_image.ntp_time_ms_ * 1000;  // ms -> μs
  av_context_->reordered_opaque = frame_timestamp_us;

  int result = avcodec_send_packet(av_context_.get(), packet.get());

  if (result < 0) {
    RTC_LOG(LS_ERROR) << "avcodec_send_packet error: " << result;
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  //maybe we need to split it into a thread and handle AVERROR(EAGAIN)
  result = avcodec_receive_frame(av_context_.get(), av_frame_.get());
  if (result < 0) {
    RTC_LOG(LS_ERROR) << "avcodec_receive_frame error: " << result;
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // We don't expect reordering. Decoded frame timestamp should match
  // the input one.
  RTC_DCHECK_EQ(av_frame_->reordered_opaque, frame_timestamp_us);

  // TODO(sakal): Maybe it is possible to get QP directly from FFmpeg.
  // Haichao:Does this influence performance?
  h264_bitstream_parser_.ParseBitstream(input_image);
  absl::optional<int> qp = h264_bitstream_parser_.GetLastSliceQp();

  if (av_context_->pix_fmt == AV_PIX_FMT_D3D11) {
    if (av_frame_->width!=texture_width || av_frame_->height != texture_height){
      texture_width = av_frame_->width;
      texture_height = av_frame_->height;
      int hr = InitializeD3D11Texture(texture_width, texture_height);
      if (hr!=0){
        return WEBRTC_VIDEO_CODEC_ERROR;
      }
    }
    // Pass on color space from input frame if explicitly specified.
    const ColorSpace& color_space =
      input_image.ColorSpace() ? *input_image.ColorSpace()
                               : ExtractH264ColorSpace(av_context_.get());

    /*auto cudabuffer = reinterpret_cast<CUdeviceptr>(Frame->m_buffer);
    if (!cudabuffer)
      return result;*/
    ID3D11Texture2D* texture = (ID3D11Texture2D*)av_frame_->data[0];
    //ID3D11DeviceContext* context = (ID3D11DeviceContext*)av_frame_->data[1];
    //d3dDeviceContext_->CopyResource(d3dTexture_, texture);
    //  DXGI_FORMAT_R8_UNORM for NV12 luminance channel

    /* it is too hard for me to convert it to RGBA.
    D3D11_SHADER_RESOURCE_VIEW_DESC luminance_desc =
        CD3D11_SHADER_RESOURCE_VIEW_DESC(
            d3dTexture_, D3D11_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8_UNORM);
    m_device->CreateShaderResourceView(d3dTexture_, &luminance_desc,
                                       &m_luminance_shader_resource_view);

    // DXGI_FORMAT_R8G8_UNORM for NV12 chrominance channel
    D3D11_SHADER_RESOURCE_VIEW_DESC chrominance_desc =
        CD3D11_SHADER_RESOURCE_VIEW_DESC(
            d3dTexture_, D3D11_SRV_DIMENSION_TEXTURE2D,
                                         DXGI_FORMAT_R8G8_UNORM);

    m_device->CreateShaderResourceView(m_texture, &chrominance_desc,
                                       &m_chrominance_shader_resource_view);

    m_device_context->PSSetShaderResources(
        0, 1, m_luminance_shader_resource_view.GetAddressOf());

    m_device_context->PSSetShaderResources(
        1, 1, m_chrominance_shader_resource_view.GetAddressOf());

    ComPtr<IDXGIResource> dxgi_resource;
    m_texture->QueryInterface(
        __uuidof(IDXGIResource),
        reinterpret_cast<void**>(dxgi_resource.GetAddressOf()));

    dxgi_resource->GetSharedHandle(&m_shared_handle);

    m_device->OpenSharedResource(
        m_shared_handle, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(m_texture.GetAddressOf()));

    ComPtr<ID3D11Texture2D> new_texture = (ID3D11Texture2D*)frame->data[0];
    const int texture_index = frame->data[1];
    m_device_context->CopySubresourceRegion(
        m_texture.Get(), 0, 0, 0, 0, new_texture.Get(), texture_index, nullptr);
*/
    rtc::scoped_refptr<NativeHandleBuffer> buffer = rtc::make_ref_counted<NativeHandleBuffer>((void*)texture, texture_width,
    texture_height);

    VideoFrame decoded_frame = VideoFrame::Builder()
                    .set_video_frame_buffer(buffer)
                    .set_timestamp_rtp(input_image.Timestamp())
                    .set_color_space(color_space)
                    .build();

  // Return decoded frame.
  // TODO(nisse): Timestamp and rotation are all zero here. Change decoder
  // interface to pass a VideoFrameBuffer instead of a VideoFrame?
  decoded_image_callback_->Decoded(decoded_frame, absl::nullopt, qp);

  // Stop referencing it, possibly freeing `input_frame`.
  av_frame_unref(av_frame_.get());

  return WEBRTC_VIDEO_CODEC_OK;
  }

  // Obtain the `video_frame` containing the decoded image.
  VideoFrame* input_frame =
      static_cast<VideoFrame*>(av_buffer_get_opaque(av_frame_->buf[0]));
  RTC_DCHECK(input_frame);
  rtc::scoped_refptr<VideoFrameBuffer> frame_buffer =
      input_frame->video_frame_buffer();

  // Instantiate Planar YUV buffer according to video frame buffer type
  const webrtc::PlanarYuvBuffer* planar_yuv_buffer = nullptr;
  const webrtc::PlanarYuv8Buffer* planar_yuv8_buffer = nullptr;
  const webrtc::PlanarYuv16BBuffer* planar_yuv16_buffer = nullptr;
  VideoFrameBuffer::Type video_frame_buffer_type = frame_buffer->type();
  switch (video_frame_buffer_type) {
    case VideoFrameBuffer::Type::kI420:
      planar_yuv_buffer = frame_buffer->GetI420();
      planar_yuv8_buffer =
          reinterpret_cast<const webrtc::PlanarYuv8Buffer*>(planar_yuv_buffer);
      break;
    case VideoFrameBuffer::Type::kI444:
      planar_yuv_buffer = frame_buffer->GetI444();
      planar_yuv8_buffer =
          reinterpret_cast<const webrtc::PlanarYuv8Buffer*>(planar_yuv_buffer);
      break;
    case VideoFrameBuffer::Type::kI422:
      planar_yuv_buffer = frame_buffer->GetI422();
      planar_yuv8_buffer =
          reinterpret_cast<const webrtc::PlanarYuv8Buffer*>(planar_yuv_buffer);
      break;
    case VideoFrameBuffer::Type::kI010:
      planar_yuv_buffer = frame_buffer->GetI010();
      planar_yuv16_buffer = reinterpret_cast<const webrtc::PlanarYuv16BBuffer*>(
          planar_yuv_buffer);
      break;
    case VideoFrameBuffer::Type::kI210:
      planar_yuv_buffer = frame_buffer->GetI210();
      planar_yuv16_buffer = reinterpret_cast<const webrtc::PlanarYuv16BBuffer*>(
          planar_yuv_buffer);
      break;
    case VideoFrameBuffer::Type::kI410:
      planar_yuv_buffer = frame_buffer->GetI410();
      planar_yuv16_buffer = reinterpret_cast<const webrtc::PlanarYuv16BBuffer*>(
          planar_yuv_buffer);
      break;
    default:
      // If this code is changed to allow other video frame buffer type,
      // make sure that the code below which wraps I420/I422/I444 buffer and
      // code which converts to NV12 is changed
      // to work with new video frame buffer type

      RTC_LOG(LS_ERROR) << "frame_buffer type: "
                        << static_cast<int32_t>(video_frame_buffer_type)
                        << " is not supported!";
      ReportError();
      return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // When needed, FFmpeg applies cropping by moving plane pointers and adjusting
  // frame width/height. Ensure that cropped buffers lie within the allocated
  // memory.
  RTC_DCHECK_LE(av_frame_->width, planar_yuv_buffer->width());
  RTC_DCHECK_LE(av_frame_->height, planar_yuv_buffer->height());
  switch (video_frame_buffer_type) {
    case VideoFrameBuffer::Type::kI420:
    case VideoFrameBuffer::Type::kI444:
    case VideoFrameBuffer::Type::kI422: {
      RTC_DCHECK_GE(av_frame_->data[kYPlaneIndex], planar_yuv8_buffer->DataY());
      RTC_DCHECK_LE(
          av_frame_->data[kYPlaneIndex] +
              av_frame_->linesize[kYPlaneIndex] * av_frame_->height,
          planar_yuv8_buffer->DataY() +
              planar_yuv8_buffer->StrideY() * planar_yuv8_buffer->height());
      RTC_DCHECK_GE(av_frame_->data[kUPlaneIndex], planar_yuv8_buffer->DataU());
      RTC_DCHECK_LE(
          av_frame_->data[kUPlaneIndex] +
              av_frame_->linesize[kUPlaneIndex] *
                  planar_yuv8_buffer->ChromaHeight(),
          planar_yuv8_buffer->DataU() + planar_yuv8_buffer->StrideU() *
                                            planar_yuv8_buffer->ChromaHeight());
      RTC_DCHECK_GE(av_frame_->data[kVPlaneIndex], planar_yuv8_buffer->DataV());
      RTC_DCHECK_LE(
          av_frame_->data[kVPlaneIndex] +
              av_frame_->linesize[kVPlaneIndex] *
                  planar_yuv8_buffer->ChromaHeight(),
          planar_yuv8_buffer->DataV() + planar_yuv8_buffer->StrideV() *
                                            planar_yuv8_buffer->ChromaHeight());
      break;
    }
    case VideoFrameBuffer::Type::kI010:
    case VideoFrameBuffer::Type::kI210:
    case VideoFrameBuffer::Type::kI410: {
      RTC_DCHECK_GE(
          av_frame_->data[kYPlaneIndex],
          reinterpret_cast<const uint8_t*>(planar_yuv16_buffer->DataY()));
      RTC_DCHECK_LE(
          av_frame_->data[kYPlaneIndex] +
              av_frame_->linesize[kYPlaneIndex] * av_frame_->height,
          reinterpret_cast<const uint8_t*>(planar_yuv16_buffer->DataY()) +
              planar_yuv16_buffer->StrideY() * 2 *
                  planar_yuv16_buffer->height());
      RTC_DCHECK_GE(
          av_frame_->data[kUPlaneIndex],
          reinterpret_cast<const uint8_t*>(planar_yuv16_buffer->DataU()));
      RTC_DCHECK_LE(
          av_frame_->data[kUPlaneIndex] +
              av_frame_->linesize[kUPlaneIndex] *
                  planar_yuv16_buffer->ChromaHeight(),
          reinterpret_cast<const uint8_t*>(planar_yuv16_buffer->DataU()) +
              planar_yuv16_buffer->StrideU() * 2 *
                  planar_yuv16_buffer->ChromaHeight());
      RTC_DCHECK_GE(
          av_frame_->data[kVPlaneIndex],
          reinterpret_cast<const uint8_t*>(planar_yuv16_buffer->DataV()));
      RTC_DCHECK_LE(
          av_frame_->data[kVPlaneIndex] +
              av_frame_->linesize[kVPlaneIndex] *
                  planar_yuv16_buffer->ChromaHeight(),
          reinterpret_cast<const uint8_t*>(planar_yuv16_buffer->DataV()) +
              planar_yuv16_buffer->StrideV() * 2 *
                  planar_yuv16_buffer->ChromaHeight());
      break;
    }
    default:
      RTC_LOG(LS_ERROR) << "frame_buffer type: "
                        << static_cast<int32_t>(video_frame_buffer_type)
                        << " is not supported!";
      ReportError();
      return WEBRTC_VIDEO_CODEC_ERROR;
  }

  rtc::scoped_refptr<webrtc::VideoFrameBuffer> cropped_buffer;
  switch (video_frame_buffer_type) {
    case VideoFrameBuffer::Type::kI420:
      cropped_buffer = WrapI420Buffer(
          av_frame_->width, av_frame_->height, av_frame_->data[kYPlaneIndex],
          av_frame_->linesize[kYPlaneIndex], av_frame_->data[kUPlaneIndex],
          av_frame_->linesize[kUPlaneIndex], av_frame_->data[kVPlaneIndex],
          av_frame_->linesize[kVPlaneIndex],
          // To keep reference alive.
          [frame_buffer] {});
      break;
    case VideoFrameBuffer::Type::kI444:
      cropped_buffer = WrapI444Buffer(
          av_frame_->width, av_frame_->height, av_frame_->data[kYPlaneIndex],
          av_frame_->linesize[kYPlaneIndex], av_frame_->data[kUPlaneIndex],
          av_frame_->linesize[kUPlaneIndex], av_frame_->data[kVPlaneIndex],
          av_frame_->linesize[kVPlaneIndex],
          // To keep reference alive.
          [frame_buffer] {});
      break;
    case VideoFrameBuffer::Type::kI422:
      cropped_buffer = WrapI422Buffer(
          av_frame_->width, av_frame_->height, av_frame_->data[kYPlaneIndex],
          av_frame_->linesize[kYPlaneIndex], av_frame_->data[kUPlaneIndex],
          av_frame_->linesize[kUPlaneIndex], av_frame_->data[kVPlaneIndex],
          av_frame_->linesize[kVPlaneIndex],
          // To keep reference alive.
          [frame_buffer] {});
      break;
    case VideoFrameBuffer::Type::kI010:
      cropped_buffer = WrapI010Buffer(
          av_frame_->width, av_frame_->height,
          reinterpret_cast<const uint16_t*>(av_frame_->data[kYPlaneIndex]),
          av_frame_->linesize[kYPlaneIndex] / 2,
          reinterpret_cast<const uint16_t*>(av_frame_->data[kUPlaneIndex]),
          av_frame_->linesize[kUPlaneIndex] / 2,
          reinterpret_cast<const uint16_t*>(av_frame_->data[kVPlaneIndex]),
          av_frame_->linesize[kVPlaneIndex] / 2,
          // To keep reference alive.
          [frame_buffer] {});
      break;
    case VideoFrameBuffer::Type::kI210:
      cropped_buffer = WrapI210Buffer(
          av_frame_->width, av_frame_->height,
          reinterpret_cast<const uint16_t*>(av_frame_->data[kYPlaneIndex]),
          av_frame_->linesize[kYPlaneIndex] / 2,
          reinterpret_cast<const uint16_t*>(av_frame_->data[kUPlaneIndex]),
          av_frame_->linesize[kUPlaneIndex] / 2,
          reinterpret_cast<const uint16_t*>(av_frame_->data[kVPlaneIndex]),
          av_frame_->linesize[kVPlaneIndex] / 2,
          // To keep reference alive.
          [frame_buffer] {});
      break;
    case VideoFrameBuffer::Type::kI410:
      cropped_buffer = WrapI410Buffer(
          av_frame_->width, av_frame_->height,
          reinterpret_cast<const uint16_t*>(av_frame_->data[kYPlaneIndex]),
          av_frame_->linesize[kYPlaneIndex] / 2,
          reinterpret_cast<const uint16_t*>(av_frame_->data[kUPlaneIndex]),
          av_frame_->linesize[kUPlaneIndex] / 2,
          reinterpret_cast<const uint16_t*>(av_frame_->data[kVPlaneIndex]),
          av_frame_->linesize[kVPlaneIndex] / 2,
          // To keep reference alive.
          [frame_buffer] {});
      break;
    default:
      RTC_LOG(LS_ERROR) << "frame_buffer type: "
                        << static_cast<int32_t>(video_frame_buffer_type)
                        << " is not supported!";
      ReportError();
      return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Pass on color space from input frame if explicitly specified.
  const ColorSpace& color_space =
      input_image.ColorSpace() ? *input_image.ColorSpace()
                               : ExtractH264ColorSpace(av_context_.get());

  VideoFrame decoded_frame = VideoFrame::Builder()
                                 .set_video_frame_buffer(cropped_buffer)
                                 .set_timestamp_rtp(input_image.Timestamp())
                                 .set_color_space(color_space)
                                 .build();

  // Return decoded frame.
  // TODO(nisse): Timestamp and rotation are all zero here. Change decoder
  // interface to pass a VideoFrameBuffer instead of a VideoFrame?
  decoded_image_callback_->Decoded(decoded_frame, absl::nullopt, qp);

  // Stop referencing it, possibly freeing `input_frame`.
  av_frame_unref(av_frame_.get());
  input_frame = nullptr;

  return WEBRTC_VIDEO_CODEC_OK;
}

const char* H264DecoderImpl::ImplementationName() const {
  return "FFmpeg";
}

bool H264DecoderImpl::IsInitialized() const {
  return av_context_ != nullptr;
}

void H264DecoderImpl::ReportInit() {
  if (has_reported_init_)
    return;
  RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H264DecoderImpl.Event",
                            kH264DecoderEventInit, kH264DecoderEventMax);
  has_reported_init_ = true;
}

void H264DecoderImpl::ReportError() {
  if (has_reported_error_)
    return;
  RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H264DecoderImpl.Event",
                            kH264DecoderEventError, kH264DecoderEventMax);
  has_reported_error_ = true;
}

}  // namespace webrtc

#endif  // WEBRTC_USE_H264
