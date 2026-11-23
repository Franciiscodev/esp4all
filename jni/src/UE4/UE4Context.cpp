#include "UE4/UE4Context.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

UE4Context::UE4Context(IMemoryReader& memory)
    : memory_(memory) {
    // Pre-allocate reusable bone scratch buffer (up to 160 bones * 48B = 7.5 KB)
    boneRawBuf_.reserve(160 * UE4Constants::FTransform_Stride);
}

bool UE4Context::Initialize(const ConfigManager& config) {
    if (!config.IsLoaded()) {
        std::cerr << "[UE4Context] Cannot initialize with an unloaded ConfigManager" << std::endl;
        return false;
    }

    config_ = config;
    ClearCaches();

    if (config_.GetNamePoolConfig().enabled) {
        nameResolver_ = NameResolverFactory::Create(memory_, config_.GetNamePoolConfig());
    } else {
        nameResolver_.reset();
    }

    initialized_ = true;
    BeginFrame();
    return true;
}

void UE4Context::ClearCaches() {
    acceptedClasses_.clear();
    rejectedClasses_.clear();
    cachedHeuristicPC_ = 0;
    cachedPcClass_ = 0;
    usedHeuristicClassBootstrap_ = false;
    if (nameResolver_) {
        nameResolver_->ClearCache();
    }
}

bool UE4Context::ResolveChain(uint64_t startAddr, const PointerChain& chain, uint64_t& outAddr) const {
    outAddr = 0;
    if (chain.Empty()) {
        return false;
    }

    uint64_t current = MemoryUtils::StripTag(startAddr);
    if (!MemoryUtils::IsValidPtr(current, 1)) {
        return false;
    }

    for (const auto& step : chain.steps) {
        const int64_t signedAddr = static_cast<int64_t>(current) + step.offset;
        if (signedAddr <= 0) {
            return false;
        }
        const uint64_t target = static_cast<uint64_t>(signedAddr);
        if (!MemoryUtils::IsValidPtr(target, 1)) {
            return false;
        }

        if (step.deref) {
            uint64_t next = memory_.ReadPointer(target);
            if (!MemoryUtils::IsValidPtr(next)) {
                return false;
            }
            current = next;
        } else {
            current = target;
        }
    }

    outAddr = current;
    return true;
}

bool UE4Context::ResolveWorld() {
    uWorld_ = 0;
    diagnostics_.usedGEngineFallback = false;
    diagnostics_.usedBridgeWorld = false;

    // Priority 0: In-process pushed UWorld* override (zero offset traversal)
    if (bridge_ != nullptr) {
        const uint64_t overrideWorld = bridge_->GetUWorldOverride();
        if (MemoryUtils::IsValidPtr(overrideWorld)) {
            uWorld_ = overrideWorld;
            diagnostics_.usedBridgeWorld = true;
            return true;
        }
    }

    if (MemoryUtils::IsValidPtr(moduleBase_, 1) && !config_.GetGWorldChain().Empty()) {
        uint64_t candidate = 0;
        if (ResolveChain(moduleBase_, config_.GetGWorldChain(), candidate) &&
            MemoryUtils::IsValidPtr(candidate)) {
            uWorld_ = candidate;
            return true;
        }
    }

    const auto& offsets = config_.GetOffsets();
    if (MemoryUtils::IsValidPtr(moduleBase_, 1) &&
        !config_.GetGEngineChain().Empty() &&
        offsets.Engine_GameViewportClient != 0 &&
        offsets.GameViewportClient_World != 0) {
        uint64_t gEngine = 0;
        if (ResolveChain(moduleBase_, config_.GetGEngineChain(), gEngine)) {
            uint64_t viewport = memory_.ReadPointer(gEngine + offsets.Engine_GameViewportClient);
            if (MemoryUtils::IsValidPtr(viewport)) {
                uint64_t world = memory_.ReadPointer(viewport + offsets.GameViewportClient_World);
                if (MemoryUtils::IsValidPtr(world)) {
                    uWorld_ = world;
                    diagnostics_.usedGEngineFallback = true;
                    return true;
                }
            }
        }
    }

    return false;
}

void UE4Context::ResolveNamePool() {
    namePoolPtr_ = 0;
    diagnostics_.usedBridgeNamePool = false;
    if (!config_.GetNamePoolConfig().enabled) {
        return;
    }

    // Priority 0: In-process pushed GNames / FNamePool pointer (e.g. from GetGNames() getter)
    if (bridge_ != nullptr) {
        const uint64_t overridePool = bridge_->GetNamePoolOverride();
        if (MemoryUtils::IsValidPtr(overridePool)) {
            namePoolPtr_ = overridePool;
            diagnostics_.usedBridgeNamePool = true;
            return;
        }
    }

    if (!MemoryUtils::IsValidPtr(moduleBase_, 1) || config_.GetNamePoolChain().Empty()) {
        return;
    }
    uint64_t resolved = 0;
    if (ResolveChain(moduleBase_, config_.GetNamePoolChain(), resolved) &&
        MemoryUtils::IsValidPtr(resolved)) {
        namePoolPtr_ = resolved;
    }
}

uint64_t UE4Context::ResolvePlayerState(uint64_t actorPtr, uint64_t knownPlayerState) const {
    if (MemoryUtils::IsValidPtr(knownPlayerState)) {
        return MemoryUtils::StripTag(knownPlayerState);
    }
    const uint32_t psOff = config_.GetOffsets().Actor_PlayerState;
    if (psOff != 0 && MemoryUtils::IsValidPtr(actorPtr)) {
        uint64_t ps = memory_.ReadPointer(actorPtr + psOff);
        if (MemoryUtils::IsValidPtr(ps)) {
            return ps;
        }
    }
    return 0;
}

