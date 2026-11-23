#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <draw.h>
#include "DirectMemoryReader.hpp"
#include "ESP/EspEngine.hpp"
#include "ESP/EspSettings.hpp"
#include "UE4/InternalBridge.hpp"
#include "UI/DebugUI.hpp"

/**
 * In-process (shared library .so) host for ESP4All Core.
 *
 * Reuses the exact same platform-agnostic libesp4all_core.a engine, Vulkan/GLES
 * Dear ImGui 1.92 overlay, Android 5–17 NativeWindow, TouchManager, and DebugUI
 * as the standalone external executable, while running inside the target's address
 * space via DirectMemoryReader (SAFE_SELF_VM / DIRECT_MEMCPY) and feeding
 * JSON-configured UE4 getter functions (internal.GNames_getter,
 * internal.GetActorArray_fn, internal.GUObjectArray) into InternalEngineBridge.
 *
 * 100% data-driven — zero game-specific SDK headers or hardcoded offsets.
 */
class InternalOverlayHost {
public:
    InternalOverlayHost();
    ~InternalOverlayHost();

    InternalOverlayHost(const InternalOverlayHost&) = delete;
    InternalOverlayHost& operator=(const InternalOverlayHost&) = delete;

    bool Initialize(const std::string& configPathOverride = "");
    int Run();
    void Shutdown();
    void RequestStop() noexcept;

    bool AttachSelfModule();
    bool ReloadConfig();

private:
    ImGuiCfg overlay_;
    DirectMemoryReader memory_;
    InternalEngineBridge bridge_;
    EspEngine engine_;
    DebugUI debugUI_;
    EspSettings settings_;

    std::atomic<bool> stopRequested_{false};
    bool running_ = false;
    bool espRunning_ = false;
    bool overlayInitialized_ = false;
    bool pendingOverlayRecreate_ = false;
    bool pendingCaptureChange_ = false;
    GraphicsBackendType pendingBackend_ = GraphicsBackendType::Auto;
    std::string statusMessage_;

    // Cached resolved native getter addresses & viewport object (zero-alloc on hot path)
    std::atomic<uint64_t> actorArrayFnAddr_{0};
    uint64_t cachedGNamesFnAddr_ = 0;
    uint64_t cachedViewportObj_ = 0;
    uint32_t cachedActorMaxCount_ = 3000;

    void ConfigureInternalHooks();
    void RefreshInternalOverrides(uint64_t frameIndex);
    bool TryResolveGNamesViaGetter();
    bool TryResolveWorldViaGUObjectArray(uint64_t frameIndex);
    void HandleUIActions(const DebugUIActions& actions);

    static bool ResolveActorsViaGetter(
        uint64_t levelPtr,
        uint64_t& outActorsData,
        int32_t& outActorsCount,
        void* userData) noexcept;
};

extern "C" {
__attribute__((visibility("default"))) bool ESP4All_InternalStart(const char* configPathOverride);
__attribute__((visibility("default"))) void ESP4All_InternalStop();
__attribute__((visibility("default"))) bool ESP4All_InternalIsRunning();
}
