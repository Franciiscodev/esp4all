#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "UE4/UE4Math.hpp"
#include "UE4/UE4Structures.hpp"

/**
 * Zero-allocation pre-allocated entity slot stored inside EntityPool.
 * Contains world coordinates, attributes, projected box, and optional 15-bone skeleton.
 */
struct PooledEntity {
    uint64_t address = 0;
    uint64_t playerState = 0;
    uint64_t classPrivate = 0;

    char className[64] = {};
    char playerName[64] = {};
    char itemLabel[64] = {};

    Vec3 worldPos;
    Vec3 screenPoint;
    UE4Math::ScreenBox box;

    // Optional 15-bone skeleton (zero heap allocs; populated only when bones.enabled == true)
    std::array<Vec3, BONE_SLOT_COUNT> boneWorld{};
    std::array<Vec3, BONE_SLOT_COUNT> boneScreen{};
    std::array<uint8_t, BONE_SLOT_COUNT> boneValid{};
    std::array<uint8_t, BONE_SLOT_COUNT> boneOnScreen{};
    bool hasSkeleton = false;

    float distanceMeters = 0.0f;
    float health = 0.0f;
    float maxHealth = 100.0f;
    int32_t teamId = -1;

    bool isBot = false;
    bool isWorldItem = false;
    bool hasHealth = false;
    bool hasTeamId = false;
    bool hasPlayerName = false;
    bool hasItemLabel = false;
    bool onScreen = false;
    bool isValid = false;

    void Reset() noexcept {
        address = 0;
        playerState = 0;
        classPrivate = 0;
        className[0] = '\0';
        playerName[0] = '\0';
        itemLabel[0] = '\0';
        worldPos = Vec3();
        screenPoint = Vec3();
        box = UE4Math::ScreenBox{};
        boneValid.fill(0);
        boneOnScreen.fill(0);
        hasSkeleton = false;
        distanceMeters = 0.0f;
        health = 0.0f;
        maxHealth = 100.0f;
        teamId = -1;
        isBot = false;
        isWorldItem = false;
        hasHealth = false;
        hasTeamId = false;
        hasPlayerName = false;
        hasItemLabel = false;
        onScreen = false;
        isValid = false;
    }
};

/**
 * Fixed-capacity Object Pool for zero-allocation ESP frame loops (ESP4All_SKILL.md:529-545).
 */
class EntityPool {
public:
    static constexpr size_t CAPACITY = 256;

    void Clear() noexcept {
        count_ = 0;
    }

    PooledEntity* Allocate() noexcept {
        if (count_ >= CAPACITY) {
            return nullptr;
        }
        PooledEntity& slot = slots_[count_++];
        slot.Reset();
        return &slot;
    }

    void PopBack() noexcept {
        if (count_ > 0) {
            --count_;
        }
    }

    size_t Count() const noexcept { return count_; }
    size_t Capacity() const noexcept { return CAPACITY; }
    bool IsFull() const noexcept { return count_ >= CAPACITY; }
    bool Empty() const noexcept { return count_ == 0; }

    PooledEntity& operator[](size_t index) noexcept { return slots_[index]; }
    const PooledEntity& operator[](size_t index) const noexcept { return slots_[index]; }

    PooledEntity* begin() noexcept { return slots_.data(); }
    PooledEntity* end() noexcept { return slots_.data() + count_; }
    const PooledEntity* begin() const noexcept { return slots_.data(); }
    const PooledEntity* end() const noexcept { return slots_.data() + count_; }

private:
    std::array<PooledEntity, CAPACITY> slots_{};
    size_t count_ = 0;
};

/**
 * Lock-free Double-Buffered EntityPool (ESP4All_SKILL.md:616-617).
 * Allows an engine tick thread to populate the back buffer while the ImGui
 * render thread reads the published front buffer concurrently with zero locks
 * and zero heap allocations.
 */
class DoubleBufferedEntityPool {
public:
    EntityPool& AcquireWriteBuffer() noexcept {
        const uint8_t writeIdx = static_cast<uint8_t>(1U - frontIndex_.load(std::memory_order_relaxed));
        return buffers_[writeIdx];
    }

    void PublishWriteBuffer() noexcept {
        const uint8_t writeIdx = static_cast<uint8_t>(1U - frontIndex_.load(std::memory_order_relaxed));
        frontIndex_.store(writeIdx, std::memory_order_release);
    }

    const EntityPool& GetReadBuffer() const noexcept {
        return buffers_[frontIndex_.load(std::memory_order_acquire)];
    }

    void ClearAll() noexcept {
        buffers_[0].Clear();
        buffers_[1].Clear();
    }

private:
    EntityPool buffers_[2];
    std::atomic<uint8_t> frontIndex_{0};
};

/**
 * Backwards-compatible owning Entity struct (used by GetEntities() convenience wrapper).
 */
struct Entity {
    uint64_t address = 0;
    uint64_t playerState = 0;
    uint64_t classPrivate = 0;
    std::string className;
    std::string playerName;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float health = 0.0f;
    float maxHealth = 100.0f;
    int32_t teamId = -1;
    bool isBot = false;
    bool hasHealth = false;
    bool hasTeamId = false;
    bool hasPlayerName = false;
    bool isValid = false;

    Entity() = default;

    Entity(uint64_t addr, const std::string& name, float px, float py, float pz)
        : address(addr), className(name), x(px), y(py), z(pz), isValid(true) {}

    static Entity FromPooled(const PooledEntity& p) {
        Entity e;
        e.address = p.address;
        e.playerState = p.playerState;
        e.classPrivate = p.classPrivate;
        e.className = p.className;
        e.playerName = p.playerName;
        e.x = p.worldPos.x;
        e.y = p.worldPos.y;
        e.z = p.worldPos.z;
        e.health = p.health;
        e.maxHealth = p.maxHealth;
        e.teamId = p.teamId;
        e.isBot = p.isBot;
        e.hasHealth = p.hasHealth;
        e.hasTeamId = p.hasTeamId;
        e.hasPlayerName = p.hasPlayerName;
        e.isValid = p.isValid;
        return e;
    }
};

class IEntityProvider {
public:
    virtual ~IEntityProvider() = default;

    virtual size_t CollectEntities(EntityPool& outPool) = 0;

    virtual std::vector<Entity> GetEntities() {
        EntityPool tempPool;
        CollectEntities(tempPool);
        std::vector<Entity> result;
        result.reserve(tempPool.Count());
        for (const auto& pooled : tempPool) {
            result.push_back(Entity::FromPooled(pooled));
        }
        return result;
    }

    virtual bool GetLocalPlayerPosition(float& outX, float& outY, float& outZ) = 0;
    virtual uint64_t GetLocalPlayerAddress() = 0;
};
