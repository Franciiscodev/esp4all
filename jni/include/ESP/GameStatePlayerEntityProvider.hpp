#pragma once
#include <vector>
#include "ESP/IEntityProvider.hpp"
#include "UE4/UE4Context.hpp"

/**
 * GameState Player Entity Provider.
 * Iterates UWorld -> GameState -> PlayerArray (TArray<APlayerState*>) -> PawnPrivate
 * directly into a pre-allocated EntityPool with zero per-frame heap allocations.
 */
class GameStatePlayerEntityProvider : public IEntityProvider {
public:
    explicit GameStatePlayerEntityProvider(UE4Context& context);

    bool Initialize();

    size_t CollectEntities(EntityPool& outPool) override;
    bool GetLocalPlayerPosition(float& outX, float& outY, float& outZ) override;
    uint64_t GetLocalPlayerAddress() override;

private:
    UE4Context& context_;
    std::vector<uint64_t> ptrBuffer_; // Reused across frames (zero per-frame alloc)
};
