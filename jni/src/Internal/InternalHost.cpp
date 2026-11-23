#include "Internal/InternalHost.hpp"
#include "ESP/EspRenderer.hpp"
#include <android/log.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ESP4AllInternal", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "ESP4AllInternal", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ESP4AllInternal", __VA_ARGS__)

namespace {
std::mutex gLifecycleMutex;
std::thread gWorkerThread;
InternalOverlayHost* gActiveHost = nullptr;
std::atomic<bool> gWorkerRunning{false};
std::atomic<bool> gGlobalStop{false};
} // namespace

InternalOverlayHost::InternalOverlayHost()
    : memory_(DirectReadMode::SAFE_SELF_VM),
      engine_(memory_) {
    engine_.SetInternalBridge(&bridge_);
}

InternalOverlayHost::~InternalOverlayHost() {
    Shutdown();
}

void InternalOverlayHost::RequestStop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
}

bool InternalOverlayHost::AttachSelfModule() {
    if (!engine_.IsConfigLoaded()) {
        statusMessage_ = "Config not loaded";
        return false;
    }

    const auto& cfg = engine_.GetConfig();
    const auto& internalCfg = cfg.GetInternalConfig();
    memory_.SetMode(internalCfg.safeMemoryRead
        ? DirectReadMode::SAFE_SELF_VM
        : DirectReadMode::DIRECT_MEMCPY);

    const uint64_t base = memory_.AttachSelfModule(cfg.GetModuleName(), cfg.GetSegmentFilter());
    if (base == 0) {
        statusMessage_ = "Module not loaded: " + cfg.GetModuleName() + " [" + cfg.GetSegmentFilter() + "]";
        LOGW("%s", statusMessage_.c_str());
        return false;
    }

    ConfigureInternalHooks();

    statusMessage_ = "Attached self PID " + std::to_string(memory_.GetPid());
    LOGI("Attached self PID=%u Base=0x%llX (%s [%s]) Mem=%s",
         memory_.GetPid(),
         static_cast<unsigned long long>(base),
         cfg.GetModuleName().c_str(),
         cfg.GetSegmentFilter().c_str(),
         DirectReadModeToString(memory_.GetMode()));
    return true;
}

void InternalOverlayHost::ConfigureInternalHooks() {
    bridge_.ClearAll();
    actorArrayFnAddr_.store(0, std::memory_order_release);
    cachedGNamesFnAddr_ = 0;
    cachedViewportObj_ = 0;

    if (!engine_.IsConfigLoaded()) {
        return;
    }

    const uint64_t base = memory_.GetModuleBase();
    if (!MemoryUtils::IsValidPtr(base, 1)) {
        return;
    }

    const auto& cfg = engine_.GetConfig();
    const auto& internalCfg = cfg.GetInternalConfig();
    cachedActorMaxCount_ = cfg.GetOffsets().entityArrayMaxCount;
    if (cachedActorMaxCount_ == 0 || cachedActorMaxCount_ > 10000) {
        cachedActorMaxCount_ = 3000;
    }

    // 1. Optional native GetActorArray(ULevel*) -> TArray<AActor*>*
    if (internalCfg.getActorArrayFnOffset != 0) {
        const uint64_t fnAddr = base + internalCfg.getActorArrayFnOffset;
        if (memory_.IsExecutableAddress(fnAddr)) {
            actorArrayFnAddr_.store(fnAddr, std::memory_order_release);
            bridge_.SetActorsArrayResolver(&InternalOverlayHost::ResolveActorsViaGetter, this);
            LOGI("Registered internal.GetActorArray_fn @ 0x%llX (+0x%llX)",
                 static_cast<unsigned long long>(fnAddr),
                 static_cast<unsigned long long>(internalCfg.getActorArrayFnOffset));
        } else {
            LOGW("Skipped internal.GetActorArray_fn (+0x%llX): target 0x%llX is not in an executable segment",
                 static_cast<unsigned long long>(internalCfg.getActorArrayFnOffset),
                 static_cast<unsigned long long>(fnAddr));
        }
    }

    // 2. Optional native GetGNames() / GetNamePool() -> void*
    if (internalCfg.gNamesGetterOffset != 0) {
        const uint64_t fnAddr = base + internalCfg.gNamesGetterOffset;
        if (memory_.IsExecutableAddress(fnAddr)) {
            cachedGNamesFnAddr_ = fnAddr;
            TryResolveGNamesViaGetter();
            LOGI("Configured internal.GNames_getter @ 0x%llX (+0x%llX)",
                 static_cast<unsigned long long>(fnAddr),
                 static_cast<unsigned long long>(internalCfg.gNamesGetterOffset));
        } else {
            LOGW("Skipped internal.GNames_getter (+0x%llX): target 0x%llX is not in an executable segment",
                 static_cast<unsigned long long>(internalCfg.gNamesGetterOffset),
                 static_cast<unsigned long long>(fnAddr));
        }
    }
}

