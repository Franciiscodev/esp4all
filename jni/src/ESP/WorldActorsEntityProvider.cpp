#include "ESP/WorldActorsEntityProvider.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>

WorldActorsEntityProvider::WorldActorsEntityProvider(UE4Context &context)
    : context_(context) {
  levelBuffer_.reserve(64);
  ptrBuffer_.reserve(2048);
}

bool WorldActorsEntityProvider::Initialize() {
  if (!context_.IsInitialized()) {
    std::cerr << "[WorldActorsEntityProvider] UE4Context is not initialized"
              << std::endl;
    return false;
  }

  const auto &cfg = context_.GetConfig();
  const auto &offsets = cfg.GetOffsets();
  const auto *bridge = context_.GetInternalBridge();
  const bool hasBridgeActors =
      (bridge != nullptr && bridge->HasActorsArrayResolver());
  const bool hasDirectChain = !cfg.GetEntityListChain().Empty();
  const bool hasLevelActors =
      (offsets.Level_Actors != 0 || hasBridgeActors) &&
      (offsets.World_PersistentLevel != 0 || offsets.World_Levels != 0);

  if (!hasDirectChain && !hasLevelActors) {
    std::cerr << "[WorldActorsEntityProvider] Missing "
                 "World_PersistentLevel/World_Levels + Level_Actors (or "
                 "InternalBridge actors resolver) or EntityList_chain"
              << std::endl;
    return false;
  }

  return true;
}

uint64_t WorldActorsEntityProvider::GetLocalPlayerAddress() {
  return context_.GetLocalPawn();
}

bool WorldActorsEntityProvider::GetLocalPlayerPosition(float &outX, float &outY,
                                                       float &outZ) {
  if (!context_.HasLocalPlayerPosition()) {
    return false;
  }
  const Vec3 &p = context_.GetLocalPlayerPosition();
  outX = p.x;
  outY = p.y;
  outZ = p.z;
  return true;
}

