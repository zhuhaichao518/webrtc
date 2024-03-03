#ifndef MODULES_DESKTOP_CAPTURE_SETTINGS_H_
#define MODULES_DESKTOP_CAPTURE_SETTINGS_H_
#include <atomic>
#include <d3d11.h>

namespace webrtc {

class DecoderSettings {
public:
    static bool is_debugging_;
    static std::atomic<bool> hardware_accelerated;
    
    //If a configureencoder is called, set it here.
    static std::atomic<bool> encode_setted;

    static int width,height,framerate;

    static bool isHardwareAccelerationSupported() {
        return true;
    }

    static bool isHardwareAccelerationEnabled() {
        return true;
    }

    static bool showFrame(ID3D11Device* device, ID3D11Texture2D* texture, int width, int height);

};

}  // namespace webrtc

#endif  // MODULES_DESKTOP_CAPTURE_SETTINGS_H_