bool InternalOverlayHost::TryResolveGNamesViaGetter() {
    if (cachedGNamesFnAddr_ == 0 || !engine_.IsConfigLoaded()) {
        return false;
    }

    using GNamesGetterFn = uintptr_t (*)();
    const auto fn = reinterpret_cast<GNamesGetterFn>(static_cast<uintptr_t>(cachedGNamesFnAddr_));
    const uint64_t rawPtr = MemoryUtils::StripTag(static_cast<uint64_t>(fn()));
    if (!MemoryUtils::IsValidPtr(rawPtr)) {
        return false;
    }

    const auto& internalCfg = engine_.GetConfig().GetInternalConfig();
    uint64_t resolved = rawPtr + internalCfg.gNamesGetterAddOffset;
    if (internalCfg.gNamesGetterDeref) {
        resolved = memory_.ReadPointer(resolved);
    }
    if (!MemoryUtils::IsValidPtr(resolved)) {
        return false;
    }

    // Validate readability through DirectMemoryReader (SAFE_SELF_VM catches bad addresses cleanly)
    uint64_t probe = 0;
    if (!memory_.ReadInto<uint64_t>(resolved, probe)) {
        return false;
    }

    bridge_.PushNamePool(resolved);
    return true;
}

bool InternalOverlayHost::ResolveActorsViaGetter(
    uint64_t levelPtr,
    uint64_t& outActorsData,
    int32_t& outActorsCount,
    void* userData) noexcept
{
    outActorsData = 0;
    outActorsCount = 0;
    if (userData == nullptr || !MemoryUtils::IsValidPtr(levelPtr)) {
        return false;
    }

    auto* self = static_cast<InternalOverlayHost*>(userData);
    const uint64_t fnAddr = self->actorArrayFnAddr_.load(std::memory_order_acquire);
    if (fnAddr == 0) {
        return false;
    }

    using GetActorArrayFn = uintptr_t (*)(uintptr_t);
    const auto fn = reinterpret_cast<GetActorArrayFn>(static_cast<uintptr_t>(fnAddr));
    const uint64_t tarrayPtr = MemoryUtils::StripTag(
        static_cast<uint64_t>(fn(static_cast<uintptr_t>(levelPtr))));
    if (!MemoryUtils::IsValidPtr(tarrayPtr)) {
        return false;
    }

    // Read TArray<AActor*> { Data, Count } via DirectMemoryReader (SAFE_SELF_VM-protected)
    const uint64_t dataPtr = self->memory_.ReadPointer(tarrayPtr + UE4Constants::TArray_Data);
    const int32_t count = self->memory_.Read<int32_t>(tarrayPtr + UE4Constants::TArray_Count);
    const int32_t maxCount = static_cast<int32_t>(self->cachedActorMaxCount_);
    if (!MemoryUtils::IsValidPtr(dataPtr) || count <= 0 || count > maxCount) {
        return false;
    }

    outActorsData = dataPtr;
    outActorsCount = count;
    return true;
}