void UE4Context::ResolveLocalPlayer() {
    localController_ = 0;
    localPawn_ = 0;
    hasLocalPos_ = false;
    hasLocalTeamId_ = false;
    localTeamId_ = -1;
    diagnostics_.usedNetDriverLocalPlayer = false;

    const auto& offsets = config_.GetOffsets();

    // Priority 0: In-process pushed controller / pawn / UClass* overrides
    if (bridge_ != nullptr) {
        const uint64_t overrideCtrl = bridge_->GetLocalControllerOverride();
        if (MemoryUtils::IsValidPtr(overrideCtrl)) {
            localController_ = overrideCtrl;
        }
        const uint64_t overridePawn = bridge_->GetLocalPawnOverride();
        if (MemoryUtils::IsValidPtr(overridePawn)) {
            localPawn_ = overridePawn;
        }
        const uint64_t knownCls = bridge_->GetKnownCharacterClass();
        if (MemoryUtils::IsValidPtr(knownCls) && acceptedClasses_.find(knownCls) == acceptedClasses_.end()) {
            acceptedClasses_.insert(knownCls);
        }
    }

    // Path 1: Standard UE4 OwningGameInstance -> LocalPlayers[0] (2 derefs) -> PlayerController -> Pawn
    if (!MemoryUtils::IsValidPtr(localPawn_) &&
        MemoryUtils::IsValidPtr(uWorld_) &&
        offsets.World_OwningGameInstance != 0 &&
        offsets.GameInstance_LocalPlayers != 0 &&
        offsets.LocalPlayer_PlayerController != 0) {
        uint64_t gameInstance = memory_.ReadPointer(uWorld_ + offsets.World_OwningGameInstance);
        if (MemoryUtils::IsValidPtr(gameInstance)) {
            uint64_t localPlayersArray = gameInstance + offsets.GameInstance_LocalPlayers;
            uint64_t localPlayersData = memory_.ReadPointer(localPlayersArray + UE4Constants::TArray_Data);
            int32_t localPlayersCount = memory_.Read<int32_t>(localPlayersArray + UE4Constants::TArray_Count);

            if (MemoryUtils::IsValidPtr(localPlayersData) && localPlayersCount > 0 && localPlayersCount <= 16) {
                uint64_t firstLocalPlayer = memory_.ReadPointer(localPlayersData);
                if (MemoryUtils::IsValidPtr(firstLocalPlayer)) {
                    uint64_t pc = memory_.ReadPointer(firstLocalPlayer + offsets.LocalPlayer_PlayerController);
                    if (MemoryUtils::IsValidPtr(pc)) {
                        localController_ = pc;
                        if (offsets.PlayerController_Pawn != 0) {
                            uint64_t pawn = memory_.ReadPointer(pc + offsets.PlayerController_Pawn);
                            if (MemoryUtils::IsValidPtr(pawn)) {
                                localPawn_ = pawn;
                            }
                        }
                    }
                }
            }
        }
    }

    // Path 1B: Standard UE4 UWorld -> NetDriver -> ServerConnection -> PlayerController -> AcknowledgedPawn
    // Pure pointer read (works in BOTH external & internal modes even when LocalPlayers is encrypted).
    const uint32_t netPcOff = (offsets.ServerConnection_PlayerController != 0)
        ? offsets.ServerConnection_PlayerController
        : offsets.LocalPlayer_PlayerController;
    if ((!MemoryUtils::IsValidPtr(localController_) || !MemoryUtils::IsValidPtr(localPawn_)) &&
        MemoryUtils::IsValidPtr(uWorld_) &&
        offsets.World_NetDriver != 0 &&
        offsets.NetDriver_ServerConnection != 0 &&
        netPcOff != 0) {
        const uint64_t netDriver = memory_.ReadPointer(uWorld_ + offsets.World_NetDriver);
        if (MemoryUtils::IsValidPtr(netDriver)) {
            const uint64_t serverConn = memory_.ReadPointer(netDriver + offsets.NetDriver_ServerConnection);
            if (MemoryUtils::IsValidPtr(serverConn)) {
                const uint64_t pc = memory_.ReadPointer(serverConn + netPcOff);
                if (MemoryUtils::IsValidPtr(pc)) {
                    if (!MemoryUtils::IsValidPtr(localController_)) {
                        localController_ = pc;
                        diagnostics_.usedNetDriverLocalPlayer = true;
                    }
                    if (!MemoryUtils::IsValidPtr(localPawn_) && offsets.PlayerController_Pawn != 0) {
                        const uint64_t pawn = memory_.ReadPointer(pc + offsets.PlayerController_Pawn);
                        if (MemoryUtils::IsValidPtr(pawn)) {
                            localPawn_ = pawn;
                            diagnostics_.usedNetDriverLocalPlayer = true;
                        }
                    }
                }
            }
        }
    }

    // Path 2: Global LocalPawn chain from moduleBase
    if (!MemoryUtils::IsValidPtr(localPawn_) && !config_.GetLocalPawnChain().Empty()) {
        uint64_t pawn = 0;
        if (ResolveChain(moduleBase_, config_.GetLocalPawnChain(), pawn) &&
            MemoryUtils::IsValidPtr(pawn)) {
            localPawn_ = pawn;
        }
    }

    // Path 3: LocalPawn chain relative to UWorld
    if (!MemoryUtils::IsValidPtr(localPawn_) &&
        MemoryUtils::IsValidPtr(uWorld_) &&
        !config_.GetLocalPawnFromWorldChain().Empty()) {
        uint64_t pawn = 0;
        if (ResolveChain(uWorld_, config_.GetLocalPawnFromWorldChain(), pawn) &&
            MemoryUtils::IsValidPtr(pawn)) {
            localPawn_ = pawn;
        }
    }

    // Fallback for LocalController via LocalPawn -> Controller
    if (!MemoryUtils::IsValidPtr(localController_) &&
        MemoryUtils::IsValidPtr(localPawn_) &&
        offsets.Actor_Controller != 0) {
        uint64_t pc = memory_.ReadPointer(localPawn_ + offsets.Actor_Controller);
        if (MemoryUtils::IsValidPtr(pc)) {
            localController_ = pc;
        }
    }

    // Read local player position, team ID, and optional ClassPrivate seed
    if (MemoryUtils::IsValidPtr(localPawn_)) {
        hasLocalPos_ = ReadActorPosition(localPawn_, localPlayerPos_);

        const auto& filter = config_.GetEntityFilterConfig();
        if (filter.mode == EntityFilterConfig::Mode::ClassPrivate &&
            filter.classPrivateUseLocalPawn) {
            uint64_t localCls = memory_.ReadPointer(localPawn_ + filter.classPrivateOffset);
            if (MemoryUtils::IsValidPtr(localCls) && acceptedClasses_.find(localCls) == acceptedClasses_.end()) {
                acceptedClasses_.insert(localCls);
                usedHeuristicClassBootstrap_ = true;
            }
        }

        const auto& actorCfg = config_.GetActorConfig();
        uint64_t teamBase = 0;
        uint32_t teamOff = 0;
        if (actorCfg.psTeamIdOffset != 0) {
            teamBase = ResolvePlayerState(localPawn_, 0);
            teamOff = actorCfg.psTeamIdOffset;
        } else if (actorCfg.teamIdOffset != 0) {
            teamBase = actorCfg.attributesOnPlayerState
                ? ResolvePlayerState(localPawn_, 0)
                : localPawn_;
            teamOff = actorCfg.teamIdOffset;
        }

        if (MemoryUtils::IsValidPtr(teamBase) && teamOff != 0) {
            int32_t team = 0;
            if (memory_.ReadInto<int32_t>(teamBase + teamOff, team)) {
                localTeamId_ = team;
                hasLocalTeamId_ = true;
            }
        }
    }
}

