#include "FFEncoder.h"

#include "rtc_base/checks.h"
#include "rtc_base/logging.h"

extern "C" {
#include "third_party/ffmpeg/libavformat/avformat.h"
#include "third_party/ffmpeg/libswscale/swscale.h"
//#if defined(WEBRTC_WIN)
#include "third_party/ffmpeg/libavutil/hwcontext_d3d11va.h"
//#endif
}  // extern "C"

namespace webrtc {
struct ScopedPtrAVFreePacket {
  void operator()(AVPacket* packet) { av_packet_free(&packet); }
};
typedef std::unique_ptr<AVPacket, ScopedPtrAVFreePacket> ScopedAVPacket;

FFEncoder::FFEncoder():hardware(true) {}

FFEncoder::~FFEncoder() {
  if (d3dDevice_) {
    d3dDevice_->Release();
  }
}

void do_nothing(void *) {}

bool FFEncoder::init(const std::string& codec_name, const VideoCodec* codec_settings) {
    int ret;

    d3dDevice_ = (ID3D11Device *)hw_device;

    /* find the H.264 video encoder */
    const AVCodec* codec = avcodec_find_encoder_by_name(codec_name.c_str());
    if(!codec) {
      // fall back to intel quick sync
      codec = avcodec_find_encoder_by_name("h264_qsv");
      if(!codec) {
        return false;
      }
    }

    RTC_DCHECK(!av_context_);

    codecsettings_ = codec_settings;

    // Initialize AVCodecContext.
    av_context_.reset(avcodec_alloc_context3(nullptr));
    av_context_->codec_type = AVMEDIA_TYPE_VIDEO;
    av_context_->codec_id = AV_CODEC_ID_H264;
    av_context_->width     = codec_settings->width;
    av_context_->height    = codec_settings->height;
    av_context_->time_base = AVRational { 1, codec_settings->maxFramerate };
    av_context_->framerate = AVRational { codec_settings->maxFramerate, 1 };
    //Todo(Haichao):Add hevc and hevc_main_10 support.
    av_context_->profile = FF_PROFILE_H264_HIGH;

    // B-frames delays decoder output.
    av_context_->max_b_frames = 0;
    // Use an infinite GOP length since I-frames are generated on demand
    // TODO:Haichao For VAAPI(linux), we should use std::numeric_limits<std::int16_t>::max()
    av_context_->gop_size = std::numeric_limits<int>::max();
 
    av_context_->keyint_min = std::numeric_limits<int>::max();

    /**< [in]: Specifies the DPB size used for encoding. Setting it to 0 will let driver use the default DPB size.
     The low latency application which wants to invalidate reference frame as an error resilience tool
    is recommended to use a large DPB size so that the encoder can keep old reference frames which can be used if recent
    frames are invalidated. */
    av_context_->refs = 0;

    av_context_->flags |= (AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY);
    av_context_->flags2 |= AV_CODEC_FLAG2_FAST;

    av_context_->color_range = AVCOL_RANGE_JPEG;
    int sws_color_space;
    if (codec_settings->width <= 1920){
      // Rec. 709
      av_context_->color_primaries = AVCOL_PRI_BT709;
      av_context_->color_trc       = AVCOL_TRC_BT709;
      av_context_->colorspace      = AVCOL_SPC_BT709;
      sws_color_space      = SWS_CS_ITU709;
    }else{
      // Rec. 2020
      av_context_->color_primaries = AVCOL_PRI_BT2020;
      av_context_->color_trc       = AVCOL_TRC_BT2020_10;
      av_context_->colorspace      = AVCOL_SPC_BT2020_NCL;
      sws_color_space      = SWS_CS_BT2020;
    }
    //Haichao:Linux & mac is different.
    av_context_->sw_pix_fmt = AV_PIX_FMT_NV12;

    if (hardware){
      av_context_->pix_fmt = AV_PIX_FMT_D3D11;
      
      // make hardware device context
      m_HwDeviceContext.reset(av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA));
      auto avd3d11context = (AVD3D11VADeviceContext *)((AVHWDeviceContext *)m_HwDeviceContext->data)->hwctx;

      std::fill_n((std::uint8_t *)avd3d11context, sizeof(AVD3D11VADeviceContext), 0);

      d3dDevice_->AddRef();
      avd3d11context->device = d3dDevice_;

      avd3d11context->lock_ctx = (void *)1;
      avd3d11context->lock     = do_nothing;
      avd3d11context->unlock   = do_nothing;

      auto err = av_hwdevice_ctx_init(m_HwDeviceContext.get());
      if(err) {
        RTC_LOG(LS_ERROR) << "error: Failed ffmpeg av_hwdevice_ctx_init";
        return false;
      }
      // end of make hardware device context

      // allocate hardware frame
      //hwframe_ctx(ctx, hwdevice_ctx, sw_fmt);
      frame_ref.reset(av_hwframe_ctx_alloc(m_HwDeviceContext.get()));
      m_HwFramesContext.reset((AVHWFramesContext *)frame_ref->data);
      m_HwFramesContext->format            = av_context_->pix_fmt;
      m_HwFramesContext->sw_format         = av_context_->sw_pix_fmt;
      m_HwFramesContext->height            = av_context_->height;
      m_HwFramesContext->width             = av_context_->width;
      m_HwFramesContext->initial_pool_size = 0;

      if(auto err = av_hwframe_ctx_init(frame_ref.get()); err < 0) {
        return err;
      }
      av_context_->hw_frames_ctx = av_buffer_ref(frame_ref.get());
      // end of allocate hardware frame
    }
    
    //Haichao:slice?
    /*int num_temporal_layers =
          std::max(codec_.H264()->numberOfTemporalLayers,
                  codec_.simulcastStream[idx].numberOfTemporalLayers);
    
    if (num_temporal_layers > 0){
      av_context_->slices = num_temporal_layers;
    }*/
    av_context_->slices = 1;

    av_context_->thread_type  = FF_THREAD_SLICE;
    av_context_->thread_count = av_context_->slices;
  
    // TODO(Haichao): use fixed bitrate. Check if we can use dynamic and handle QP.
    auto bitrate = codec_settings->maxBitrate * 1000;
    av_context_->rc_max_rate    = bitrate;
    av_context_->rc_buffer_size = bitrate / codec_settings->maxFramerate;
    av_context_->bit_rate       = bitrate;
    av_context_->rc_min_rate    = bitrate;

    if(auto status = avcodec_open2(av_context_.get(), codec, nullptr/*haichao:&options?*/)) {
      return false;
    }
    
    av_frame_.reset(av_frame_alloc());
    av_frame_->format = av_context_->pix_fmt;
    av_frame_->width  = av_context_->width;
    av_frame_->height = av_context_->height;

    if(hardware) {
      av_frame_->hw_frames_ctx = av_buffer_ref(av_context_->hw_frames_ctx);
    }

    return true;
}

bool FFEncoder::EncodeFrame(const VideoFrame& input_image){
    if (d3dDevice_ == nullptr){
      if (input_image->)
    } 
    if (hardware){
        
    }
}

bool FFEncoder::supportsCodec(const std::string& codec_name) {
    return avcodec_find_encoder_by_name(codec_name.c_str()) != nullptr;
}

bool FFEncoder::setEncoderParams(const VideoCodec* codec_settings) {
    return true;
}

} //namespace webrtc