bool InternalOverlayHost::TryResolveWorldViaGUObjectArray(uint64_t frameIndex) {
    if (!engine_.IsConfigLoaded() || !engine_.IsInitialized()) {
        return false;
    }

    const auto& cfg = engine_.GetConfig();
    const auto& internalCfg = cfg.GetInternalConfig();
    const uint32_t vpWorldOff = cfg.GetOffsets().GameViewportClient_World;
    const uint64_t base = memory_.GetModuleBase();

    if (internalCfg.gUObjectArrayOffset == 0 || vpWorldOff == 0 || !MemoryUtils::IsValidPtr(base, 1)) {
        return false;
    }

    // Fast path: cached UGameViewportClient* -> World (1 safe read per frame, 0 allocations)
    if (MemoryUtils::IsValidPtr(cachedViewportObj_)) {
        const uint64_t world = memory_.ReadPointer(cachedViewportObj_ + vpWorldOff);
        if (MemoryUtils::IsValidPtr(world)) {
            bridge_.PushUWorld(world);
            return true;
        }
        // Viewport cleared/recreated (e.g. map transition) — clear stale override & rescan below
        cachedViewportObj_ = 0;
        bridge_.PushUWorld(0);
    }

    // Rate-limit full GUObjectArray scan to once every 60 frames (~1 Hz)
    if (frameIndex % 60 != 0) {
        return false;
    }

    const auto& ctx = engine_.GetContext();
    if (!ctx.IsNameSystemEnabled()) {
        return false;
    }

    const uint64_t objArrayAddr = base + internalCfg.gUObjectArrayOffset;
    const uint64_t objectsBase = memory_.ReadPointer(objArrayAddr);
    const int32_t numElements = memory_.Read<int32_t>(objArrayAddr + internalCfg.uObjectArrayNumOffset);
    if (!MemoryUtils::IsValidPtr(objectsBase) || numElements <= 0) {
        return false;
    }

    const int32_t maxScan = (numElements > 250000) ? 250000 : numElements;
    const uint32_t elemSize = internalCfg.uObjectArrayElementSize;
    const uint32_t chunkSize = (internalCfg.uObjectArrayChunkSize > 0)
        ? internalCfg.uObjectArrayChunkSize
        : 0x10000U;
    const std::string& targetNeedle = internalCfg.viewportClassName;
    if (targetNeedle.empty()) {
        return false;
    }

    uint64_t cachedChunkIdx = UINT64_MAX;
    uint64_t cachedChunkPtr = 0;
    char nameBuf[96] = {};

    for (int32_t i = 0; i < maxScan; ++i) {
        uint64_t itemAddr = 0;
        if (internalCfg.uObjectArrayChunked) {
            const uint64_t chunkIdx = static_cast<uint64_t>(i) / chunkSize;
            const uint64_t withinChunk = static_cast<uint64_t>(i) % chunkSize;
            if (chunkIdx != cachedChunkIdx) {
                cachedChunkIdx = chunkIdx;
                cachedChunkPtr = memory_.ReadPointer(objectsBase + chunkIdx * sizeof(uint64_t));
            }
            if (!MemoryUtils::IsValidPtr(cachedChunkPtr)) {
                continue;
            }
            itemAddr = cachedChunkPtr + withinChunk * elemSize;
        } else {
            itemAddr = objectsBase + static_cast<uint64_t>(i) * elemSize;
        }

        const uint64_t objPtr = memory_.ReadPointer(itemAddr);
        if (!MemoryUtils::IsValidPtr(objPtr)) {
            continue;
        }

        // Check object name first, then ClassPrivate name (covers both instances & subclasses)
        bool matched = false;
        if (ctx.ReadObjectNameInto(objPtr, nameBuf, sizeof(nameBuf)) &&
            std::strstr(nameBuf, targetNeedle.c_str()) != nullptr) {
            matched = true;
        } else {
            const uint64_t clsPtr = ctx.ReadClassPrivate(objPtr);
            if (MemoryUtils::IsValidPtr(clsPtr) &&
                ctx.ReadObjectNameInto(clsPtr, nameBuf, sizeof(nameBuf)) &&
                std::strstr(nameBuf, targetNeedle.c_str()) != nullptr) {
                matched = true;
            }
        }

        if (!matched) {
            continue;
        }

        const uint64_t world = memory_.ReadPointer(objPtr + vpWorldOff);
        if (MemoryUtils::IsValidPtr(world)) {
            cachedViewportObj_ = objPtr;
            bridge_.PushUWorld(world);
            LOGI("Resolved UWorld=0x%llX via GUObjectArray viewport=0x%llX (%s)",
                 static_cast<unsigned long long>(world),
                 static_cast<unsigned long long>(objPtr),
                 nameBuf);
            return true;
        }
    }

    return false;
}