bool UE4Context::ReadViewMatrix() {
    hasViewMatrix_ = false;
    diagnostics_.matrixAddress = 0;

    const auto& camCfg = config_.GetCameraConfig();
    if (camCfg.viewMatrixChain.Empty()) {
        return false;
    }

    uint64_t matAddr = 0;
    if (!ResolveChain(moduleBase_, camCfg.viewMatrixChain, matAddr)) {
        return false;
    }

    std::array<float, 16> rawMat{};
    if (!memory_.ReadBuffer(matAddr, rawMat.data(), sizeof(rawMat))) {
        return false;
    }

    bool anyNonZero = false;
    for (float v : rawMat) {
        if (!std::isfinite(v)) {
            return false;
        }
        if (std::fabs(v) > 1e-6f) {
            anyNonZero = true;
        }
    }
    if (!anyNonZero) {
        return false;
    }

    viewMatrix_ = rawMat;
    activeMatrixLayout_ = camCfg.matrixLayout;
    diagnostics_.matrixAddress = matAddr;
    hasViewMatrix_ = true;
    return true;
}

bool UE4Context::ReadCameraCacheFromController(
    uint64_t controllerPtr,
    FMinimalViewInfo& outPov,
    uint64_t* outCamMgr) const
{
    const auto& offsets = config_.GetOffsets();
    const auto& camCfg = config_.GetCameraConfig();

    if (!MemoryUtils::IsValidPtr(controllerPtr) ||
        offsets.PlayerController_PlayerCameraManager == 0 ||
        camCfg.cameraCacheOffset == 0) {
        return false;
    }

    uint64_t camMgr = memory_.ReadPointer(controllerPtr + offsets.PlayerController_PlayerCameraManager);
    if (!MemoryUtils::IsValidPtr(camMgr)) {
        return false;
    }
    if (outCamMgr) {
        *outCamMgr = camMgr;
    }

    const uint64_t cacheBase = camMgr + camCfg.cameraCacheOffset;
    const uint64_t povBase   = cacheBase + camCfg.povOffset;

    FMinimalViewInfo pov;
    if (!memory_.ReadInto<Vec3>(povBase + camCfg.locationOffset, pov.Location)) {
        return false;
    }
    if (!memory_.ReadInto<Vec3>(povBase + camCfg.rotationOffset, pov.Rotation)) {
        return false;
    }

    const uint64_t fovAddr = camCfg.fovRelativeToCache
        ? (cacheBase + camCfg.fovOffset)
        : (povBase + camCfg.fovOffset);
    if (!memory_.ReadInto<float>(fovAddr, pov.FOV)) {
        return false;
    }

    if (!std::isfinite(pov.Location.x) || !std::isfinite(pov.Location.y) || !std::isfinite(pov.Location.z) ||
        !std::isfinite(pov.Rotation.x) || !std::isfinite(pov.Rotation.y) || !std::isfinite(pov.Rotation.z) ||
        !std::isfinite(pov.FOV) || pov.FOV < 1.0f || pov.FOV > 179.0f) {
        return false;
    }

    outPov = pov;
    return true;
}

bool UE4Context::ReadCameraCache() {
    hasCameraPOV_ = false;
    diagnostics_.cameraManager = 0;
    diagnostics_.usedHeuristicCameraPC = false;

    uint64_t camMgr = 0;
    FMinimalViewInfo pov;

    if (MemoryUtils::IsValidPtr(localController_) &&
        ReadCameraCacheFromController(localController_, pov, &camMgr)) {
        cameraPOV_ = pov;
        diagnostics_.cameraManager = camMgr;
        hasCameraPOV_ = true;
        return true;
    }

    // Try cached heuristic PlayerController if LocalPlayers is encrypted
    if (config_.GetCameraConfig().heuristicPcScan &&
        MemoryUtils::IsValidPtr(cachedHeuristicPC_) &&
        ReadCameraCacheFromController(cachedHeuristicPC_, pov, &camMgr)) {
        localController_ = cachedHeuristicPC_;
        diagnostics_.localController = cachedHeuristicPC_;
        diagnostics_.cameraManager = camMgr;
        diagnostics_.usedHeuristicCameraPC = true;
        cameraPOV_ = pov;
        hasCameraPOV_ = true;
        return true;
    }

    return false;
}

