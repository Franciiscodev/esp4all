#include "ESP/GameStatePlayerEntityProvider.hpp"
#include <cmath>
#include <iostream>

GameStatePlayerEntityProvider::GameStatePlayerEntityProvider(UE4Context& context)
    : context_(context) {
    ptrBuffer_.reserve(128);
}

bool GameStatePlayerEntityProvider::Initialize() {
    if (!context_.IsInitialized()) {
        std::cerr << "[GameStatePlayerEntityProvider] UE4Context is not initialized" << std::endl;
        return false;
    }

    const auto& offsets = context_.GetConfig().GetOffsets();
    if (offsets.World_GameState == 0 ||
        offsets.GameState_PlayerArray == 0 ||
        offsets.PlayerState_PawnPrivate == 0) {
        std::cerr << "[GameStatePlayerEntityProvider] Missing World_GameState, GameState_PlayerArray, or PlayerState_PawnPrivate offset"
                  << std::endl;
        return false;
    }

    return true;
}

uint64_t GameStatePlayerEntityProvider::GetLocalPlayerAddress() {
    return context_.GetLocalPawn();
}

bool GameStatePlayerEntityProvider::GetLocalPlayerPosition(float& outX, float& outY, float& outZ) {
    if (!context_.HasLocalPlayerPosition()) {
        return false;
    }
    const Vec3& p = context_.GetLocalPlayerPosition();
    outX = p.x;
    outY = p.y;
    outZ = p.z;
    return true;
}

size_t GameStatePlayerEntityProvider::CollectEntities(EntityPool& outPool) {
    outPool.Clear();

    const uint64_t gameState = context_.GetGameState();
    if (!MemoryUtils::IsValidPtr(gameState)) {
        return 0;
    }

    IMemoryReader& memory = context_.GetMemory();
    const ConfigManager& config = context_.GetConfig();
    const auto& offsets = config.GetOffsets();
    const auto& filter = config.GetEntityFilterConfig();
    const auto& actorCfg = config.GetActorConfig();
    const auto& camCfg = config.GetCameraConfig();
    const bool bonesEnabled = config.GetBoneConfig().enabled;

    const uint64_t playerArrayAddr = gameState + offsets.GameState_PlayerArray;
    const uint64_t playerArrayData = memory.ReadPointer(playerArrayAddr + UE4Constants::TArray_Data);
    const int32_t playerCount = memory.Read<int32_t>(playerArrayAddr + UE4Constants::TArray_Count);

    auto& diag = context_.GetMutableDiagnostics();
    diag.actorsData = playerArrayData;
    diag.actorsCount = playerCount;

    if (!MemoryUtils::IsValidPtr(playerArrayData) || playerCount <= 0 || playerCount > 1000) {
        return 0;
    }

    ptrBuffer_.resize(static_cast<size_t>(playerCount));
    if (!memory.ReadBuffer(playerArrayData, ptrBuffer_.data(), ptrBuffer_.size() * sizeof(uint64_t))) {
        return 0;
    }

    const uint64_t localPawn = context_.GetLocalPawn();
    const bool hasLocalPos = context_.HasLocalPlayerPosition();
    const Vec3 localPos = context_.GetLocalPlayerPosition();
    const bool nameSysEnabled = context_.IsNameSystemEnabled();
    uint32_t droppedByPoolCap = 0;

    for (size_t idx = 0; idx < ptrBuffer_.size(); ++idx) {
        if (outPool.IsFull()) {
            for (size_t rem = idx; rem < ptrBuffer_.size(); ++rem) {
                const uint64_t remPS = MemoryUtils::StripTag(ptrBuffer_[rem]);
                if (MemoryUtils::IsValidPtr(remPS)) {
                    ++droppedByPoolCap;
                }
            }
            break;
        }

        const uint64_t playerStatePtr = MemoryUtils::StripTag(ptrBuffer_[idx]);
        if (!MemoryUtils::IsValidPtr(playerStatePtr)) {
            continue;
        }

        const uint64_t pawnPtr = memory.ReadPointer(playerStatePtr + offsets.PlayerState_PawnPrivate);
        if (!MemoryUtils::IsValidPtr(pawnPtr)) {
            continue;
        }

        if (filter.skipLocalPlayer && localPawn != 0 && pawnPtr == localPawn) {
            continue;
        }

        Vec3 pos;
        if (!context_.ReadActorPosition(pawnPtr, pos)) {
            continue;
        }

        if (filter.skipZeroPos && pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) {
            continue;
        }

        float distMeters = 0.0f;
        if (hasLocalPos) {
            if (filter.maxZDelta > 0.0f &&
                UE4Math::VerticalDelta(localPos, pos, camCfg.upAxis) > filter.maxZDelta) {
                continue;
            }
            distMeters = UE4Math::DistanceMeters(localPos, pos, actorCfg.distanceScale);
            if (filter.maxDistanceMeters > 0.0f && distMeters > filter.maxDistanceMeters) {
                continue;
            }
        }

        char classBuf[64] = {};
        if (nameSysEnabled) {
            if (!context_.ReadObjectNameInto(pawnPtr, classBuf, sizeof(classBuf))) {
                context_.ReadObjectNameInto(playerStatePtr, classBuf, sizeof(classBuf));
            }
        }

        if (filter.mode != EntityFilterConfig::Mode::None) {
            if (!context_.MatchesEntityFilter(pawnPtr, nameSysEnabled ? classBuf : nullptr)) {
                continue;
            }
        }

        PooledEntity* slot = outPool.Allocate();
        if (!slot) {
            break;
        }

        slot->address = pawnPtr;
        slot->playerState = playerStatePtr;
        MemoryUtils::CopyStringSafe(slot->className, sizeof(slot->className),
                                    classBuf[0] != '\0' ? classBuf : "Player");
        slot->worldPos = pos;
        slot->distanceMeters = distMeters;
        slot->isWorldItem = false;
        slot->isValid = true;

        context_.ReadActorAttributes(pawnPtr, playerStatePtr, *slot);

        if (filter.skipSameTeam && slot->hasTeamId && context_.HasLocalTeamId() &&
            slot->teamId == context_.GetLocalTeamId()) {
            outPool.PopBack();
            continue;
        }
        if (filter.skipDead && slot->hasHealth && slot->health <= 0.0f) {
            outPool.PopBack();
            continue;
        }

        if (bonesEnabled) {
            context_.ReadActorSkeleton(pawnPtr, *slot);
        }
    }

    diag.droppedByPoolCap = droppedByPoolCap;
    return outPool.Count();
}
