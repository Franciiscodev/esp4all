#ifndef NATIVESURFACE_DRAW_H
#define NATIVESURFACE_DRAW_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unistd.h>

#include <android/native_window.h>

#include <Graphics/GraphicsBackend.hpp>
#include <Input/TouchManager.hpp>
#include <Window/CaptureMode.hpp>
#include <Window/NativeWindow.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <timer.h>

class ImGuiCfg {
private:
    esp4all::NativeWindow window_;
    std::unique_ptr<IGraphicsBackend> backend_;
    esp4all::TouchManager touch_;
    CaptureMode captureMode_ = CaptureMode::Visible;

    int surface_w_ = 0;
    int surface_h_ = 0;
    int targetFps_ = 60;
    bool g_Initialized = false;

    std::chrono::steady_clock::time_point lastDisplayPoll_{};
    std::chrono::steady_clock::time_point lastFrameTime_{};
    std::chrono::steady_clock::time_point frameStartedAt_{};
    std::chrono::steady_clock::time_point settleUntil_{};
    bool hasLastFrameTime_ = false;
    bool frameHadInput_ = false;
    bool frameWasScrolling_ = false;
    uint64_t lastFrameFingerprint_ = 0;

    bool createSurfaceAndBackend(GraphicsBackendType preferredBackend, CaptureMode captureMode);
    bool tryCreateBackend(GraphicsBackendType concreteType, CaptureMode captureMode);
    bool rebindToFreshWindow(int width, int height, CaptureMode captureMode);
    void handleDisplayResize();
    bool updateDisplayInfo();
    bool ImGui_init();
    void paceFrame(int fps, bool frameSkipped);
    void shutdownGraphicsOnly();

public:
    timer RenderingFPS;

    // Main-thread mirrored display metrics
    int Orientation = 0;
    int screen_x = 0;
    int screen_y = 0;
    int screenX2 = 0;
    int screenY2 = 0;
    float displayDensity = 1.0f;

    ImGuiCfg() = default;
    ~ImGuiCfg() {
        Shutdown();
    }

    ImGuiCfg(const ImGuiCfg&) = delete;
    ImGuiCfg& operator=(const ImGuiCfg&) = delete;

    bool InitOverlay(
        GraphicsBackendType preferredBackend = GraphicsBackendType::Auto,
        CaptureMode captureMode = CaptureMode::Visible);
    bool RecreateOverlay(GraphicsBackendType preferredBackend, CaptureMode captureMode);
    // Switches CaptureMode via live surface Rebind() without destroying the ImGui context.
    bool SetCaptureMode(CaptureMode captureMode);

    void StartFrame(int fps);
    // When allowIdleSkip is true (ESP paused), skips redundant GPU draws if UI is static.
    void EndFrame(bool allowIdleSkip = false);
    void Shutdown();

    const char* GetActiveBackendName() const noexcept {
        return backend_ ? backend_->GetName() : "None";
    }
    GraphicsBackendType GetActiveBackendType() const noexcept {
        return backend_ ? backend_->GetType() : GraphicsBackendType::Auto;
    }
    CaptureMode GetCaptureMode() const noexcept { return captureMode_; }

    // Touch subsystem (multi-protocol evdev + optional /dev/uinput interception)
    bool StartTouch(bool enableControl = false, std::string* outError = nullptr);
    void StopTouch();
    bool SetTouchControl(bool enabled, std::string* outError = nullptr);
    bool IsTouchControlEnabled() const noexcept { return touch_.IsControlEnabled(); }
    std::string GetTouchStatusSummary() const { return touch_.GetStatusSummary(); }
};

#endif