void InternalOverlayHost::RefreshInternalOverrides(uint64_t frameIndex) {
    if (!engine_.IsConfigLoaded()) {
        return;
    }

    // Lazily retry GNames_getter if engine hadn't initialized GNames yet during attach
    if (cachedGNamesFnAddr_ != 0 && bridge_.GetNamePoolOverride() == 0 && (frameIndex % 30 == 0)) {
        TryResolveGNamesViaGetter();
    }

    // Resolve UWorld via GUObjectArray -> GameViewportClient -> World when configured
    const auto& internalCfg = engine_.GetConfig().GetInternalConfig();
    if (internalCfg.gUObjectArrayOffset != 0) {
        const auto& diag = engine_.GetContext().GetDiagnostics();
        if (cachedViewportObj_ != 0 || !MemoryUtils::IsValidPtr(diag.uWorld)) {
            TryResolveWorldViaGUObjectArray(frameIndex);
        }
    }
}

bool InternalOverlayHost::ReloadConfig() {
    if (!engine_.LoadConfig(settings_.ue4ConfigPath)) {
        statusMessage_ = "Config load failed";
        LOGW("ReloadConfig failed: %s", engine_.GetLastError().c_str());
        return false;
    }

    AttachSelfModule();

    if (!engine_.Initialize()) {
        statusMessage_ = "Engine init failed";
        LOGW("Engine initialize failed: %s", engine_.GetLastError().c_str());
        return false;
    }

    statusMessage_ = "Config reloaded OK";
    LOGI("Hot-reloaded config: %s", settings_.ue4ConfigPath.c_str());
    return true;
}

