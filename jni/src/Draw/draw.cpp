#include <draw.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include "Graphics/FrameHasher.hpp"
#include "Graphics/GraphicsManager.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;

constexpr milliseconds kDisplayPollInterval(150);
constexpr milliseconds kIdleSettleDuration(500);
constexpr milliseconds kIdleSleepSlice(33);

void loadAndroidSystemFont(ImGuiIO& io) {
    static constexpr const char* kFontCandidates[] = {
        "/system/fonts/RobotoStatic-Regular.ttf",
        "/system/fonts/Roboto-Regular.ttf",
        "/system/fonts/NotoSans-Regular.ttf",
        "/system/fonts/DroidSans.ttf"
    };
    for (const char* path : kFontCandidates) {
        if (access(path, R_OK) == 0 && io.Fonts->AddFontFromFileTTF(path) != nullptr) {
            std::printf("[ImGuiCfg] Loaded system font: %s\n", path);
            return;
        }
    }
    std::printf("[ImGuiCfg] System TTF font not found, falling back to built-in ImGui font\n");
    io.Fonts->AddFontDefault();
}

std::string execOneShot(const char* command) {
    char buffer[128];
    std::string result;
    FILE* pipe = popen(command, "r");
    if (!pipe) {
        return result;
    }
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    return result;
}

bool isDrawDataEmpty(const ImDrawData* drawData) noexcept {
    if (!drawData || drawData->CmdListsCount <= 0) {
        return true;
    }
    for (int n = 0; n < drawData->CmdListsCount; ++n) {
        const ImDrawList* list = drawData->CmdLists[n];
        if (!list) {
            continue;
        }
        for (int i = 0; i < list->CmdBuffer.Size; ++i) {
            const ImDrawCmd& cmd = list->CmdBuffer[i];
            if (cmd.UserCallback != nullptr || cmd.ElemCount > 0) {
                return false;
            }
        }
    }
    return true;
}

uint64_t computeDrawDataFingerprint(const ImDrawData* drawData) noexcept {
    esp4all::FrameHasher hasher;
    if (!drawData) {
        return hasher.Finish();
    }
    hasher.MixF32(drawData->DisplaySize.x);
    hasher.MixF32(drawData->DisplaySize.y);

    for (int n = 0; n < drawData->CmdListsCount; ++n) {
        const ImDrawList* list = drawData->CmdLists[n];
        if (!list) {
            continue;
        }
        for (int i = 0; i < list->CmdBuffer.Size; ++i) {
            const ImDrawCmd& cmd = list->CmdBuffer[i];
            hasher.MixBytes(&cmd.ClipRect, sizeof(cmd.ClipRect));
            hasher.MixU64(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cmd.TexRef._TexData)));
            hasher.MixU64(static_cast<uint64_t>(cmd.TexRef._TexID));
            hasher.MixU64(static_cast<uint64_t>(cmd.ElemCount));
            hasher.MixU64(static_cast<uint64_t>(cmd.VtxOffset));
            hasher.MixU64(static_cast<uint64_t>(cmd.IdxOffset));
        }
        hasher.MixBytes(list->IdxBuffer.Data,
                        static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
        hasher.MixBytes(list->VtxBuffer.Data,
                        static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert));
    }
    return hasher.Finish();
}

} // namespace

bool ImGuiCfg::updateDisplayInfo() {
    std::string err;
    const DisplayInfo info = esp4all::NativeWindow::QueryDisplayInfo(&err);

    int w = static_cast<int>(info.width);
    int h = static_cast<int>(info.height);
    int orient = info.OrientationIndex();
    float density = info.density;

    // One-shot fallback only during startup if DisplayInfo was unavailable
    if ((w <= 0 || h <= 0) && screen_x <= 0) {
        if (!err.empty()) {
            std::printf("[ImGuiCfg] QueryDisplayInfo fallback (%s)\n", err.c_str());
        }
        std::string wmOut = execOneShot("wm size");
        int pw = 0, ph = 0;
        if (std::sscanf(wmOut.c_str(), "Physical size: %dx%d", &pw, &ph) == 2 && pw > 0 && ph > 0) {
            w = pw;
            h = ph;
            orient = 0;
            density = esp4all::NativeWindow::GetDisplayDensity();
        }
    }

    lastDisplayPoll_ = Clock::now();
    if (w <= 0 || h <= 0) {
        return false;
    }

    const bool changed = (w != screen_x || h != screen_y || orient != Orientation);
    screen_x = w;
    screen_y = h;
    screenX2 = w / 2;
    screenY2 = h / 2;
    Orientation = orient;
    if (density > 0.0f) {
        displayDensity = density;
    }
    touch_.UpdateDisplay(w, h, static_cast<DisplayRotation>(orient & 3));
    return changed;
}

