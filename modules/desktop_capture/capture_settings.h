#ifndef MODULES_DESKTOP_CAPTURE_SETTINGS_H_
#define MODULES_DESKTOP_CAPTURE_SETTINGS_H_
#include <atomic>

namespace webrtc {

class DecoderSettings {
public:
    static std::atomic<bool> hardware_accelerated;
    static bool isHardwareAccelerationSupported() {
        return true;
    }

    static bool isHardwareAccelerationEnabled() {
        return true;
    }

};

}  // namespace webrtc

#endif  // MODULES_DESKTOP_CAPTURE_SETTINGS_H_