bool UE4Context::ScanActorsForCameraController(const uint64_t* actors, size_t count) {
    if (!config_.GetCameraConfig().heuristicPcScan || hasViewMatrix_ || hasCameraPOV_ || !actors || count == 0) {
        return hasCameraPOV_;
    }

    const auto& offsets = config_.GetOffsets();
    if (offsets.PlayerController_PlayerCameraManager == 0 || config_.GetCameraConfig().cameraCacheOffset == 0) {
        return false;
    }

    const uint32_t clsOff = config_.GetOffsets().Actor_ClassPrivate;

    // Pass 1: O(1) ClassPrivate check if PlayerController UClass* was already discovered
    if (MemoryUtils::IsValidPtr(cachedPcClass_)) {
        for (size_t i = 0; i < count; ++i) {
            uint64_t actor = MemoryUtils::StripTag(actors[i]);
            if (!MemoryUtils::IsValidPtr(actor)) continue;
            if (memory_.ReadPointer(actor + clsOff) != cachedPcClass_) continue;

            uint64_t camMgr = 0;
            FMinimalViewInfo pov;
            if (ReadCameraCacheFromController(actor, pov, &camMgr)) {
                cachedHeuristicPC_ = actor;
                localController_ = actor;
                diagnostics_.localController = actor;
                diagnostics_.cameraManager = camMgr;
                diagnostics_.usedHeuristicCameraPC = true;
                diagnostics_.hasCameraPOV = true;
                cameraPOV_ = pov;
                hasCameraPOV_ = true;
                return true;
            }
        }
    }

    // Pass 2: One-time heuristic scan for actor with valid CameraManager + FOV in [1..179]
    for (size_t i = 0; i < count; ++i) {
        uint64_t actor = MemoryUtils::StripTag(actors[i]);
        if (!MemoryUtils::IsValidPtr(actor)) continue;

        uint64_t camMgr = 0;
        FMinimalViewInfo pov;
        if (ReadCameraCacheFromController(actor, pov, &camMgr)) {
            cachedHeuristicPC_ = actor;
            uint64_t pcCls = memory_.ReadPointer(actor + clsOff);
            if (MemoryUtils::IsValidPtr(pcCls)) {
                cachedPcClass_ = pcCls;
            }
            localController_ = actor;
            diagnostics_.localController = actor;
            diagnostics_.cameraManager = camMgr;
            diagnostics_.usedHeuristicCameraPC = true;
            diagnostics_.hasCameraPOV = true;
            cameraPOV_ = pov;
            hasCameraPOV_ = true;
            return true;
        }
    }

    return false;
}

void UE4Context::UpdateCamera() {
    hasViewMatrix_ = false;
    hasCameraPOV_ = false;
    usingViewMatrix_ = false;
    diagnostics_.usedBridgeCamera = false;
    activeMatrixLayout_ = config_.GetCameraConfig().matrixLayout;

    // Priority 0: In-process pushed camera (InternalEngineBridge) — zero offset reads!
    if (bridge_ != nullptr) {
        const CameraMode mode = config_.GetCameraConfig().mode;
        std::array<float, 16> bridgeMat{};
        MatrixLayout bridgeLayout = activeMatrixLayout_;
        FMinimalViewInfo bridgePov;

        if (mode != CameraMode::CameraCache && bridge_->TryReadViewMatrix(bridgeMat, bridgeLayout)) {
            viewMatrix_ = bridgeMat;
            activeMatrixLayout_ = bridgeLayout;
            hasViewMatrix_ = true;
            usingViewMatrix_ = true;
            diagnostics_.usedBridgeCamera = true;
            return;
        }
        if (mode != CameraMode::ViewMatrix && bridge_->TryReadCameraPOV(bridgePov) &&
            std::isfinite(bridgePov.FOV) && bridgePov.FOV >= 1.0f && bridgePov.FOV <= 179.0f) {
            cameraPOV_ = bridgePov;
            hasCameraPOV_ = true;
            usingViewMatrix_ = false;
            diagnostics_.usedBridgeCamera = true;
            return;
        }
    }

    const CameraMode mode = config_.GetCameraConfig().mode;

    if (mode == CameraMode::ViewMatrix) {
        if (ReadViewMatrix()) {
            usingViewMatrix_ = true;
        }
    } else if (mode == CameraMode::CameraCache) {
        ReadCameraCache();
        usingViewMatrix_ = false;
    } else {
        if (ReadViewMatrix()) {
            usingViewMatrix_ = true;
        } else if (ReadCameraCache()) {
            usingViewMatrix_ = false;
        }
    }
}

