#include "OverlayApp.hpp"
#include "ESP/EspRenderer.hpp"
#include <atomic>
#include <iostream>

namespace {
std::atomic<bool> gStopRequested{false};
} // namespace

void OverlayApp::RequestStop() noexcept {
    gStopRequested.store(true, std::memory_order_release);
}

OverlayApp::OverlayApp()
    : engine_(memory_) {}

OverlayApp::~OverlayApp() {
    Shutdown();
}

bool OverlayApp::AttachTargetProcess() {
    if (!engine_.IsConfigLoaded()) {
        statusMessage_ = "Config not loaded";
        return false;
    }

    const auto& cfg = engine_.GetConfig();
    const uint32_t pid = memory_.GetPid(cfg.GetPackageName());
    if (pid == 0) {
        statusMessage_ = "Process not found: " + cfg.GetPackageName();
        std::cout << "[OverlayApp] " << statusMessage_ << std::endl;
        return false;
    }

    const uint64_t base = memory_.GetBaseAddress(cfg.GetModuleName(), cfg.GetSegmentFilter());
    if (base == 0) {
        statusMessage_ = "Module not found: " + cfg.GetModuleName() + " [" + cfg.GetSegmentFilter() + "]";
        std::cout << "[OverlayApp] " << statusMessage_ << std::endl;
        return false;
    }

    statusMessage_ = "Attached PID " + std::to_string(pid);
    std::cout << "[OverlayApp] Attached PID=" << pid
              << " Base=0x" << std::hex << base << std::dec
              << " (" << cfg.GetModuleName() << " [" << cfg.GetSegmentFilter() << "])"
              << std::endl;
    return true;
}

bool OverlayApp::ReloadConfig() {
    if (!engine_.LoadConfig(settings_.ue4ConfigPath)) {
        statusMessage_ = "Config load failed";
        std::cout << "[OverlayApp] ReloadConfig failed: " << engine_.GetLastError() << std::endl;
        return false;
    }

    AttachTargetProcess();

    if (!engine_.Initialize()) {
        statusMessage_ = "Engine init failed";
        std::cout << "[OverlayApp] Engine initialize failed: " << engine_.GetLastError() << std::endl;
        return false;
    }

    statusMessage_ = "Config reloaded OK";
    std::cout << "[OverlayApp] Hot-reloaded config: " << settings_.ue4ConfigPath << std::endl;
    return true;
}

bool OverlayApp::Initialize() {
    settings_.Load();

    // Load UE4 JSON config once at startup
    if (engine_.LoadConfig(settings_.ue4ConfigPath)) {
        std::cout << "[OverlayApp] Config loaded: " << engine_.GetConfig().GetPackageName()
                  << " (" << engine_.GetConfig().GetModuleName()
                  << " [" << engine_.GetConfig().GetSegmentFilter() << "])" << std::endl;

        if (!AttachTargetProcess()) {
            std::cout << "[OverlayApp] Target process not ready yet; use 'Reattach PID' in UI." << std::endl;
        }
        engine_.Initialize();
    } else {
        statusMessage_ = "Push JSON to " + settings_.ue4ConfigPath + " and press Reload JSON";
        std::cout << "[OverlayApp] WARNING: " << engine_.GetLastError() << std::endl;
    }

    const auto preferredBackend = static_cast<GraphicsBackendType>(settings_.graphicsBackend);
    if (!overlay_.InitOverlay(preferredBackend, settings_.GetCaptureMode())) {
        std::cerr << "[OverlayApp] Failed to initialize ImGui / Graphics overlay" << std::endl;
        return false;
    }
    overlayInitialized_ = true;

    std::string touchErr;
    overlay_.StartTouch(settings_.touchControl, &touchErr);
    if (settings_.touchControl && !overlay_.IsTouchControlEnabled()) {
        settings_.touchControl = false;
        statusMessage_ = touchErr.empty() ? "Touch uinput unavailable (Read-Only)" : touchErr;
    }

    gStopRequested.store(false, std::memory_order_release);
    running_ = true;
    return true;
}

