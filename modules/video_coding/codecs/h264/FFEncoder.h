#ifndef FF_ENCODER_H
#define FF_ENCODER_H

#include <memory>
#include <string>
#include <vector>
//#ifdef WEBRTC_WIN
#include <d3d11.h>
#include <wrl/client.h>
//#endif

extern "C" {
#include "third_party/ffmpeg/libavcodec/avcodec.h"
#include "third_party/ffmpeg/libavformat/avformat.h"
#include "third_party/ffmpeg/libavutil/hwcontext.h"
}  // extern "C"

#include "api/video_codecs/video_encoder.h"

namespace webrtc {
class FFEncoder {
public:
    FFEncoder();
    ~FFEncoder();
    
    bool init(ID3D11Device* d3d11device, int width, int height);
    bool init(const std::string& codec_name, const VideoCodec* codec_settings, ID3D11Device* d3d11device);
    bool ContinueInit(const VideoFrame& input_image);
    bool supportsCodec(const std::string& codec_name);
    bool EncodeFrame(const VideoFrame& input_image);
    bool setEncoderParams(const VideoCodec* codec_settings);

private: 
    const VideoCodec* codecsettings_;
    bool hardware;
    struct AVCodecContextDeleter {
      void operator()(AVCodecContext* av_context_) { avcodec_free_context(&av_context_); }
    };
    std::unique_ptr<AVCodecContext, AVCodecContextDeleter> av_context_;
    struct AVFrameDeleter {
        void operator()(AVFrame* frame) { av_frame_free(&frame); }
    };
    std::unique_ptr<AVFrame, AVFrameDeleter> av_frame_;

    std::unique_ptr<AVBufferRef, decltype(&av_buffer_unref)> m_HwDeviceContext;

    std::unique_ptr<AVBufferRef, decltype(&av_buffer_unref)> frame_ref;

    std::unique_ptr<AVHWFramesContext, decltype(&av_buffer_unref)> m_HwFramesContext;

//#if defined(WEBRTC_WIN)
    bool is_testing;
    //Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    ID3D11Device * d3dDevice_;
    //Microsoft::WRL::ComPtr<ID3D11Texture2D> d3dtexture_;
//#endif
};

} //namespace webrtc

#endif // FF_ENCODER_H