bool UE4Context::BeginFrame() {
    diagnostics_ = ChainDiagnostics{};
    diagnostics_.pid = memory_.GetPid();
    diagnostics_.bonesEnabled = config_.GetBoneConfig().enabled;

    moduleBase_ = MemoryUtils::StripTag(memory_.GetModuleBase());
    diagnostics_.moduleBase = moduleBase_;

    uWorld_ = 0;
    persistentLevel_ = 0;
    gameState_ = 0;
    namePoolPtr_ = 0;
    localController_ = 0;
    localPawn_ = 0;
    hasLocalPos_ = false;
    hasLocalTeamId_ = false;
    hasViewMatrix_ = false;
    hasCameraPOV_ = false;
    usingViewMatrix_ = false;

    const bool hasBase = MemoryUtils::IsValidPtr(moduleBase_, 1);
    if (!initialized_ || (!hasBase && bridge_ == nullptr)) {
        return false;
    }

    if (hasBase || bridge_ != nullptr) {
        ResolveNamePool();
    }
    diagnostics_.namePoolPtr = namePoolPtr_;
    diagnostics_.nameSystemEnabled = config_.GetNamePoolConfig().enabled;
    diagnostics_.nameCacheEntries = nameResolver_ ? nameResolver_->GetCacheSize() : 0;

    // Optional direct EntityList chain for non-UE4 engines (work12 / work14)
    if (hasBase && !config_.GetEntityListChain().Empty()) {
        uint64_t container = 0;
        if (ResolveChain(moduleBase_, config_.GetEntityListChain(), container) &&
            MemoryUtils::IsValidPtr(container)) {
            persistentLevel_ = container;
            diagnostics_.usedEntityListChain = true;
        }
    }

    if (ResolveWorld()) {
        diagnostics_.uWorld = uWorld_;

        const auto& offsets = config_.GetOffsets();
        if (!MemoryUtils::IsValidPtr(persistentLevel_) && offsets.World_PersistentLevel != 0) {
            uint64_t lvl = memory_.ReadPointer(uWorld_ + offsets.World_PersistentLevel);
            if (MemoryUtils::IsValidPtr(lvl)) {
                persistentLevel_ = lvl;
            }
        }
        if (offsets.World_GameState != 0) {
            uint64_t gs = memory_.ReadPointer(uWorld_ + offsets.World_GameState);
            if (MemoryUtils::IsValidPtr(gs)) {
                gameState_ = gs;
            }
        }
    }

    diagnostics_.persistentLevel = persistentLevel_;
    diagnostics_.gameState = gameState_;

    ResolveLocalPlayer();
    diagnostics_.localController = localController_;
    diagnostics_.localPawn = localPawn_;
    diagnostics_.hasLocalPos = hasLocalPos_;
    diagnostics_.localPos = localPlayerPos_;

    diagnostics_.acceptedClassCount = acceptedClasses_.size();
    diagnostics_.rejectedClassCount = rejectedClasses_.size();
    diagnostics_.usedHeuristicClassBootstrap = usedHeuristicClassBootstrap_;

    UpdateCamera();
    diagnostics_.hasViewMatrix = hasViewMatrix_;
    diagnostics_.hasCameraPOV = hasCameraPOV_;
    diagnostics_.usingMatrixCamera = usingViewMatrix_;

    return HasValidWorld();
}

bool UE4Context::ReadActorPosition(uint64_t actorPtr, Vec3& outPos) const {
    if (!MemoryUtils::IsValidPtr(actorPtr)) {
        return false;
    }

    const auto& offsets = config_.GetOffsets();
    const auto& actorCfg = config_.GetActorConfig();

    if (offsets.Actor_RootComponent == 0) {
        return false;
    }

    uint64_t rootComp = memory_.ReadPointer(actorPtr + offsets.Actor_RootComponent);
    if (!MemoryUtils::IsValidPtr(rootComp)) {
        return false;
    }

    uint64_t posAddr = 0;
    if (actorCfg.positionSource == PositionSource::ComponentToWorld &&
        offsets.SceneComponent_ComponentToWorld != 0) {
        posAddr = rootComp + offsets.SceneComponent_ComponentToWorld + UE4Constants::FTransform_Translation;
    } else if (offsets.SceneComponent_RelativeLocation != 0) {
        posAddr = rootComp + offsets.SceneComponent_RelativeLocation;
    } else {
        return false;
    }

    Vec3 pos;
    if (!memory_.ReadInto<Vec3>(posAddr, pos)) {
        return false;
    }
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) {
        return false;
    }

    outPos = pos;
    return true;
}

bool UE4Context::ReadObjectNameInto(uint64_t objectPtr, char* outBuf, size_t outBufSize) const {
    if (!outBuf || outBufSize == 0) return false;
    outBuf[0] = '\0';

    if (!nameResolver_ || !MemoryUtils::IsValidPtr(namePoolPtr_) || !MemoryUtils::IsValidPtr(objectPtr)) {
        return false;
    }

    uint32_t nameIndex = 0;
    if (!memory_.ReadInto<uint32_t>(objectPtr + config_.GetOffsets().Actor_FNameIndex, nameIndex)) {
        return false;
    }

    return nameResolver_->ResolveInto(namePoolPtr_, nameIndex, outBuf, outBufSize);
}

std::string UE4Context::ReadObjectName(uint64_t objectPtr) const {
    char buf[128] = {};
    if (!ReadObjectNameInto(objectPtr, buf, sizeof(buf))) {
        return "";
    }
    return std::string(buf);
}

uint64_t UE4Context::ReadClassPrivate(uint64_t objectPtr) const {
    if (!MemoryUtils::IsValidPtr(objectPtr)) {
        return 0;
    }
    return memory_.ReadPointer(objectPtr + config_.GetOffsets().Actor_ClassPrivate);
}

