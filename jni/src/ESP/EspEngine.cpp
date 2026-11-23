#include "ESP/EspEngine.hpp"
#include "ESP/GameStatePlayerEntityProvider.hpp"
#include "ESP/WorldActorsEntityProvider.hpp"
#include <chrono>
#include <iostream>

EspEngine::EspEngine(IMemoryReader& memory)
    : context_(memory) {}

bool EspEngine::LoadConfig(const std::string& configPath) {
    if (!config_.Load(configPath)) {
        lastError_ = config_.GetLastError();
        return false;
    }
    lastError_.clear();
    return true;
}

bool EspEngine::LoadConfigFromString(const std::string& jsonText) {
    if (!config_.LoadFromString(jsonText)) {
        lastError_ = config_.GetLastError();
        return false;
    }
    lastError_.clear();
    return true;
}

bool EspEngine::Initialize() {
    initialized_ = false;
    provider_.reset();
    doublePool_.ClearAll();
    stats_ = EspFrameStats{};

    if (!config_.IsLoaded()) {
        lastError_ = "Config is not loaded";
        return false;
    }

    // In external mode (no InternalEngineBridge overrides), surface config validation errors early
    if (context_.GetInternalBridge() == nullptr && !config_.GetReport().ok()) {
        for (const auto& issue : config_.GetReport().issues) {
            if (issue.level == ConfigIssue::Level::Error) {
                lastError_ = "[" + issue.code + "] " + issue.path + ": " + issue.msg;
                break;
            }
        }
        return false;
    }

    if (!context_.Initialize(config_)) {
        lastError_ = "Failed to initialize UE4Context";
        return false;
    }

    const std::string& providerType = config_.GetProviderType();
    if (providerType == "GameStatePlayer") {
        auto gs = std::make_unique<GameStatePlayerEntityProvider>(context_);
        if (!gs->Initialize()) {
            lastError_ = "Failed to initialize GameStatePlayerEntityProvider (check GameState/PlayerArray/PawnPrivate offsets)";
            return false;
        }
        provider_ = std::move(gs);
    } else {
        auto wa = std::make_unique<WorldActorsEntityProvider>(context_);
        if (!wa->Initialize()) {
            lastError_ = "Failed to initialize WorldActorsEntityProvider (check PersistentLevel/LevelActors or EntityList_chain)";
            return false;
        }
        provider_ = std::move(wa);
    }

    initialized_ = true;
    lastError_.clear();
    return true;
}

bool EspEngine::ReloadConfig(const std::string& configPath) {
    if (!LoadConfig(configPath)) {
        return false;
    }
    return Initialize();
}

void EspEngine::ProbeChainDiagnostics() {
    if (!context_.IsInitialized()) {
        return;
    }
    context_.BeginFrame();
    stats_.hasValidWorld = context_.HasValidWorld();
    stats_.hasValidCamera = context_.HasValidCamera();
}

void EspEngine::Update(int screenW, int screenH) {
    const auto tStart = std::chrono::steady_clock::now();

    EntityPool& writePool = doublePool_.AcquireWriteBuffer();
    writePool.Clear();
    stats_ = EspFrameStats{};

    if (!initialized_ || !provider_) {
        doublePool_.PublishWriteBuffer();
        return;
    }

    context_.BeginFrame();

    const size_t collected = provider_->CollectEntities(writePool);
    const auto& diag = context_.GetDiagnostics();

    stats_.hasValidWorld = context_.HasValidWorld();
    stats_.hasValidCamera = context_.HasValidCamera();
    stats_.totalRawActors = (diag.actorsCount > 0) ? static_cast<uint32_t>(diag.actorsCount) : 0U;
    stats_.filteredEntities = static_cast<uint32_t>(collected);
    stats_.droppedByPoolCap = diag.droppedByPoolCap;

    if (stats_.hasValidCamera && screenW > 0 && screenH > 0) {
        uint32_t playersOnScreen = 0;
        uint32_t itemsOnScreen = 0;

        for (auto& entity : writePool) {
            if (!entity.isValid) {
                continue;
            }

            if (entity.isWorldItem) {
                if (context_.ProjectWorldToScreen(entity.worldPos, screenW, screenH, entity.screenPoint)) {
                    entity.onScreen = true;
                    ++itemsOnScreen;
                }
            } else {
                if (context_.ProjectActorBox(entity.worldPos, screenW, screenH, entity.box)) {
                    entity.onScreen = true;
                    ++playersOnScreen;
                }

                if (entity.hasSkeleton) {
                    for (size_t b = 0; b < BONE_SLOT_COUNT; ++b) {
                        if (entity.boneValid[b] &&
                            context_.ProjectWorldToScreen(entity.boneWorld[b], screenW, screenH, entity.boneScreen[b])) {
                            entity.boneOnScreen[b] = 1;
                        } else {
                            entity.boneOnScreen[b] = 0;
                        }
                    }
                }
            }
        }

        stats_.drawnPlayers = playersOnScreen;
        stats_.drawnWorldItems = itemsOnScreen;
    }

    doublePool_.PublishWriteBuffer();

    const auto tEnd = std::chrono::steady_clock::now();
    stats_.updateMs = std::chrono::duration<float, std::milli>(tEnd - tStart).count();
}
