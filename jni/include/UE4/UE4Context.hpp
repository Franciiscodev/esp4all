#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>
#include "Config/ConfigManager.hpp"
#include "ESP/IEntityProvider.hpp"
#include "IMemoryReader.hpp"
#include "UE4/InternalBridge.hpp"
#include "UE4/UE4Math.hpp"
#include "UE4/UE4NameResolver.hpp"
#include "UE4/UE4Structures.hpp"

/**
 * Unified UE4 / Generic 3D Engine Translation Layer (UE4Context).
 *
 * Single source of truth for:
 *  - Dynamic per-frame resolution of UWorld (GWorld chain + GEngine fallback) or direct EntityList chain
 *  - FNamePool / GNames resolution with zero-alloc cached lookup
 *  - LocalPlayer / PlayerController / LocalPawn resolution (once per frame)
 *  - Dual camera support: Direct ViewMatrix 4x4 (Col/Row-Major) & CameraCache POV (Z-Up/Y-Up Trig)
 *    + optional heuristic PlayerController camera scan when LocalPlayers is encrypted
 *  - Configurable entity filtering (None, Sentinel direct/chain, ClassName, ClassPrivate + heuristic bootstrap)
 *  - Config-driven actor attributes (AActor, APlayerState, hybrid) & optional FTransform Bone ESP
 */
class UE4Context {
public:
    explicit UE4Context(IMemoryReader& memory);

    bool Initialize(const ConfigManager& config);
    bool BeginFrame();

    // Optional in-process bridge for pushed Camera/World/LocalPawn/UClass overrides
    void SetInternalBridge(InternalEngineBridge* bridge) noexcept { bridge_ = bridge; }
    InternalEngineBridge* GetInternalBridge() const noexcept { return bridge_; }

    bool ResolveChain(uint64_t startAddr, const PointerChain& chain, uint64_t& outAddr) const;
    bool ReadActorPosition(uint64_t actorPtr, Vec3& outPos) const;

    // Zero-allocation name resolution into caller buffer (on cache hit)
    bool ReadObjectNameInto(uint64_t objectPtr, char* outBuf, size_t outBufSize) const;
    std::string ReadObjectName(uint64_t objectPtr) const;
    uint64_t ReadClassPrivate(uint64_t objectPtr) const;

    // Config-driven attributes (AActor, APlayerState, or hybrid)
    void ReadActorAttributes(uint64_t actorPtr, uint64_t playerStatePtr, PooledEntity& entity) const;
    void ReadActorAttributes(uint64_t actorPtr, uint64_t playerStatePtr, Entity& entity) const;

    // Optional FTransform-based 15-bone skeleton reader (single batch ReadBuffer, zero alloc)
    bool ReadActorSkeleton(uint64_t actorPtr, PooledEntity& entity);

    // Heuristic PlayerController scan for CameraCache when LocalPlayers is encrypted
    bool ScanActorsForCameraController(const uint64_t* actors, size_t count);

    // Filtering (with heuristic ClassPrivate bootstrap)
    bool QuickPreFilter(uint64_t actorPtr, uint64_t* outClassPrivate = nullptr) const;
    bool MatchesEntityFilter(uint64_t actorPtr, const char* preReadClassName, uint64_t preReadClassPrivate = 0);
    bool MatchesEntityFilter(uint64_t actorPtr, const std::string* preReadClassName, uint64_t preReadClassPrivate = 0);

    // W2S Projection (Matrix Col/Row-Major or Z-Up/Y-Up Trig)
    bool ProjectWorldToScreen(const Vec3& worldPos, int screenW, int screenH, Vec3& outScreen) const;
    bool ProjectActorBox(const Vec3& worldPos, int screenW, int screenH, UE4Math::ScreenBox& outBox) const;

    void ClearCaches();

    // Per-frame accessors
    bool IsInitialized() const noexcept { return initialized_; }
    bool HasValidWorld() const noexcept { return MemoryUtils::IsValidPtr(uWorld_) || MemoryUtils::IsValidPtr(persistentLevel_); }
    bool HasValidCamera() const noexcept { return hasViewMatrix_ || hasCameraPOV_; }
    bool IsUsingViewMatrix() const noexcept { return usingViewMatrix_; }
    bool IsNameSystemEnabled() const noexcept {
        return config_.GetNamePoolConfig().enabled &&
               nameResolver_ != nullptr &&
               MemoryUtils::IsValidPtr(namePoolPtr_);
    }