void OverlayApp::HandleUIActions(const DebugUIActions& actions) {
    if (actions.cycleMemoryMode) {
        memory_.CycleAccessMode();
        statusMessage_ = std::string("Memory mode: ") + MemoryAccessModeToString(memory_.GetAccessMode());
    }

    if (actions.captureModeChanged) {
        pendingCaptureChange_ = true;
    }

    if (actions.toggleTouchControl) {
        std::string touchErr;
        if (overlay_.SetTouchControl(settings_.touchControl, &touchErr)) {
            statusMessage_ = settings_.touchControl
                ? "Touch Intercept (uinput) enabled"
                : "Touch returned to Read-Only mode";
        } else {
            settings_.touchControl = false;
            statusMessage_ = touchErr.empty() ? "Failed to enable uinput touch control" : touchErr;
        }
    }

    if (actions.cycleGraphicsBackend) {
        const GraphicsBackendType nextBackend =
            (overlay_.GetActiveBackendType() == GraphicsBackendType::Vulkan)
                ? GraphicsBackendType::OpenGL
                : GraphicsBackendType::Vulkan;
        settings_.graphicsBackend = static_cast<uint8_t>(nextBackend);
        pendingBackend_ = nextBackend;
        pendingOverlayRecreate_ = true;
    }

    if (actions.reattachProcess) {
        AttachTargetProcess();
        if (engine_.IsInitialized()) {
            engine_.ProbeChainDiagnostics();
        }
    }

    if (actions.reloadJson) {
        ReloadConfig();
    }

    if (actions.initializeEngine) {
        if (!engine_.IsConfigLoaded()) {
            engine_.LoadConfig(settings_.ue4ConfigPath);
        }
        if (memory_.GetPid() == 0 || memory_.GetModuleBase() == 0) {
            AttachTargetProcess();
        }
        if (engine_.Initialize()) {
            statusMessage_ = "Engine initialized";
        } else {
            statusMessage_ = "Engine init failed";
        }
    }

    if (actions.toggleEsp) {
        espRunning_ = !espRunning_;
        statusMessage_ = espRunning_ ? "ESP running" : "ESP paused";
    }

    if (actions.saveSettings) {
        statusMessage_ = settings_.Save() ? "UI settings saved" : "Failed to save UI settings";
    }

    if (actions.requestExit) {
        running_ = false;
    }
}

int OverlayApp::Run() {
    uint64_t frameIndex = 0;

    while (running_ && !gStopRequested.load(std::memory_order_acquire)) {
        overlay_.StartFrame(static_cast<int>(settings_.targetFps));

        if (espRunning_ && engine_.IsInitialized()) {
            engine_.Update(overlay_.screen_x, overlay_.screen_y);
            EspRenderer::Render(
                ImGui::GetBackgroundDrawList(),
                engine_,
                settings_,
                overlay_.screen_x,
                overlay_.screen_y);

            // Rate-limited console telemetry (~1/s at 60 FPS)
            if (frameIndex % 60 == 0) {
                const auto& d = engine_.GetContext().GetDiagnostics();
                const auto& s = engine_.GetStats();
                std::cout << "[ESP] UWorld=0x" << std::hex << d.uWorld
                          << (d.usedGEngineFallback ? "(GEngine)" : "(GWorld)")
                          << " Level=0x" << d.persistentLevel
                          << " GS=0x" << d.gameState
                          << " Raw=" << std::dec << s.totalRawActors
                          << " Pool=" << s.filteredEntities << "/" << s.poolCapacity
                          << " Drawn=" << s.drawnPlayers
                          << " Items=" << s.drawnWorldItems
                          << " Update=" << s.updateMs << "ms"
                          << " Cam=" << (d.usingMatrixCamera ? "ViewMatrix" : (d.hasCameraPOV ? "CameraCache" : "NONE"))
                          << std::endl;
            }
        } else if (engine_.IsInitialized() && (frameIndex % 15 == 0)) {
            // Keep Chain Debugger live while ESP is paused
            engine_.ProbeChainDiagnostics();
        }

        const DebugUIActions actions = debugUI_.Render(
            memory_,
            MemoryAccessModeToString(memory_.GetAccessMode()),
            engine_,
            settings_,
            espRunning_,
            overlay_.GetActiveBackendName(),
            overlay_.GetTouchStatusSummary(),
            statusMessage_,
            "External");
        HandleUIActions(actions);

        overlay_.EndFrame(/*allowIdleSkip=*/!espRunning_);

        // Apply deferred surface/backend changes safely between frames (never mid-ImGui frame)
        if (pendingOverlayRecreate_) {
            pendingOverlayRecreate_ = false;
            pendingCaptureChange_ = false;
            if (overlay_.RecreateOverlay(pendingBackend_, settings_.GetCaptureMode())) {
                statusMessage_ = std::string("Overlay: ") + overlay_.GetActiveBackendName() +
                                 " | Capture: " + CaptureModeToString(overlay_.GetCaptureMode());
            } else {
                statusMessage_ = "Failed to recreate overlay surface/backend";
                running_ = false;
            }
        } else if (pendingCaptureChange_) {
            pendingCaptureChange_ = false;
            if (overlay_.SetCaptureMode(settings_.GetCaptureMode())) {
                statusMessage_ = std::string("Capture: ") + CaptureModeToString(overlay_.GetCaptureMode()) +
                                 " (" + overlay_.GetActiveBackendName() + ")";
            } else {
                statusMessage_ = "Failed to switch Capture mode";
                running_ = false;
            }
        }

        ++frameIndex;
    }

    Shutdown();
    return 0;
}

void OverlayApp::Shutdown() {
    if (overlayInitialized_) {
        overlay_.Shutdown();
        overlayInitialized_ = false;
    }
    settings_.Save();
}
