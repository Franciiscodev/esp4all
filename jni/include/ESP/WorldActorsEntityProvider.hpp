#pragma once
#include <vector>
#include "ESP/IEntityProvider.hpp"
#include "UE4/UE4Context.hpp"

/**
 * World Actors Entity Provider.
 * Iterates UWorld -> PersistentLevel (or UWorld -> Levels[] for Level Streaming)
 * -> TArray<AActor*> directly into a pre-allocated EntityPool with zero per-frame allocations.
 */
class WorldActorsEntityProvider : public IEntityProvider {
public:
    explicit WorldActorsEntityProvider(UE4Context& context);

    bool Initialize();

    size_t CollectEntities(EntityPool& outPool) override;
    bool GetLocalPlayerPosition(float& outX, float& outY, float& outZ) override;
    uint64_t GetLocalPlayerAddress() override;

private:
    UE4Context& context_;
    std::vector<uint64_t> levelBuffer_; // Reused across frames for UWorld::Levels[]
    std::vector<uint64_t> ptrBuffer_;   // Reused across frames for ULevel::Actors[]

    void CollectFromLevel(
        uint64_t levelPtr,
        EntityPool& outPool,
        int32_t& totalRawActors,
        uint64_t& firstActorsData,
        uint32_t& droppedByPoolCap);
};
