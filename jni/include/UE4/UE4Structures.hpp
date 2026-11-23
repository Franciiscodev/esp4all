#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "IMemoryReader.hpp"

/**
 * UE4 / Generic 3D engine data structures, pointer chains, and configuration types.
 */

struct Vec3 {
    float x, y, z;
    Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
};

struct FQuat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

/**
 * Standard UE4 ARM64 FTransform (100% consistent across UE4 builds: stride = 0x30 = 48 bytes).
 */
struct FTransform {
    FQuat Rotation;      // +0x00 (16B)
    Vec3  Translation;   // +0x10 (12B)
    float pad0 = 0.0f;   // +0x1C (4B padding)
    Vec3  Scale3D{1.0f, 1.0f, 1.0f}; // +0x20 (12B)
    float pad1 = 0.0f;   // +0x2C (4B padding)
};
static_assert(sizeof(FTransform) == 0x30, "FTransform must be 48 bytes (0x30 stride)");

/**
 * Standard 15-bone humanoid skeleton slots (fixed-size for zero-allocation pooling).
 */
enum BoneSlot : uint8_t {
    BONE_HEAD = 0,
    BONE_CHEST,
    BONE_PELVIS,
    BONE_L_SHOULDER,
    BONE_R_SHOULDER,
    BONE_L_ELBOW,
    BONE_R_ELBOW,
    BONE_L_WRIST,
    BONE_R_WRIST,
    BONE_L_THIGH,
    BONE_R_THIGH,
    BONE_L_KNEE,
    BONE_R_KNEE,
    BONE_L_ANKLE,
    BONE_R_ANKLE,
    BONE_SLOT_COUNT = 15
};

constexpr std::array<std::pair<uint8_t, uint8_t>, 14> kSkeletonEdges = {{
    {BONE_HEAD, BONE_CHEST},
    {BONE_CHEST, BONE_PELVIS},
    {BONE_CHEST, BONE_L_SHOULDER},
    {BONE_L_SHOULDER, BONE_L_ELBOW},
    {BONE_L_ELBOW, BONE_L_WRIST},
    {BONE_CHEST, BONE_R_SHOULDER},
    {BONE_R_SHOULDER, BONE_R_ELBOW},
    {BONE_R_ELBOW, BONE_R_WRIST},
    {BONE_PELVIS, BONE_L_THIGH},
    {BONE_L_THIGH, BONE_L_KNEE},
    {BONE_L_KNEE, BONE_L_ANKLE},
    {BONE_PELVIS, BONE_R_THIGH},
    {BONE_R_THIGH, BONE_R_KNEE},
    {BONE_R_KNEE, BONE_R_ANKLE}
}};

struct BoneProfile {
    std::array<int32_t, BONE_SLOT_COUNT> indices{};
    bool valid = false;

    BoneProfile() {
        indices.fill(-1);
    }
};

/**
 * Single step in an arbitrary pointer chain.
 * - deref == true:  addr = ReadPointer(addr + offset)
 * - deref == false: addr = addr + offset
 */
struct ChainStep {
    int64_t offset = 0;
    bool deref = true;

    ChainStep() = default;
    ChainStep(int64_t off, bool doDeref = true) : offset(off), deref(doDeref) {}
};

struct PointerChain {
    std::vector<ChainStep> steps;

    bool Empty() const noexcept { return steps.empty(); }
    void Clear() noexcept { steps.clear(); }
};

enum class MatrixLayout {
    ColumnMajor, // UE4 / PUBG / Farlight / Messiah: row-vector * M (m[0,4,8,12])
    RowMajor     // PUBG New State / work12: M * col-vector (m[0,1,2,3])
};

enum class UpAxis {
    Z, // UE4 Left-Handed Z-Up (+X Forward, +Y Right, +Z Up)
    Y  // Y-Up engines (Unity / Messiah / PUBG New State)
};

enum class CameraMode {
    Auto,        // Prefer direct ViewMatrix chain if configured, fallback to CameraCache
    ViewMatrix,  // Direct ViewMatrix 4x4 via view_matrix_chain
    CameraCache  // APlayerCameraManager -> CameraCache -> POV + trig W2S
};

enum class PositionSource {
    RelativeLocation, // RootComponent + RelativeLocation (FVector 12B)
    ComponentToWorld  // RootComponent + ComponentToWorld + 0x10 (FTransform.Translation 12B)
};

