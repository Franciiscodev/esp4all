#pragma once
#include <string>
#include "ESP/EspEngine.hpp"
#include "ESP/EspSettings.hpp"
#include "IMemoryReader.hpp"

/**
 * Actions emitted by DebugUI back to OverlayApp or InternalOverlayHost
 * (clean decoupling, no circular ownership).
 */
struct DebugUIActions {
    bool reloadJson = false;
    bool reattachProcess = false;
    bool cycleMemoryMode = false;
    bool cycleGraphicsBackend = false;
    bool captureModeChanged = false;
    bool toggleTouchControl = false;
    bool initializeEngine = false;
    bool toggleEsp = false;
    bool saveSettings = false;
    bool requestExit = false;
};

/**
 * ImGui Debug & Chain Debugger Panel.
 * Displays live per-link pointer status (PID -> Base -> GWorld -> Level/GS -> Actors -> Camera),
 * filtered/drawn counts, hot-reload / reattach buttons, graphics backend switcher,
 * capture mode selector, touch interception toggle, and ESP toggles.
 */
class DebugUI {
public:
    DebugUIActions Render(
        const IMemoryReader& memory,
        const char* memoryModeName,
        const EspEngine& engine,
        EspSettings& settings,
        bool espRunning,
        const char* graphicsBackendName,
        const std::string& touchStatus,
        const std::string& statusMessage,
        const char* hostModeTag = "External");

private:
    static void DrawStatusBadge(bool ok, const char* okText = "OK", const char* failText = "FAIL");
    static void DrawChainLink(const char* label, uint64_t ptr, const char* extra = nullptr);
};