bool ImGuiCfg::rebindToFreshWindow(int width, int height, CaptureMode captureMode) {
    if (!backend_ || width <= 0 || height <= 0) {
        return false;
    }

    esp4all::NativeWindow nextWin;
    std::string err;
    if (!nextWin.Create(
            "ESP4AllOverlay",
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height),
            captureMode,
            &err)) {
        std::printf("[ImGuiCfg] Rebind NativeWindow::Create failed: %s\n",
                    err.empty() ? "unknown error" : err.c_str());
        return false;
    }

    // Keep the old window alive until the graphics backend detaches its surface and binds the new one.
    if (!backend_->Rebind(nextWin.Get())) {
        std::printf("[ImGuiCfg] Backend Rebind failed for %dx%d\n", width, height);
        nextWin.Destroy();
        return false;
    }

    esp4all::NativeWindow retired = std::move(window_);
    window_ = std::move(nextWin);
    retired.Destroy();

    surface_w_ = width;
    surface_h_ = height;
    captureMode_ = captureMode;
    lastFrameFingerprint_ = 0;
    settleUntil_ = Clock::now() + kIdleSettleDuration;
    return true;
}

void ImGuiCfg::handleDisplayResize() {
    if (!backend_ || !window_.IsValid() || screen_x <= 0 || screen_y <= 0) {
        return;
    }
    if (screen_x == surface_w_ && screen_y == surface_h_) {
        return;
    }

    std::printf("[ImGuiCfg] Display resized/rotated to %dx%d (orient=%d); rebinding layer\n",
                screen_x, screen_y, Orientation);

    bool resizedInPlace = false;
    std::string err;
    if (window_.Resize(static_cast<uint32_t>(screen_x),
                       static_cast<uint32_t>(screen_y),
                       resizedInPlace,
                       &err) &&
        resizedInPlace &&
        backend_->Rebind(window_.Get())) {
        surface_w_ = screen_x;
        surface_h_ = screen_y;
        lastFrameFingerprint_ = 0;
        settleUntil_ = Clock::now() + kIdleSettleDuration;
        return;
    }

    if (!rebindToFreshWindow(screen_x, screen_y, captureMode_)) {
        std::printf("[ImGuiCfg] Live Rebind fallback failed, performing full overlay rebuild\n");
        (void)RecreateOverlay(GetActiveBackendType(), captureMode_);
    }
}

bool ImGuiCfg::tryCreateBackend(GraphicsBackendType concreteType, CaptureMode captureMode) {
    std::string err;
    if (!window_.Create(
            "ESP4AllOverlay",
            static_cast<uint32_t>(surface_w_),
            static_cast<uint32_t>(surface_h_),
            captureMode,
            &err)) {
        std::printf("[ImGuiCfg] NativeWindow::Create failed: %s\n",
                    err.empty() ? "unknown error" : err.c_str());
        return false;
    }

    std::unique_ptr<IGraphicsBackend> candidate = GraphicsManager::Create(concreteType);
    if (!candidate || !candidate->Create(window_.Get(), surface_w_, surface_h_)) {
        if (candidate) {
            candidate->Cleanup();
        }
        window_.Destroy();
        return false;
    }

    backend_ = std::move(candidate);
    captureMode_ = captureMode;
    return true;
}

bool ImGuiCfg::createSurfaceAndBackend(GraphicsBackendType preferredBackend, CaptureMode captureMode) {
    const uint32_t api = esp4all::NativeWindow::GetAndroidApiLevel();
    if (preferredBackend == GraphicsBackendType::OpenGL) {
        if (tryCreateBackend(GraphicsBackendType::OpenGL, captureMode)) {
            std::printf("[ImGuiCfg] Initialized %s backend (%dx%d, API %u, Capture=%s, Density=%.2f)\n",
                        backend_->GetName(), surface_w_, surface_h_, api,
                        CaptureModeToString(captureMode), displayDensity);
            return true;
        }
        return false;
    }

    // Auto or Vulkan: try Vulkan first, then cleanly recreate a fresh ANativeWindow for OpenGL ES fallback
    if (tryCreateBackend(GraphicsBackendType::Vulkan, captureMode)) {
        std::printf("[ImGuiCfg] Initialized Vulkan backend (%dx%d, API %u, Capture=%s, Density=%.2f)\n",
                    surface_w_, surface_h_, api, CaptureModeToString(captureMode), displayDensity);
        return true;
    }

    std::printf("[ImGuiCfg] Vulkan unavailable, falling back to OpenGL ES\n");
    if (tryCreateBackend(GraphicsBackendType::OpenGL, captureMode)) {
        std::printf("[ImGuiCfg] Initialized %s backend (%dx%d, API %u, Capture=%s, Density=%.2f)\n",
                    backend_->GetName(), surface_w_, surface_h_, api,
                    CaptureModeToString(captureMode), displayDensity);
        return true;
    }

    return false;
}