    uint64_t GetModuleBase() const noexcept { return moduleBase_; }
    uint64_t GetUWorld() const noexcept { return uWorld_; }
    uint64_t GetPersistentLevel() const noexcept { return persistentLevel_; }
    uint64_t GetGameState() const noexcept { return gameState_; }
    uint64_t GetNamePoolPtr() const noexcept { return namePoolPtr_; }
    uint64_t GetLocalController() const noexcept { return localController_; }
    uint64_t GetLocalPawn() const noexcept { return localPawn_; }
    bool HasLocalPlayerPosition() const noexcept { return hasLocalPos_; }
    const Vec3& GetLocalPlayerPosition() const noexcept { return localPlayerPos_; }
    bool HasLocalTeamId() const noexcept { return hasLocalTeamId_; }
    int32_t GetLocalTeamId() const noexcept { return localTeamId_; }

    const std::array<float, 16>& GetViewMatrix() const noexcept { return viewMatrix_; }
    const FMinimalViewInfo& GetCameraPOV() const noexcept { return cameraPOV_; }

    const ConfigManager& GetConfig() const noexcept { return config_; }
    IMemoryReader& GetMemory() const noexcept { return memory_; }
    const ChainDiagnostics& GetDiagnostics() const noexcept { return diagnostics_; }
    ChainDiagnostics& GetMutableDiagnostics() noexcept { return diagnostics_; }

private:
    IMemoryReader& memory_;
    ConfigManager config_;
    std::unique_ptr<INameResolver> nameResolver_;
    bool initialized_ = false;

    uint64_t moduleBase_ = 0;
    uint64_t uWorld_ = 0;
    uint64_t persistentLevel_ = 0;
    uint64_t gameState_ = 0;
    uint64_t namePoolPtr_ = 0;
    uint64_t localController_ = 0;
    uint64_t localPawn_ = 0;
    Vec3 localPlayerPos_;
    bool hasLocalPos_ = false;
    int32_t localTeamId_ = -1;
    bool hasLocalTeamId_ = false;

    std::array<float, 16> viewMatrix_{};
    MatrixLayout activeMatrixLayout_ = MatrixLayout::ColumnMajor;
    FMinimalViewInfo cameraPOV_;
    bool hasViewMatrix_ = false;
    bool hasCameraPOV_ = false;
    bool usingViewMatrix_ = false;

    InternalEngineBridge* bridge_ = nullptr;
    ChainDiagnostics diagnostics_;

    // Lazy ClassPrivate O(1) caches + heuristic PC cache
    std::unordered_set<uint64_t> acceptedClasses_;
    std::unordered_set<uint64_t> rejectedClasses_;
    uint64_t cachedHeuristicPC_ = 0;
    uint64_t cachedPcClass_ = 0;
    bool usedHeuristicClassBootstrap_ = false;
    static constexpr size_t MAX_REJECTED_CLASSES = 4096;

    // Reusable raw buffer for batch bone FTransform reads (zero per-frame alloc)
    std::vector<uint8_t> boneRawBuf_;

    bool ResolveWorld();
    void ResolveNamePool();
    void ResolveLocalPlayer();
    void UpdateCamera();
    bool ReadViewMatrix();
    bool ReadCameraCacheFromController(uint64_t controllerPtr, FMinimalViewInfo& outPov, uint64_t* outCamMgr = nullptr) const;
    bool ReadCameraCache();
    bool CheckSentinel(uint64_t actorPtr) const;
    bool HeuristicLooksLikePawn(uint64_t actorPtr) const;
    uint64_t ResolvePlayerState(uint64_t actorPtr, uint64_t knownPlayerState) const;
    static bool MatchesNameRules(
        const char* name,
        const std::vector<std::string>& exactList,
        const std::vector<std::string>& containsList,
        const std::vector<std::string>& excludeList);
};
