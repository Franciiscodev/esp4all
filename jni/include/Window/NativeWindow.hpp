#pragma once

#include "Window/CaptureMode.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

struct ANativeWindow;

namespace esp4all {

struct VsyncInfo {
    std::chrono::steady_clock::time_point timestamp{};
    std::chrono::nanoseconds period{0};
    bool valid = false;
};

/**
 * RAII Android SurfaceFlinger overlay window (Android 5–17 / API 21–37+).
 *
 * Uses a process-wide cached SurfaceFlinger connection (client, display token, transaction)
 * backed by symbol-probing LibGui, so repeated display queries and layer recreations leak
 * zero resources. Validates android_native_base_t::magic ('_wnd') before acquiring ANativeWindow.
 */
class NativeWindow {
public:
    NativeWindow() = default;
    ~NativeWindow();

    NativeWindow(const NativeWindow&) = delete;
    NativeWindow& operator=(const NativeWindow&) = delete;
    NativeWindow(NativeWindow&& other) noexcept;
    NativeWindow& operator=(NativeWindow&& other) noexcept;

    // Creates a top-most (INT32_MAX) transparent RGBA_8888 overlay layer.
    // If width or height is 0, uses the current main display dimensions.
    bool Create(
        const char* name,
        uint32_t width,
        uint32_t height,
        CaptureMode capture = CaptureMode::Visible,
        std::string* outError = nullptr);

    void Destroy();

    bool IsValid() const noexcept { return window_ != nullptr; }
    ANativeWindow* Get() const noexcept { return window_; }
    CaptureMode GetCaptureMode() const noexcept { return captureMode_; }
    uint32_t GetWidth() const noexcept { return width_; }
    uint32_t GetHeight() const noexcept { return height_; }
    bool IsVisible() const noexcept { return visible_; }

    bool SetVisible(bool visible, std::string* outError = nullptr);
    bool Resize(uint32_t width, uint32_t height, bool& outResizedInPlace, std::string* outError = nullptr);

    // Static display & system helpers (zero subprocesses, cached SurfaceFlinger connection).
    static DisplayInfo QueryDisplayInfo(std::string* outError = nullptr);
    static bool QueryVsync(VsyncInfo& outVsync);
    static uint32_t GetAndroidApiLevel();
    static float GetDisplayDensity();
    static std::vector<std::string> GetResolvedSymbols();

private:
    void* control_ = nullptr;
    void* surface_ = nullptr;
    ANativeWindow* window_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    CaptureMode captureMode_ = CaptureMode::Visible;
    bool visible_ = false;
};

} // namespace esp4all
