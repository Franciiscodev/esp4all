#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "Config/ConfigManager.hpp"
#include "ESP/IEntityProvider.hpp"
#include "IMemoryReader.hpp"
#include "UE4/InternalBridge.hpp"
#include "UE4/UE4Context.hpp"

/**
 * Per-frame ESP pipeline telemetry (displayed live in DebugUI Chain Debugger).
 */
struct EspFrameStats {
    uint32_t totalRawActors = 0;      // Actors.Num / PlayerArray.Num
    uint32_t filteredEntities = 0;    // Entities collected into pool
    uint32_t droppedByPoolCap = 0;    // Valid actors skipped after pool reached CAPACITY (256)
    uint32_t drawnPlayers = 0;        // On-screen projected players/actors
    uint32_t drawnWorldItems = 0;     // On-screen projected vehicles/loot
    uint32_t poolCapacity = static_cast<uint32_t>(EntityPool::CAPACITY);
    float updateMs = 0.0f;            // Time spent in EspEngine::Update()
    bool hasValidWorld = false;
    bool hasValidCamera = false;
};

/**
 * Platform-agnostic ESP Engine with a lock-free Double-Buffered Object Pool.
 *
 * Works identically in:
 *  - External mode (MemoryTool: process_vm_readv / pread64 / driver)
 *  - Internal mode (DirectMemoryReader + optional InternalEngineBridge overrides)
 */
class EspEngine {
public:
    explicit EspEngine(IMemoryReader& memory);

    /**
     * Attach an optional in-process InternalEngineBridge for pushed Camera / World /
     * LocalPawn / UClass* overrides (zero offset traversal when fresh, automatic fallback).
     */
    void SetInternalBridge(InternalEngineBridge* bridge) noexcept {
        context_.SetInternalBridge(bridge);
    }

    InternalEngineBridge* GetInternalBridge() const noexcept {
        return context_.GetInternalBridge();
    }

    bool LoadConfig(const std::string& configPath);
    bool LoadConfigFromString(const std::string& jsonText);
    bool Initialize();
    bool ReloadConfig(const std::string& configPath);

    /**
     * Zero-allocation update writing into the back buffer and atomically publishing
     * it to readers (thread-safe for separate Engine Tick & ImGui Render threads).
     */
    void Update(int screenW, int screenH);

    void ProbeChainDiagnostics();

    bool IsConfigLoaded() const noexcept { return config_.IsLoaded(); }
    bool IsInitialized() const noexcept { return initialized_ && provider_ != nullptr; }
    const std::string& GetLastError() const noexcept { return lastError_; }

    const ConfigManager& GetConfig() const noexcept { return config_; }
    const UE4Context& GetContext() const noexcept { return context_; }
    const EntityPool& GetPool() const noexcept { return doublePool_.GetReadBuffer(); }
    const EspFrameStats& GetStats() const noexcept { return stats_; }

private:
    ConfigManager config_;
    UE4Context context_;
    std::unique_ptr<IEntityProvider> provider_;
    DoubleBufferedEntityPool doublePool_;
    EspFrameStats stats_;
    bool initialized_ = false;
    std::string lastError_;
};