bool ImGuiCfg::InitOverlay(GraphicsBackendType preferredBackend, CaptureMode captureMode) {
    (void)updateDisplayInfo();
    if (screen_x <= 0 || screen_y <= 0) {
        std::printf("[ImGuiCfg] Failed to query display dimensions\n");
        return false;
    }

    // Exact display dimensions (zero wasted square max(w,h)^2 swapchain memory).
    surface_w_ = screen_x;
    surface_h_ = screen_y;

    if (!createSurfaceAndBackend(preferredBackend, captureMode)) {
        std::printf("[ImGuiCfg] All graphics backends failed to initialize\n");
        return false;
    }

    if (!ImGui_init()) {
        std::printf("[ImGuiCfg] ImGui initialization failed\n");
        shutdownGraphicsOnly();
        return false;
    }

    RenderingFPS.AotuFPS_init();
    RenderingFPS.setAffinity();
    lastFrameFingerprint_ = 0;
    settleUntil_ = Clock::now() + kIdleSettleDuration;
    return true;
}

bool ImGuiCfg::SetCaptureMode(CaptureMode captureMode) {
    if (captureMode == captureMode_ && window_.IsValid()) {
        return true;
    }
    if (backend_ && g_Initialized && rebindToFreshWindow(screen_x, screen_y, captureMode)) {
        std::printf("[ImGuiCfg] Rebound overlay to Capture=%s without destroying ImGui context\n",
                    CaptureModeToString(captureMode));
        return true;
    }
    return RecreateOverlay(GetActiveBackendType(), captureMode);
}

bool ImGuiCfg::RecreateOverlay(GraphicsBackendType preferredBackend, CaptureMode captureMode) {
    // Runs safely between EndFrame() and next StartFrame() while touch thread remains alive
    shutdownGraphicsOnly();

    (void)updateDisplayInfo();
    if (screen_x > 0 && screen_y > 0) {
        surface_w_ = screen_x;
        surface_h_ = screen_y;
    }

    if (!createSurfaceAndBackend(preferredBackend, captureMode)) {
        std::printf("[ImGuiCfg] RecreateOverlay failed to create backend\n");
        return false;
    }

    if (!ImGui_init()) {
        std::printf("[ImGuiCfg] RecreateOverlay failed to reinitialize ImGui\n");
        shutdownGraphicsOnly();
        return false;
    }

    lastFrameFingerprint_ = 0;
    settleUntil_ = Clock::now() + kIdleSettleDuration;
    return true;
}

bool ImGuiCfg::StartTouch(bool enableControl, std::string* outError) {
    return touch_.Start(
        screen_x,
        screen_y,
        static_cast<DisplayRotation>(Orientation & 3),
        enableControl,
        outError);
}

void ImGuiCfg::StopTouch() {
    touch_.Stop();
}

bool ImGuiCfg::SetTouchControl(bool enabled, std::string* outError) {
    return touch_.SetControl(enabled, outError);
}

void ImGuiCfg::StartFrame(int fps) {
    const Clock::time_point now = Clock::now();
    frameStartedAt_ = now;
    targetFps_ = std::max(1, fps);

    if (std::chrono::duration_cast<milliseconds>(now - lastDisplayPoll_) >= kDisplayPollInterval) {
        if (updateDisplayInfo()) {
            handleDisplayResize();
        }
    }

    this->RenderingFPS.SetFps(static_cast<unsigned int>(targetFps_));
    if (backend_) {
        backend_->NewFrame(window_.Get());
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(surface_w_), static_cast<float>(surface_h_));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    if (hasLastFrameTime_) {
        const float dt = std::chrono::duration<float>(now - lastFrameTime_).count();
        io.DeltaTime = std::clamp(dt, 1e-4f, 0.25f);
    } else {
        io.DeltaTime = 1.0f / 60.0f;
        hasLastFrameTime_ = true;
    }
    lastFrameTime_ = now;

    frameHadInput_ = touch_.ApplyPendingToImGui();
    ImGui::NewFrame();
    frameWasScrolling_ = touch_.AfterNewFrame(static_cast<float>(surface_w_), static_cast<float>(surface_h_));

    if (frameHadInput_ || frameWasScrolling_) {
        settleUntil_ = now + kIdleSettleDuration;
    }
}

