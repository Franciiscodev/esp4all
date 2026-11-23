#pragma once
#include <string>
#include <draw.h>
#include "ESP/EspEngine.hpp"
#include "ESP/EspSettings.hpp"
#include "Memory.hpp"
#include "UI/DebugUI.hpp"

/**
 * Top-level Overlay Application using strict composition (zero diamond inheritance).
 *
 * Composes:
 *  - ImGuiCfg   (Android Surface / EGL / ImGui / Touch)
 *  - MemoryTool (process_vm_readv / pread64 / kernel driver backends)
 *  - EspEngine  (UE4Context + EntityProvider + zero-alloc EntityPool)
 *  - DebugUI    (real-time Chain Debugger & control window)
 *  - EspSettings
 */
class OverlayApp {
public:
    OverlayApp();
    ~OverlayApp();

    OverlayApp(const OverlayApp&) = delete;
    OverlayApp& operator=(const OverlayApp&) = delete;

    bool Initialize();
    int Run();
    void Shutdown();
    static void RequestStop() noexcept;

    bool AttachTargetProcess();
    bool ReloadConfig();

private:
    ImGuiCfg overlay_;
    MemoryTool memory_;
    EspEngine engine_;
    DebugUI debugUI_;
    EspSettings settings_;

    bool running_ = false;
    bool espRunning_ = false;
    bool overlayInitialized_ = false;
    bool pendingOverlayRecreate_ = false;
    bool pendingCaptureChange_ = false;
    GraphicsBackendType pendingBackend_ = GraphicsBackendType::Auto;
    std::string statusMessage_;

    void HandleUIActions(const DebugUIActions& actions);
};