enum class EntityArrayMode {
    TArrayCount,    // UE4 TArray<T>: Data @ +0x0, int32 Count @ +0x8 (default)
    BeginEndPtr,    // std::vector-like: Data @ +0x0, EndPtr @ +0x8 (work12)
    NullTerminated  // Raw pointer array scanned until null or maxScanCount (work14)
};

/**
 * Configuration for UE4 / Engine offsets (loaded from JSON).
 */
struct UE4Offsets {
    uint64_t GWorld = 0;
    uint64_t GWorld_Intermediate = 0;
    uint64_t GEngine = 0;
    uint64_t GEngine_Intermediate = 0;
    uint64_t FNamePool = 0;
    uint64_t FNamePool_Offset = 0;

    uint32_t Engine_GameViewportClient = 0;
    uint32_t GameViewportClient_World = 0;

    uint32_t World_PersistentLevel = 0;
    uint32_t World_Levels = 0; // Optional TArray<ULevel*> for Level Streaming
    uint32_t World_NetDriver = 0; // Optional UWorld::NetDriver (bypass encrypted LocalPlayers)
    uint32_t World_OwningGameInstance = 0;
    uint32_t World_GameState = 0;

    uint32_t Level_Actors = 0;
    EntityArrayMode entityArrayMode = EntityArrayMode::TArrayCount;
    uint32_t entityArrayMaxCount = 3000;

    uint32_t GameState_PlayerArray = 0;
    uint32_t PlayerState_PawnPrivate = 0;

    uint32_t Actor_ClassPrivate = 0x10;
    uint32_t Actor_FNameIndex = 0x18;
    uint32_t Actor_RootComponent = 0;
    uint32_t Actor_PlayerState = 0;
    uint32_t Actor_Controller = 0;

    uint32_t SceneComponent_RelativeLocation = 0;
    uint32_t SceneComponent_ComponentToWorld = 0;

    uint32_t GameInstance_LocalPlayers = 0;
    uint32_t LocalPlayer_PlayerController = 0;
    uint32_t NetDriver_ServerConnection = 0;        // UNetDriver::ServerConnection (UNetConnection*)
    uint32_t ServerConnection_PlayerController = 0; // UPlayer::PlayerController (defaults to LocalPlayer_PlayerController)
    uint32_t PlayerController_PlayerCameraManager = 0;
    uint32_t PlayerController_Pawn = 0;

    uint32_t CameraManager_CameraCachePrivate = 0;
    uint32_t CameraCacheEntry_POV = 0x10;
};

/**
 * Configuration for FName resolution system (FNamePool UE4.23+ or legacy GNames).
 */
struct FNamePoolConfig {
    enum class Type {
        GNames,
        FNamePool
    };

    bool enabled = true;
    Type type = Type::FNamePool;

    uint32_t FNameEntryAllocator_CurrentBlock = 0x8;
    uint32_t FNameEntryAllocator_Blocks = 0x10;
    uint32_t FNameEntryAllocator_Stride = 0x2;
    uint16_t FNameEntry_HeaderSize = 0x2;

    uint32_t ChunkSize = 0x4000;
    uint32_t EntryStride = 0x8;
    uint32_t NameStringOffset = 0xC;
};

/**
 * Camera configuration supporting both direct ViewMatrix chain and CameraCache POV
 * (with optional heuristic PlayerController discovery when LocalPlayers is encrypted).
 */
struct CameraConfig {
    CameraMode mode = CameraMode::Auto;
    MatrixLayout matrixLayout = MatrixLayout::ColumnMajor;
    UpAxis upAxis = UpAxis::Z;

    PointerChain viewMatrixChain;

    uint32_t cameraCacheOffset = 0;
    uint32_t povOffset = 0x10;
    uint32_t locationOffset = 0x0;
    uint32_t rotationOffset = 0xC;
    uint32_t fovOffset = 0x18;
    bool fovRelativeToCache = false;

    // Fallback: scan Actors[] for PlayerController via plausible CameraManager FOV (1..179 deg)
    bool heuristicPcScan = false;
};

/**
 * Configurable entity filtering pipeline (None, Sentinel, ClassName, ClassPrivate + Heuristic Bootstrap).
 */