void ImGuiCfg::paceFrame(int fps, bool frameSkipped) {
    if (frameSkipped) {
        // Idle UI: sleep up to 33ms with 0% GPU work, waking instantly (<1ms) on touch input.
        (void)touch_.WaitForEvent(kIdleSleepSlice);
        RenderingFPS.AotuFPS_init();
        return;
    }

    const int clampedFps = std::clamp(fps, 1, 240);
    const nanoseconds targetPeriod(1000000000LL / static_cast<int64_t>(clampedFps));

    esp4all::VsyncInfo vsync{};
    if (esp4all::NativeWindow::QueryVsync(vsync) && vsync.valid && vsync.period > nanoseconds::zero()) {
        // Align to native display VSync when targetFps matches or exceeds the panel's refresh rate.
        if (targetPeriod <= vsync.period + milliseconds(1)) {
            const Clock::time_point now = Clock::now();
            const nanoseconds sinceAnchor = frameStartedAt_ - vsync.timestamp;
            const int64_t step = (sinceAnchor < nanoseconds::zero())
                ? -((-sinceAnchor) / vsync.period)
                : (sinceAnchor / vsync.period) + 1;
            const Clock::time_point nextVsync = vsync.timestamp + step * vsync.period;
            if (nextVsync > now) {
                const nanoseconds rem = std::min<nanoseconds>(nextVsync - now, vsync.period);
                std::this_thread::sleep_for(rem);
            }
            RenderingFPS.AotuFPS_init();
            return;
        }
    }

    this->RenderingFPS.AotuFPS();
}

void ImGuiCfg::EndFrame(bool allowIdleSkip) {
    ImGui::Render();
    touch_.PublishInterfaceView(
        surface_w_,
        surface_h_,
        static_cast<DisplayRotation>(Orientation & 3),
        displayDensity);

    ImDrawData* drawData = ImGui::GetDrawData();
    bool skippedGpu = false;

    if (backend_) {
        if (!allowIdleSkip) {
            // Active ESP mode: keep layer visible and submit immediately for minimum latency.
            if (!window_.IsVisible()) {
                (void)window_.SetVisible(true);
            }
            backend_->Render(drawData);
            lastFrameFingerprint_ = 0; // Next paused frame always refreshes cleanly.
        } else if (isDrawDataEmpty(drawData)) {
            if (window_.IsVisible()) {
                (void)window_.SetVisible(false);
            }
            lastFrameFingerprint_ = 0;
            skippedGpu = true;
        } else {
            if (!window_.IsVisible()) {
                (void)window_.SetVisible(true);
                lastFrameFingerprint_ = 0;
            }
            const uint64_t fp = computeDrawDataFingerprint(drawData);
            const Clock::time_point now = Clock::now();
            const bool settled = (now >= settleUntil_) && !frameHadInput_ && !frameWasScrolling_;
            if (settled && lastFrameFingerprint_ != 0 && fp == lastFrameFingerprint_) {
                skippedGpu = true;
            } else {
                lastFrameFingerprint_ = fp;
                backend_->Render(drawData);
            }
        }
    }

    paceFrame(targetFps_, skippedGpu);
}

void ImGuiCfg::Shutdown() {
    StopTouch();
    shutdownGraphicsOnly();
}

bool ImGuiCfg::ImGui_init() {
    if (g_Initialized) {
        return true;
    }
    if (!backend_ || !window_.IsValid()) {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendPlatformName = "esp4all-android";
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.DisplaySize = ImVec2(static_cast<float>(surface_w_), static_cast<float>(surface_h_));

    if (!backend_->SetupImGui()) {
        ImGui::DestroyContext();
        return false;
    }

    loadAndroidSystemFont(io);

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    const float density = std::clamp((displayDensity > 0.5f) ? displayDensity : 2.5f, 1.0f, 5.0f);
    style.FontSizeBase = 14.0f;
    style.FontScaleDpi = density;
    style.FontScaleMain = 1.0f;
    style.ScaleAllSizes(density);
    style.WindowRounding = 6.0f * density;
    style.FrameRounding = 4.0f * density;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;
    io.MouseDragThreshold = 6.0f * density;

    hasLastFrameTime_ = false;
    lastFrameFingerprint_ = 0;
    g_Initialized = true;
    return true;
}

void ImGuiCfg::shutdownGraphicsOnly() {
    if (g_Initialized) {
        if (backend_) {
            backend_->ShutdownImGui();
        }
        ImGui::DestroyContext();
        g_Initialized = false;
    }

    if (backend_) {
        backend_->Cleanup();
        backend_.reset();
    }

    window_.Destroy();
    lastFrameFingerprint_ = 0;
}