void WorldActorsEntityProvider::CollectFromLevel(uint64_t levelPtr,
                                                 EntityPool &outPool,
                                                 int32_t &totalRawActors,
                                                 uint64_t &firstActorsData,
                                                 uint32_t &droppedByPoolCap) {
  IMemoryReader &memory = context_.GetMemory();
  const ConfigManager &config = context_.GetConfig();
  const auto &offsets = config.GetOffsets();
  const auto &filter = config.GetEntityFilterConfig();
  const auto &actorCfg = config.GetActorConfig();
  const auto &camCfg = config.GetCameraConfig();
  const bool bonesEnabled = config.GetBoneConfig().enabled;

  uint64_t actorsData = 0;
  int32_t actorsCount = 0;
  bool resolvedViaBridge = false;

  // Priority 0: In-process ActorsArrayResolver callback (e.g. GetActorArray_fn(ULevel*))
  if (const auto *bridge = context_.GetInternalBridge()) {
    if (bridge->TryResolveLevelActors(levelPtr, actorsData, actorsCount)) {
      resolvedViaBridge = true;
      context_.GetMutableDiagnostics().usedBridgeActors = true;
    }
  }

  if (!resolvedViaBridge) {
    if (offsets.Level_Actors == 0 && !context_.GetDiagnostics().usedEntityListChain) {
      return;
    }
    const uint64_t arrayHeaderAddr = levelPtr + offsets.Level_Actors;

    switch (offsets.entityArrayMode) {
    case EntityArrayMode::TArrayCount: {
      actorsData =
          memory.ReadPointer(arrayHeaderAddr + UE4Constants::TArray_Data);
      actorsCount =
          memory.Read<int32_t>(arrayHeaderAddr + UE4Constants::TArray_Count);
      break;
    }
    case EntityArrayMode::BeginEndPtr: {
      actorsData = memory.ReadPointer(arrayHeaderAddr + 0x0);
      const uint64_t endPtr = memory.ReadPointer(arrayHeaderAddr + 0x8);
      if (MemoryUtils::IsValidPtr(actorsData) && endPtr > actorsData) {
        const uint64_t diff = (endPtr - actorsData) / sizeof(uint64_t);
        actorsCount = static_cast<int32_t>(
            std::min<uint64_t>(diff, offsets.entityArrayMaxCount));
      }
      break;
    }
    case EntityArrayMode::NullTerminated: {
      actorsData = memory.ReadPointer(arrayHeaderAddr + 0x0);
      actorsCount = static_cast<int32_t>(offsets.entityArrayMaxCount);
      break;
    }
    }
  }

  if (!MemoryUtils::IsValidPtr(actorsData) || actorsCount <= 0 ||
      actorsCount > 10000) {
    return;
  }

  if (firstActorsData == 0) {
    firstActorsData = actorsData;
  }

  ptrBuffer_.resize(static_cast<size_t>(actorsCount));
  if (!memory.ReadBuffer(actorsData, ptrBuffer_.data(),
                         ptrBuffer_.size() * sizeof(uint64_t))) {
    return;
  }

  if (!resolvedViaBridge &&
      offsets.entityArrayMode == EntityArrayMode::NullTerminated) {
    size_t actualCount = 0;
    while (actualCount < ptrBuffer_.size() && ptrBuffer_[actualCount] != 0) {
      ++actualCount;
    }
    ptrBuffer_.resize(actualCount);
  }

  totalRawActors += static_cast<int32_t>(ptrBuffer_.size());

  if (camCfg.heuristicPcScan && !context_.HasValidCamera()) {
    context_.ScanActorsForCameraController(ptrBuffer_.data(),
                                           ptrBuffer_.size());
  }

  const uint64_t localPawn = context_.GetLocalPawn();
  const bool hasLocalPos = context_.HasLocalPlayerPosition();
  const Vec3 localPos = context_.GetLocalPlayerPosition();
  const auto &vehMap = config.GetVehicleMap();
  const auto &lootMap = config.GetLootMap();
  const bool hasWorldMaps = !vehMap.empty() || !lootMap.empty();
  const bool nameSysEnabled = context_.IsNameSystemEnabled();

  for (size_t idx = 0; idx < ptrBuffer_.size(); ++idx) {
    if (outPool.IsFull()) {
      for (size_t rem = idx; rem < ptrBuffer_.size(); ++rem) {
        const uint64_t remActor = MemoryUtils::StripTag(ptrBuffer_[rem]);
        if (!MemoryUtils::IsValidPtr(remActor)) {
          continue;
        }
        if (filter.skipLocalPlayer && localPawn != 0 && remActor == localPawn) {
          continue;
        }
        ++droppedByPoolCap;
      }
      break;
    }

    const uint64_t actor = MemoryUtils::StripTag(ptrBuffer_[idx]);
    if (!MemoryUtils::IsValidPtr(actor)) {
      continue;
    }

    if (filter.skipLocalPlayer && localPawn != 0 && actor == localPawn) {
      continue;
    }

    uint64_t classPrivate = 0;
    const bool passedQuickFilter =
        context_.QuickPreFilter(actor, &classPrivate);
    if (!passedQuickFilter && !hasWorldMaps) {
      continue;
    }

    Vec3 pos;
    if (!context_.ReadActorPosition(actor, pos)) {
      continue;
    }

    if (filter.skipZeroPos && pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) {
      continue;
    }

    float distMeters = 0.0f;
    if (hasLocalPos) {
      if (filter.maxZDelta > 0.0f &&
          UE4Math::VerticalDelta(localPos, pos, camCfg.upAxis) >
              filter.maxZDelta) {
        continue;
      }
      distMeters =
          UE4Math::DistanceMeters(localPos, pos, actorCfg.distanceScale);
      if (filter.maxDistanceMeters > 0.0f &&
          distMeters > filter.maxDistanceMeters) {
        continue;
      }
    }

    char classBuf[64] = {};
    const bool needClassName =
        nameSysEnabled && (filter.mode == EntityFilterConfig::Mode::ClassName ||
                           filter.mode == EntityFilterConfig::Mode::None ||
                           hasWorldMaps || passedQuickFilter);
    if (needClassName) {
      context_.ReadObjectNameInto(actor, classBuf, sizeof(classBuf));
    }

    const bool isPrimaryEntity =
        passedQuickFilter &&
        context_.MatchesEntityFilter(actor, needClassName ? classBuf : nullptr,
                                     classPrivate);

    const char *matchedItemLabel = nullptr;
    if (!isPrimaryEntity && hasWorldMaps && classBuf[0] != '\0') {
      auto itV = vehMap.find(classBuf);
      if (itV != vehMap.end()) {
        matchedItemLabel = itV->second.c_str();
      } else {
        auto itL = lootMap.find(classBuf);
        if (itL != lootMap.end()) {
          matchedItemLabel = itL->second.c_str();
        }
      }
    }

    if (!isPrimaryEntity && matchedItemLabel == nullptr) {
      continue;
    }

    PooledEntity *slot = outPool.Allocate();
    if (!slot) {
      break;
    }

    slot->address = actor;
    slot->classPrivate = classPrivate;
    MemoryUtils::CopyStringSafe(slot->className, sizeof(slot->className),
                                classBuf);
    slot->worldPos = pos;
    slot->distanceMeters = distMeters;
    slot->isWorldItem = !isPrimaryEntity;
    slot->isValid = true;

    if (matchedItemLabel != nullptr) {
      MemoryUtils::CopyStringSafe(slot->itemLabel, sizeof(slot->itemLabel),
                                  matchedItemLabel);
      slot->hasItemLabel = true;
    }

    if (isPrimaryEntity) {
      context_.ReadActorAttributes(actor, 0, *slot);

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
        context_.ReadActorSkeleton(actor, *slot);
      }
    }
  }
}