void UE4Context::ReadActorAttributes(uint64_t actorPtr, uint64_t playerStatePtr, PooledEntity& entity) const {
    const auto& actorCfg = config_.GetActorConfig();

    const bool needPS =
        actorCfg.attributesOnPlayerState ||
        actorCfg.psHealthOffset != 0 ||
        actorCfg.psMaxHealthOffset != 0 ||
        actorCfg.psTeamIdOffset != 0 ||
        actorCfg.psPlayerNameOffset != 0;

    const uint64_t resolvedPS = needPS ? ResolvePlayerState(actorPtr, playerStatePtr) : 0;
    if (resolvedPS != 0 && entity.playerState == 0) {
        entity.playerState = resolvedPS;
    }

    auto resolveFieldTarget = [&](uint32_t psOverrideOff, uint32_t actorOff, uint64_t& outBase, uint32_t& outOff) {
        if (psOverrideOff != 0) {
            outBase = resolvedPS;
            outOff = psOverrideOff;
        } else if (actorOff != 0) {
            outBase = actorCfg.attributesOnPlayerState ? resolvedPS : actorPtr;
            outOff = actorOff;
        } else {
            outBase = 0;
            outOff = 0;
        }
    };

    uint64_t hpBase = 0;
    uint32_t hpOff = 0;
    resolveFieldTarget(actorCfg.psHealthOffset, actorCfg.healthOffset, hpBase, hpOff);
    if (MemoryUtils::IsValidPtr(hpBase) && hpOff != 0) {
        float hp = 0.0f;
        if (memory_.ReadInto<float>(hpBase + hpOff, hp) && std::isfinite(hp)) {
            entity.health = hp;
            entity.hasHealth = true;
        }
    }

    uint64_t maxHpBase = 0;
    uint32_t maxHpOff = 0;
    resolveFieldTarget(actorCfg.psMaxHealthOffset, actorCfg.maxHealthOffset, maxHpBase, maxHpOff);
    if (MemoryUtils::IsValidPtr(maxHpBase) && maxHpOff != 0) {
        float maxHp = 0.0f;
        if (memory_.ReadInto<float>(maxHpBase + maxHpOff, maxHp) &&
            std::isfinite(maxHp) && maxHp > 0.0f) {
            entity.maxHealth = maxHp;
        }
    }

    uint64_t teamBase = 0;
    uint32_t teamOff = 0;
    resolveFieldTarget(actorCfg.psTeamIdOffset, actorCfg.teamIdOffset, teamBase, teamOff);
    if (MemoryUtils::IsValidPtr(teamBase) && teamOff != 0) {
        int32_t team = 0;
        if (memory_.ReadInto<int32_t>(teamBase + teamOff, team)) {
            entity.teamId = team;
            entity.hasTeamId = true;
        }
    }

    if (actorCfg.isBotOffset != 0 && MemoryUtils::IsValidPtr(actorPtr)) {
        uint8_t botFlag = 0;
        if (memory_.ReadInto<uint8_t>(actorPtr + actorCfg.isBotOffset, botFlag)) {
            entity.isBot = (botFlag != 0);
        }
    }

    uint64_t nameBase = 0;
    uint32_t nameOff = 0;
    resolveFieldTarget(actorCfg.psPlayerNameOffset, actorCfg.playerNameOffset, nameBase, nameOff);
    if (MemoryUtils::IsValidPtr(nameBase) && nameOff != 0) {
        uint64_t namePtr = memory_.ReadPointer(nameBase + nameOff);
        if (MemoryUtils::IsValidPtr(namePtr, 1)) {
            bool ok = actorCfg.playerNameIsUtf16
                ? memory_.ReadUtf16StringInto(namePtr, entity.playerName, sizeof(entity.playerName), 32)
                : memory_.ReadAnsiStringInto(namePtr, entity.playerName, sizeof(entity.playerName), 63);
            entity.hasPlayerName = ok && (entity.playerName[0] != '\0');
        }
    }
}

void UE4Context::ReadActorAttributes(uint64_t actorPtr, uint64_t playerStatePtr, Entity& entity) const {
    PooledEntity tmp;
    tmp.address = entity.address;
    tmp.playerState = entity.playerState;
    ReadActorAttributes(actorPtr, playerStatePtr, tmp);
    entity.playerState = tmp.playerState;
    entity.health = tmp.health;
    entity.maxHealth = tmp.maxHealth;
    entity.teamId = tmp.teamId;
    entity.isBot = tmp.isBot;
    entity.hasHealth = tmp.hasHealth;
    entity.hasTeamId = tmp.hasTeamId;
    entity.hasPlayerName = tmp.hasPlayerName;
    if (tmp.hasPlayerName) {
        entity.playerName = tmp.playerName;
    }
}

bool UE4Context::ReadActorSkeleton(uint64_t actorPtr, PooledEntity& entity) {
    entity.hasSkeleton = false;
    const auto& boneCfg = config_.GetBoneConfig();
    if (!boneCfg.enabled || !MemoryUtils::IsValidPtr(actorPtr)) {
        return false;
    }

    if (hasLocalPos_ && boneCfg.maxDistanceMeters > 0.0f &&
        entity.distanceMeters > boneCfg.maxDistanceMeters) {
        return false;
    }

    const uint64_t meshPtr = memory_.ReadPointer(actorPtr + boneCfg.meshOffset);
    if (!MemoryUtils::IsValidPtr(meshPtr)) {
        return false;
    }

    FTransform compToWorld{};
    if (!memory_.ReadInto<FTransform>(meshPtr + boneCfg.componentToWorldOffset, compToWorld)) {
        return false;
    }

    const uint64_t boneArrayPtr = memory_.ReadPointer(meshPtr + boneCfg.boneArrayOffset);
    if (!MemoryUtils::IsValidPtr(boneArrayPtr)) {
        return false;
    }

    int32_t boneCount = 0;
    if (boneCfg.boneCountOffset != 0) {
        memory_.ReadInto<int32_t>(meshPtr + boneCfg.boneCountOffset, boneCount);
    }

    // Select bone index profile matching exact BoneCount, or fall back to defaultProfile
    const BoneProfile* profile = nullptr;
    if (boneCount > 0) {
        auto it = boneCfg.profilesByCount.find(boneCount);
        if (it != boneCfg.profilesByCount.end() && it->second.valid) {
            profile = &it->second;
        }
    }
    if (!profile && boneCfg.defaultProfile.valid) {
        profile = &boneCfg.defaultProfile;
    }
    if (!profile) {
        return false;
    }

    // Determine contiguous span to batch-read in ONE syscall (zero per-frame alloc)
    int32_t maxIndex = -1;
    for (int32_t idx : profile->indices) {
        if (idx > maxIndex) {
            maxIndex = idx;
        }
    }
    if (maxIndex < 0 || maxIndex >= 256) {
        return false;
    }
    if (boneCount > 0 && maxIndex >= boneCount) {
        return false;
    }

    const size_t totalBytes = static_cast<size_t>(maxIndex + 1) * static_cast<size_t>(boneCfg.boneStride);
    boneRawBuf_.resize(totalBytes);

    const uint64_t boneBaseAddr = boneArrayPtr + boneCfg.boneArrayBias;
    if (!memory_.ReadBuffer(boneBaseAddr, boneRawBuf_.data(), totalBytes)) {
        return false;
    }

    const UE4Math::Matrix4x4 compMat = UE4Math::TransformToMatrixWithScale(compToWorld);
    bool anyValid = false;

    for (size_t s = 0; s < BONE_SLOT_COUNT; ++s) {
        const int32_t idx = profile->indices[s];
        if (idx < 0) {
            entity.boneValid[s] = 0;
            continue;
        }

        FTransform localBone{};
        const size_t byteOff = static_cast<size_t>(idx) * static_cast<size_t>(boneCfg.boneStride);
        std::memcpy(&localBone, boneRawBuf_.data() + byteOff, sizeof(FTransform));

        Vec3 worldBone = UE4Math::BoneTransformToWorld(localBone, compMat);
        if (std::isfinite(worldBone.x) && std::isfinite(worldBone.y) && std::isfinite(worldBone.z)) {
            entity.boneWorld[s] = worldBone;
            entity.boneValid[s] = 1;
            anyValid = true;
        } else {
            entity.boneValid[s] = 0;
        }
    }

    entity.hasSkeleton = anyValid;
    return anyValid;
}

