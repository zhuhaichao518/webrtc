#ifndef MODULES_DESKTOP_CAPTURE_SETTINGS_H_
#define MODULES_DESKTOP_CAPTURE_SETTINGS_H_


namespace webrtc {

class DecoderSettings {
public:
    static bool isHardwareAccelerationSupported() {
        return true;
    }

    static bool isHardwareAccelerationEnabled() {
        return true;
    }
};

}  // namespace webrtc

#endif  // MODULES_DESKTOP_CAPTURE_SETTINGS_H_