bool InternalOverlayHost::Initialize(const std::string& configPathOverride) {
    settings_.Load();
    if (!configPathOverride.empty()) {
        settings_.ue4ConfigPath = configPathOverride;
    }

    if (engine_.LoadConfig(settings_.ue4ConfigPath)) {
        const auto& cfg = engine_.GetConfig();
        LOGI("Config loaded: %s (%s [%s])",
             cfg.GetPackageName().c_str(),
             cfg.GetModuleName().c_str(),
             cfg.GetSegmentFilter().c_str());

        // Wait briefly for target game library (e.g. libUE4.so) to finish loading inside host process
        for (int attempt = 0; attempt < 40 && !stopRequested_.load(std::memory_order_acquire); ++attempt) {
            if (AttachSelfModule()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        engine_.Initialize();
    } else {
        statusMessage_ = "Push JSON to " + settings_.ue4ConfigPath + " and press Reload JSON";
        LOGW("Config warning: %s", engine_.GetLastError().c_str());
    }

    const auto preferredBackend = static_cast<GraphicsBackendType>(settings_.graphicsBackend);
    if (!overlay_.InitOverlay(preferredBackend, settings_.GetCaptureMode())) {
        LOGE("Failed to initialize ImGui / Graphics overlay");
        return false;
    }
    overlayInitialized_ = true;

    std::string touchErr;
    overlay_.StartTouch(settings_.touchControl, &touchErr);
    if (settings_.touchControl && !overlay_.IsTouchControlEnabled()) {
        settings_.touchControl = false;
        statusMessage_ = touchErr.empty() ? "Touch uinput unavailable (Read-Only)" : touchErr;
    }

    stopRequested_.store(false, std::memory_order_release);
    running_ = true;
    return true;
}

void InternalOverlayHost::HandleUIActions(const DebugUIActions& actions) {
    if (actions.cycleMemoryMode) {
        memory_.CycleMode();
        statusMessage_ = std::string("Memory mode: ") + DirectReadModeToString(memory_.GetMode());
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
        AttachSelfModule();
        if (engine_.IsInitialized()) {
            RefreshInternalOverrides(0);
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
        if (memory_.GetModuleBase() == 0) {
            AttachSelfModule();
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

int InternalOverlayHost::Run() {
    uint64_t frameIndex = 0;

    while (running_ &&
           !stopRequested_.load(std::memory_order_acquire) &&
           !gGlobalStop.load(std::memory_order_acquire)) {
        overlay_.StartFrame(static_cast<int>(settings_.targetFps));

        if (espRunning_ && engine_.IsInitialized()) {
            RefreshInternalOverrides(frameIndex);
            engine_.Update(overlay_.screen_x, overlay_.screen_y);
            EspRenderer::Render(
                ImGui::GetBackgroundDrawList(),
                engine_,
                settings_,
                overlay_.screen_x,
                overlay_.screen_y);

            if (frameIndex % 120 == 0) {
                const auto& d = engine_.GetContext().GetDiagnostics();
                const auto& s = engine_.GetStats();
                LOGI("UWorld=0x%llX(%s) Level=0x%llX Raw=%u Pool=%u/%u Drawn=%u Update=%.2fms Cam=%s",
                     static_cast<unsigned long long>(d.uWorld),
                     d.usedBridgeWorld ? "Bridge" : (d.usedGEngineFallback ? "GEngine" : "GWorld"),
                     static_cast<unsigned long long>(d.persistentLevel),
                     s.totalRawActors,
                     s.filteredEntities,
                     s.poolCapacity,
                     s.drawnPlayers,
                     s.updateMs,
                     d.usingMatrixCamera ? "ViewMatrix" : (d.hasCameraPOV ? "CameraCache" : "NONE"));
            }
        } else if (engine_.IsInitialized() && (frameIndex % 15 == 0)) {
            RefreshInternalOverrides(frameIndex);
            engine_.ProbeChainDiagnostics();
        }

        const DebugUIActions actions = debugUI_.Render(
            memory_,
            DirectReadModeToString(memory_.GetMode()),
            engine_,
            settings_,
            espRunning_,
            overlay_.GetActiveBackendName(),
            overlay_.GetTouchStatusSummary(),
            statusMessage_,
            "Internal (.so)");
        HandleUIActions(actions);

        overlay_.EndFrame(/*allowIdleSkip=*/!espRunning_);

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

void InternalOverlayHost::Shutdown() {
    bridge_.ClearAll();
    actorArrayFnAddr_.store(0, std::memory_order_release);
    if (overlayInitialized_) {
        overlay_.Shutdown();
        overlayInitialized_ = false;
    }
    settings_.Save();
}

// =============================================================================
// Exported C Lifecycle API + Constructor/Destructor Auto-Start
// =============================================================================

extern "C" bool ESP4All_InternalStart(const char* configPathOverride) {
    std::lock_guard<std::mutex> lock(gLifecycleMutex);
    if (gWorkerRunning.load(std::memory_order_acquire)) {
        return true;
    }
    if (gWorkerThread.joinable()) {
        gWorkerThread.join();
    }

    gGlobalStop.store(false, std::memory_order_release);
    std::string cfgPath = (configPathOverride != nullptr) ? std::string(configPathOverride) : std::string();
    gWorkerRunning.store(true, std::memory_order_release);

    gWorkerThread = std::thread([cfgPath]() {
        InternalOverlayHost host;
        {
            std::lock_guard<std::mutex> hostLock(gLifecycleMutex);
            gActiveHost = &host;
        }
        if (host.Initialize(cfgPath)) {
            host.Run();
        }
        {
            std::lock_guard<std::mutex> hostLock(gLifecycleMutex);
            gActiveHost = nullptr;
        }
        gWorkerRunning.store(false, std::memory_order_release);
    });
    return true;
}

extern "C" void ESP4All_InternalStop() {
    gGlobalStop.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(gLifecycleMutex);
        if (gActiveHost != nullptr) {
            gActiveHost->RequestStop();
        }
    }
    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lock(gLifecycleMutex);
        if (gWorkerThread.joinable() && gWorkerThread.get_id() != std::this_thread::get_id()) {
            toJoin = std::move(gWorkerThread);
        }
    }
    if (toJoin.joinable()) {
        toJoin.join();
    }
}

extern "C" bool ESP4All_InternalIsRunning() {
    return gWorkerRunning.load(std::memory_order_acquire);
}

__attribute__((constructor)) static void ESP4All_OnLibraryLoad() {
    const char* noAuto = std::getenv("ESP4ALL_NO_AUTOSTART");
    if (noAuto != nullptr && std::strcmp(noAuto, "1") == 0) {
        return;
    }
    ESP4All_InternalStart(nullptr);
}

__attribute__((destructor)) static void ESP4All_OnLibraryUnload() {
    ESP4All_InternalStop();
}