bool UE4Context::CheckSentinel(uint64_t actorPtr) const {
    const auto& filter = config_.GetEntityFilterConfig();

    uint64_t sentinelAddr = 0;
    if (!filter.sentinelChain.Empty()) {
        if (!ResolveChain(actorPtr, filter.sentinelChain, sentinelAddr)) {
            return false;
        }
    } else if (filter.sentinelOffset != 0) {
        sentinelAddr = actorPtr + filter.sentinelOffset;
    } else {
        return true;
    }

    if (filter.sentinelType == EntityFilterConfig::SentinelType::Float) {
        float val = 0.0f;
        if (!memory_.ReadInto<float>(sentinelAddr, val) || !std::isfinite(val)) {
            return false;
        }
        return std::fabs(val - filter.sentinelFloatValue) <= filter.sentinelEpsilon;
    } else {
        int32_t val = 0;
        if (!memory_.ReadInto<int32_t>(sentinelAddr, val)) {
            return false;
        }
        return val == filter.sentinelIntValue;
    }
}

bool UE4Context::HeuristicLooksLikePawn(uint64_t actorPtr) const {
    const auto& filter = config_.GetEntityFilterConfig();
    const auto& actorCfg = config_.GetActorConfig();
    const auto& boneCfg = config_.GetBoneConfig();
    const auto& offsets = config_.GetOffsets();

    // Criterion 1: Must have a valid RootComponent and non-zero finite world position
    Vec3 pos;
    if (!ReadActorPosition(actorPtr, pos)) {
        return false;
    }
    if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) {
        return false;
    }

    // Criterion 2 (strongest): Plausible HP within [min_hp, max_hp] (ESP4All_SKILL.md:188)
    uint64_t hpBase = 0;
    uint32_t hpOff = 0;
    if (actorCfg.psHealthOffset != 0) {
        hpBase = ResolvePlayerState(actorPtr, 0);
        hpOff = actorCfg.psHealthOffset;
    } else if (actorCfg.healthOffset != 0) {
        hpBase = actorCfg.attributesOnPlayerState ? ResolvePlayerState(actorPtr, 0) : actorPtr;
        hpOff = actorCfg.healthOffset;
    }
    if (hpOff != 0) {
        if (!MemoryUtils::IsValidPtr(hpBase)) {
            return false;
        }
        float hp = 0.0f;
        if (!memory_.ReadInto<float>(hpBase + hpOff, hp) || !std::isfinite(hp)) {
            return false;
        }
        return hp >= filter.heuristicMinHp && hp <= filter.heuristicMaxHp;
    }

    // Criterion 3: Valid SkeletalMesh with plausible humanoid bone count [15..200]
    if (boneCfg.meshOffset != 0 && boneCfg.boneCountOffset != 0) {
        uint64_t mesh = memory_.ReadPointer(actorPtr + boneCfg.meshOffset);
        if (MemoryUtils::IsValidPtr(mesh)) {
            int32_t bc = memory_.Read<int32_t>(mesh + boneCfg.boneCountOffset);
            if (bc >= 15 && bc <= 200) {
                return true;
            }
        }
        return false;
    }

    // Criterion 4: Valid PlayerState pointer attached to Pawn
    if (offsets.Actor_PlayerState != 0) {
        uint64_t ps = memory_.ReadPointer(actorPtr + offsets.Actor_PlayerState);
        return MemoryUtils::IsValidPtr(ps);
    }

    return false;
}

bool UE4Context::MatchesNameRules(
    const char* name,
    const std::vector<std::string>& exactList,
    const std::vector<std::string>& containsList,
    const std::vector<std::string>& excludeList)
{
    if (!name || name[0] == '\0') {
        return false;
    }
    for (const auto& ex : excludeList) {
        if (!ex.empty() && std::strcmp(name, ex.c_str()) == 0) {
            return false;
        }
    }
    if (exactList.empty() && containsList.empty()) {
        return true;
    }
    for (const auto& exact : exactList) {
        if (!exact.empty() && std::strcmp(name, exact.c_str()) == 0) {
            return true;
        }
    }
    for (const auto& sub : containsList) {
        if (!sub.empty() && std::strstr(name, sub.c_str()) != nullptr) {
            return true;
        }
    }
    return false;
}

