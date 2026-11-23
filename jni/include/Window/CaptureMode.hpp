#pragma once
#include <cstdint>

/**
 * Controls how screenshots and screen recordings see the overlay layer.
 *  - Visible: Normal overlay, visible in screenshots and screen recordings.
 *  - Hidden:  Excluded from screenshots/recordings via SurfaceFlinger eSkipScreenshot /
 *             LayerMetadata window type (Android 8.1–17).
 *  - Secure:  Protected via SurfaceFlinger eSecure flag (0x80), blacked out in captures
 *             including Android 5–8.0 and OEM ROMs that ignore skip-screenshot flags.
 */
enum class CaptureMode : uint8_t {
    Visible = 0,
    Hidden  = 1,
    Secure  = 2
};

inline const char* CaptureModeToString(CaptureMode mode) noexcept {
    switch (mode) {
        case CaptureMode::Hidden: return "Hidden";
        case CaptureMode::Secure: return "Secure";
        case CaptureMode::Visible:
        default:                  return "Visible";
    }
}

inline CaptureMode SanitizeCaptureMode(uint8_t raw) noexcept {
    return (raw <= static_cast<uint8_t>(CaptureMode::Secure))
        ? static_cast<CaptureMode>(raw)
        : CaptureMode::Visible;
}

enum class DisplayRotation : uint8_t {
    Deg0   = 0,
    Deg90  = 1,
    Deg180 = 2,
    Deg270 = 3
};

struct DisplayInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    DisplayRotation rotation = DisplayRotation::Deg0;
    float density = 1.0f;
    float refreshRate = 0.0f;

    bool IsValid() const noexcept {
        return width > 0 && height > 0;
    }

    int OrientationIndex() const noexcept {
        return static_cast<int>(rotation);
    }

    uint32_t Degrees() const noexcept {
        return static_cast<uint32_t>(rotation) * 90U;
    }
};