size_t WorldActorsEntityProvider::CollectEntities(EntityPool &outPool) {
  outPool.Clear();
  levelBuffer_.clear();

  IMemoryReader &memory = context_.GetMemory();
  const auto &offsets = context_.GetConfig().GetOffsets();
  auto &diag = context_.GetMutableDiagnostics();

  // Path 1: Multi-level streaming via UWorld::Levels[] (TArray<ULevel*>)
  if (!diag.usedEntityListChain && offsets.World_Levels != 0 &&
      MemoryUtils::IsValidPtr(context_.GetUWorld())) {
    const uint64_t levelsArrAddr = context_.GetUWorld() + offsets.World_Levels;
    const uint64_t levelsData =
        memory.ReadPointer(levelsArrAddr + UE4Constants::TArray_Data);
    const int32_t levelsCount =
        memory.Read<int32_t>(levelsArrAddr + UE4Constants::TArray_Count);

    if (MemoryUtils::IsValidPtr(levelsData) && levelsCount > 0 &&
        levelsCount <= 128) {
      levelBuffer_.resize(static_cast<size_t>(levelsCount));
      if (memory.ReadBuffer(levelsData, levelBuffer_.data(),
                            levelBuffer_.size() * sizeof(uint64_t))) {
        // Filter invalid pointers and deduplicate in-place (zero allocations)
        size_t writeIdx = 0;
        for (size_t i = 0; i < levelBuffer_.size(); ++i) {
          const uint64_t lvl = MemoryUtils::StripTag(levelBuffer_[i]);
          if (!MemoryUtils::IsValidPtr(lvl)) {
            continue;
          }
          bool duplicate = false;
          for (size_t j = 0; j < writeIdx; ++j) {
            if (levelBuffer_[j] == lvl) {
              duplicate = true;
              break;
            }
          }
          if (!duplicate) {
            levelBuffer_[writeIdx++] = lvl;
          }
        }
        levelBuffer_.resize(writeIdx);
      } else {
        levelBuffer_.clear();
      }
    }
  }

  // Path 2 (default or fallback): single PersistentLevel / EntityList container
  if (levelBuffer_.empty()) {
    const uint64_t persistent = context_.GetPersistentLevel();
    if (MemoryUtils::IsValidPtr(persistent)) {
      levelBuffer_.push_back(persistent);
    }
  }

  diag.levelsCount = static_cast<uint32_t>(levelBuffer_.size());
  if (levelBuffer_.empty()) {
    return 0;
  }

  int32_t totalRawActors = 0;
  uint64_t firstActorsData = 0;
  uint32_t droppedByPoolCap = 0;

  for (size_t lvlIdx = 0; lvlIdx < levelBuffer_.size(); ++lvlIdx) {
    const uint64_t lvl = levelBuffer_[lvlIdx];
    if (outPool.IsFull()) {
      // Estimate remaining actors in skipped streaming levels (1 read/level,
      // zero alloc)
      uint64_t remData = 0;
      int32_t remCount = 0;
      const auto *bridge = context_.GetInternalBridge();
      if (bridge != nullptr &&
          bridge->TryResolveLevelActors(lvl, remData, remCount)) {
        if (remCount > 0 && remCount <= 10000) {
          droppedByPoolCap += static_cast<uint32_t>(remCount);
        }
      } else if (offsets.Level_Actors != 0 || diag.usedEntityListChain) {
        const uint64_t arrAddr = lvl + offsets.Level_Actors;
        if (offsets.entityArrayMode == EntityArrayMode::TArrayCount) {
          remCount = memory.Read<int32_t>(arrAddr + UE4Constants::TArray_Count);
          if (remCount > 0 && remCount <= 10000) {
            droppedByPoolCap += static_cast<uint32_t>(remCount);
          }
        } else if (offsets.entityArrayMode == EntityArrayMode::BeginEndPtr) {
          const uint64_t dataPtr = memory.ReadPointer(arrAddr + 0x0);
          const uint64_t endPtr = memory.ReadPointer(arrAddr + 0x8);
          if (MemoryUtils::IsValidPtr(dataPtr) && endPtr > dataPtr) {
            const uint64_t diff =
                std::min<uint64_t>((endPtr - dataPtr) / sizeof(uint64_t),
                                   offsets.entityArrayMaxCount);
            droppedByPoolCap += static_cast<uint32_t>(diff);
          }
        } else {
          ++droppedByPoolCap;
        }
      }
      continue;
    }
    CollectFromLevel(lvl, outPool, totalRawActors, firstActorsData,
                     droppedByPoolCap);
  }

  diag.actorsData = firstActorsData;
  diag.actorsCount = totalRawActors;
  diag.droppedByPoolCap = droppedByPoolCap;
  return outPool.Count();
}