bool UE4Context::QuickPreFilter(uint64_t actorPtr, uint64_t* outClassPrivate) const {
    const auto& filter = config_.GetEntityFilterConfig();

    if (filter.mode == EntityFilterConfig::Mode::Sentinel) {
        return CheckSentinel(actorPtr);
    }

    if (filter.mode == EntityFilterConfig::Mode::ClassPrivate) {
        uint64_t cls = memory_.ReadPointer(actorPtr + filter.classPrivateOffset);
        if (outClassPrivate) {
            *outClassPrivate = cls;
        }
        if (!MemoryUtils::IsValidPtr(cls)) {
            return false;
        }
        if (rejectedClasses_.find(cls) != rejectedClasses_.end()) {
            return false;
        }
    }

    return true;
}

bool UE4Context::MatchesEntityFilter(
    uint64_t actorPtr,
    const char* preReadClassName,
    uint64_t preReadClassPrivate)
{
    const auto& filter = config_.GetEntityFilterConfig();

    switch (filter.mode) {
        case EntityFilterConfig::Mode::None:
            return true;

        case EntityFilterConfig::Mode::Sentinel:
            return CheckSentinel(actorPtr);

        case EntityFilterConfig::Mode::ClassName: {
            char nameBuf[128] = {};
            const char* namePtr = preReadClassName;
            if (!namePtr) {
                ReadObjectNameInto(actorPtr, nameBuf, sizeof(nameBuf));
                namePtr = nameBuf;
            }
            return MatchesNameRules(
                namePtr,
                filter.classNameExact,
                filter.classNameContains,
                filter.classNameExclude);
        }

        case EntityFilterConfig::Mode::ClassPrivate: {
            uint64_t cls = MemoryUtils::IsValidPtr(preReadClassPrivate)
                ? preReadClassPrivate
                : memory_.ReadPointer(actorPtr + filter.classPrivateOffset);
            if (!MemoryUtils::IsValidPtr(cls)) {
                return false;
            }
            if (acceptedClasses_.find(cls) != acceptedClasses_.end()) {
                return true;
            }
            if (rejectedClasses_.find(cls) != rejectedClasses_.end()) {
                return false;
            }

            // Path A: Name-based bootstrap when name system is enabled & target_names are configured
            if (!filter.classPrivateTargetNames.empty()) {
                char clsNameBuf[128] = {};
                ReadObjectNameInto(cls, clsNameBuf, sizeof(clsNameBuf));
                const char* resolvedName = clsNameBuf;
                if (resolvedName[0] == '\0' && preReadClassName && preReadClassName[0] != '\0') {
                    resolvedName = preReadClassName;
                }
                if (resolvedName[0] == '\0') {
                    ReadObjectNameInto(actorPtr, clsNameBuf, sizeof(clsNameBuf));
                    resolvedName = clsNameBuf;
                }
                if (resolvedName[0] != '\0') {
                    if (MatchesNameRules(resolvedName, {}, filter.classPrivateTargetNames, {})) {
                        acceptedClasses_.insert(cls);
                        diagnostics_.acceptedClassCount = acceptedClasses_.size();
                        return true;
                    }
                    if (rejectedClasses_.size() < MAX_REJECTED_CLASSES) {
                        rejectedClasses_.insert(cls);
                        diagnostics_.rejectedClassCount = rejectedClasses_.size();
                    }
                    return false;
                }
            }

            // Path B: Heuristic bootstrap fallback without GNames (ESP4All_SKILL.md:183-195)
            if (filter.classPrivateHeuristicBootstrap) {
                if (HeuristicLooksLikePawn(actorPtr)) {
                    acceptedClasses_.insert(cls);
                    usedHeuristicClassBootstrap_ = true;
                    diagnostics_.acceptedClassCount = acceptedClasses_.size();
                    diagnostics_.usedHeuristicClassBootstrap = true;
                    return true;
                }
                if (rejectedClasses_.size() < MAX_REJECTED_CLASSES) {
                    rejectedClasses_.insert(cls);
                    diagnostics_.rejectedClassCount = rejectedClasses_.size();
                }
            }
            return false;
        }
    }

    return true;
}

bool UE4Context::MatchesEntityFilter(
    uint64_t actorPtr,
    const std::string* preReadClassName,
    uint64_t preReadClassPrivate)
{
    return MatchesEntityFilter(
        actorPtr,
        preReadClassName ? preReadClassName->c_str() : nullptr,
        preReadClassPrivate);
}

bool UE4Context::ProjectWorldToScreen(const Vec3& worldPos, int screenW, int screenH, Vec3& outScreen) const {
    const auto& camCfg = config_.GetCameraConfig();
    if (usingViewMatrix_ && hasViewMatrix_) {
        return UE4Math::WorldToScreenMatrix(
            worldPos, viewMatrix_, screenW, screenH, outScreen, activeMatrixLayout_);
    }
    if (hasCameraPOV_) {
        return UE4Math::WorldToScreenTrig(worldPos, cameraPOV_, screenW, screenH, outScreen, camCfg.upAxis);
    }
    return false;
}

bool UE4Context::ProjectActorBox(const Vec3& worldPos, int screenW, int screenH, UE4Math::ScreenBox& outBox) const {
    const auto& camCfg = config_.GetCameraConfig();
    const auto& actorCfg = config_.GetActorConfig();

    if (usingViewMatrix_ && hasViewMatrix_) {
        return UE4Math::ComputeScreenBoxMatrix(
            worldPos, actorCfg.boxHeight, viewMatrix_, screenW, screenH, outBox,
            activeMatrixLayout_, camCfg.upAxis, actorCfg.boxBaseOffset);
    }
    if (hasCameraPOV_) {
        return UE4Math::ComputeScreenBoxTrig(
            worldPos, actorCfg.boxHeight, cameraPOV_, screenW, screenH, outBox,
            camCfg.upAxis, actorCfg.boxBaseOffset);
    }
    outBox = UE4Math::ScreenBox{};
    return false;
}