struct EntityFilterConfig {
    enum class Mode {
        None,
        Sentinel,
        ClassName,
        ClassPrivate
    };

    enum class SentinelType {
        Float,
        Int32
    };

    Mode mode = Mode::None;
    float maxDistanceMeters = 500.0f;
    float maxZDelta = 10000.0f;
    bool skipZeroPos = true;
    bool skipLocalPlayer = true;
    bool skipSameTeam = false;
    bool skipDead = false;

    // Sentinel mode config (direct offset OR arbitrary chain on actor, e.g. work12)
    uint32_t sentinelOffset = 0;
    PointerChain sentinelChain;
    SentinelType sentinelType = SentinelType::Float;
    float sentinelFloatValue = 0.0f;
    float sentinelEpsilon = 0.1f;
    int32_t sentinelIntValue = 0;

    // ClassName mode config
    std::vector<std::string> classNameContains;
    std::vector<std::string> classNameExact;
    std::vector<std::string> classNameExclude;

    // ClassPrivate (+0x10) mode config with optional heuristic bootstrap (ESP4All_SKILL.md:183-195)
    uint32_t classPrivateOffset = 0x10;
    std::vector<std::string> classPrivateTargetNames;
    bool classPrivateHeuristicBootstrap = true;
    bool classPrivateUseLocalPawn = true;
    float heuristicMinHp = 1.0f;
    float heuristicMaxHp = 1000.0f;
};

/**
 * Configurable Bone ESP settings via JSON (optional, disabled by default).
 */
struct BoneEspConfig {
    bool enabled = false;
    uint32_t meshOffset = 0;
    uint32_t componentToWorldOffset = 0;
    uint32_t boneArrayOffset = 0;
    uint32_t boneArrayBias = 0;       // e.g. 0x30 in PUBG Mobile (src3/src4)
    uint32_t boneStride = 0x30;       // FTransform stride (48 bytes)
    uint32_t boneCountOffset = 0;     // Defaults to boneArrayOffset + 0x8 (TArray::Num)
    float maxDistanceMeters = 250.0f;
    float maxSegmentPixels = 350.0f;  // Visual sanity filter against corrupted bone spikes

    BoneProfile defaultProfile;
    std::unordered_map<int32_t, BoneProfile> profilesByCount;
};

/**
 * Configurable actor attributes (AActor, APlayerState, or hybrid per-field overrides).
 */
struct ActorAttributesConfig {
    PositionSource positionSource = PositionSource::RelativeLocation;
    bool attributesOnPlayerState = false;

    uint32_t healthOffset = 0;
    uint32_t maxHealthOffset = 0;
    uint32_t teamIdOffset = 0;
    uint32_t playerNameOffset = 0;
    bool playerNameIsUtf16 = true;
    uint32_t isBotOffset = 0;

    uint32_t psHealthOffset = 0;
    uint32_t psMaxHealthOffset = 0;
    uint32_t psTeamIdOffset = 0;
    uint32_t psPlayerNameOffset = 0;

    float boxBaseOffset = 0.0f;  // Vertical offset for feet (e.g., -0.5f m in work12, -5.0f cm in PUBG)
    float boxHeight = 165.0f;    // Vertical height from base to head (cm in UE4, ~1.9f m in Y-Up engines)
    float distanceScale = 0.01f; // UE4 cm -> meters (1.0f for meter-scale engines)
};

/**
 * Optional configuration for in-process (internal .so) native UE4 getter invocation
 * and GUObjectArray -> GameViewport -> World discovery.
 * Ignored in standalone external executable mode.
 */
struct InternalConfig {
    uint64_t gNamesGetterOffset = 0;       // Module-relative offset of `void* (*)()` returning GNames/FNamePool*
    uint64_t gNamesGetterAddOffset = 0;    // Optional byte offset added to GNames_getter return value
    bool gNamesGetterDeref = false;        // True if GNames_getter returns pointer-to-pointer (TNameEntryArray**)

    uint64_t getActorArrayFnOffset = 0;    // Module-relative offset of `void* (*)(uintptr_t levelPtr)` -> TArray<AActor*>*

    uint64_t gUObjectArrayOffset = 0;      // Module-relative offset of FUObjectArray::ObjObjects
    uint32_t uObjectArrayElementSize = 0x18;   // sizeof(FUObjectItem), typically 0x18 on ARM64
    uint32_t uObjectArrayChunkSize = 0x10000;  // Elements per chunk in FChunkedFixedUObjectArray
    uint32_t uObjectArrayNumOffset = 0x14;     // Offset of NumElements relative to ObjObjects (0x14 chunked, 0xC flat)
    bool uObjectArrayChunked = true;           // true = FChunkedFixedUObjectArray (UE4.21+), false = flat TUObjectArray
    std::string viewportClassName = "GameViewportClient"; // Substring match for UGameViewportClient discovery

    bool safeMemoryRead = true;            // true = SAFE_SELF_VM (SIGSEGV-proof), false = DIRECT_MEMCPY
};

/**
 * Camera view information (matches UE4's FMinimalViewInfo)
 */
struct FMinimalViewInfo {
    Vec3 Location;
    Vec3 Rotation; // x = Pitch, y = Yaw, z = Roll (degrees)
    float FOV = 90.0f;
};

/**
 * Per-frame diagnostics capturing every link in the resolution chain.
 */
struct ChainDiagnostics {
    uint32_t pid = 0;
    uint64_t moduleBase = 0;
    uint64_t uWorld = 0;
    bool usedGEngineFallback = false;
    bool usedEntityListChain = false;
    bool usedBridgeWorld = false;
    uint64_t persistentLevel = 0;
    uint32_t levelsCount = 0;
    uint64_t gameState = 0;
    uint64_t namePoolPtr = 0;
    bool nameSystemEnabled = false;
    bool usedBridgeNamePool = false;
    size_t nameCacheEntries = 0;
    size_t acceptedClassCount = 0;
    size_t rejectedClassCount = 0;
    bool usedHeuristicClassBootstrap = false;
    uint64_t actorsData = 0;
    int32_t actorsCount = 0;
    bool usedBridgeActors = false;
    uint32_t droppedByPoolCap = 0;
    uint64_t localController = 0;
    bool usedNetDriverLocalPlayer = false;
    bool usedHeuristicCameraPC = false;
    uint64_t localPawn = 0;
    bool hasLocalPos = false;
    Vec3 localPos;
    uint64_t cameraManager = 0;
    uint64_t matrixAddress = 0;
    bool hasViewMatrix = false;
    bool hasCameraPOV = false;
    bool usingMatrixCamera = false;
    bool usedBridgeCamera = false;
    bool bonesEnabled = false;
};

namespace UE4Constants {
    constexpr uint32_t UObject_VTable = 0x0;
    constexpr uint32_t UObject_Flags = 0x8;
    constexpr uint32_t UObject_InternalIndex = 0xC;
    constexpr uint32_t UObject_ClassPrivate = 0x10;
    constexpr uint32_t UObject_NamePrivate = 0x18;
    constexpr uint32_t UObject_OuterPrivate = 0x20;

    constexpr uint32_t TArray_Data = 0x0;
    constexpr uint32_t TArray_Count = 0x8;
    constexpr uint32_t TArray_Max = 0xC;

    constexpr uint32_t FTransform_Rotation = 0x0;
    constexpr uint32_t FTransform_Translation = 0x10;
    constexpr uint32_t FTransform_Scale3D = 0x20;
    constexpr uint32_t FTransform_Stride = 0x30;

    constexpr uint32_t POV_Location = 0x0;
    constexpr uint32_t POV_Rotation = 0xC;
    constexpr uint32_t POV_FOV = 0x18;

    constexpr uint32_t FVector_Size = 0xC;
    constexpr uint32_t FRotator_Size = 0xC;

    constexpr uint32_t FName_ComparisonIndex = 0x0;
    constexpr uint32_t FName_Number = 0x4;

    constexpr uint32_t FNamePool_MaxBlocks = 8192;
    constexpr uint32_t FNameEntry_DefaultStride = 2;
    constexpr uint16_t FNameEntry_WideBit = 0x1;
    constexpr uint16_t FNameEntry_LenShift = 6;
    constexpr uint16_t FNameEntry_LenMask = 0x3FF;

    constexpr uint64_t MinValidPointer = MemoryUtils::MIN_VALID_PTR;
    constexpr uint64_t MaxValidPointer = MemoryUtils::MAX_VALID_PTR;
    constexpr uint32_t PointerAlignment = 4;
}
