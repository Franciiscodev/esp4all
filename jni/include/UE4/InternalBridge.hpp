#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <type_traits>
#include "IMemoryReader.hpp"
#include "UE4/UE4Structures.hpp"

/**
 * Lock-free SeqLock snapshot for tearing-free, zero-allocation handoff of POD structs
 * between an engine/tick producer thread and an ESP/render consumer thread.
 */
template<typename T>
class LockFreeSnapshot {
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");

public:
    void Write(const T& value) noexcept {
        const uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1ULL, std::memory_order_release); // Odd: writer active
        std::atomic_thread_fence(std::memory_order_acq_rel);
        data_ = value;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2ULL, std::memory_order_release); // Even: stable snapshot
    }

    bool Read(T& out) const noexcept {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 == 0ULL || (s1 & 1ULL) != 0ULL) {
                if (s1 == 0ULL) return false;
                continue;
            }
            const T copy = data_;
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint64_t s2 = seq_.load(std::memory_order_acquire);
            if (s1 == s2) {
                out = copy;
                return true;
            }
        }
        return false;
    }

private:
    std::atomic<uint64_t> seq_{0};
    T data_{};
};

/**
 * In-process Engine Integration Bridge (InternalEngineBridge).
 *
 * Allows a host engine / callback (e.g., GetPlayerViewPoint, SceneView, or
 * OnCharacterSpawn in an in-process build) to push live camera, UWorld,
 * LocalPawn, and UClass* values directly into UE4Context without requiring
 * offset traversal. When a pushed value is absent or stale, UE4Context
 * automatically falls back to config-driven offset reading.
 */
class InternalEngineBridge {
public:
    /**
     * Zero-allocation callback for resolving decrypted/getter-backed ULevel actor arrays
     * (e.g. native UE4 `GetActorArray(ULevel*)` returning `TArray<AActor*>*`).
     * Must return true only when outActorsData is a valid pointer and outActorsCount > 0.
     */
    using ActorsArrayResolverFn = bool (*)(
        uint64_t levelPtr,
        uint64_t& outActorsData,
        int32_t& outActorsCount,
        void* userData) noexcept;

    struct ActorsResolverSlot {
        ActorsArrayResolverFn fn = nullptr;
        void* userData = nullptr;
    };

    struct CameraPovPacket {
        FMinimalViewInfo pov{};
        int64_t timestampMs = 0;
        bool active = false;
    };

    struct ViewMatrixPacket {
        std::array<float, 16> matrix{};
        MatrixLayout layout = MatrixLayout::ColumnMajor;
        int64_t timestampMs = 0;
        bool active = false;
    };

    /**
     * Configure freshness threshold in milliseconds (0 disables expiration).
     */
    void SetMaxStaleMs(int64_t maxStaleMs) noexcept {
        maxStaleMs_.store(maxStaleMs, std::memory_order_relaxed);
    }

    int64_t GetMaxStaleMs() const noexcept {
        return maxStaleMs_.load(std::memory_order_relaxed);
    }

    // --- Producer methods (safe to call from Engine / Game thread) ---

    void PushCameraPOV(const FMinimalViewInfo& pov) noexcept {
        CameraPovPacket pkt;
        pkt.pov = pov;
        pkt.timestampMs = NowMs();
        pkt.active = true;
        cameraPov_.Write(pkt);
    }

    void ClearCameraPOV() noexcept {
        CameraPovPacket empty;
        empty.active = false;
        cameraPov_.Write(empty);
    }

    void PushViewMatrix(
        const std::array<float, 16>& matrix,
        MatrixLayout layout = MatrixLayout::ColumnMajor) noexcept
    {
        ViewMatrixPacket pkt;
        pkt.matrix = matrix;
        pkt.layout = layout;
        pkt.timestampMs = NowMs();
        pkt.active = true;
        viewMatrix_.Write(pkt);
    }

    void ClearViewMatrix() noexcept {
        ViewMatrixPacket empty;
        empty.active = false;
        viewMatrix_.Write(empty);
    }

    void PushUWorld(uint64_t uWorldPtr) noexcept {
        uWorldOverride_.store(MemoryUtils::StripTag(uWorldPtr), std::memory_order_release);
    }

    void PushNamePool(uint64_t namePoolPtr) noexcept {
        namePoolOverride_.store(MemoryUtils::StripTag(namePoolPtr), std::memory_order_release);
    }

    void PushLocalController(uint64_t controllerPtr) noexcept {
        localControllerOverride_.store(MemoryUtils::StripTag(controllerPtr), std::memory_order_release);
    }

    void PushLocalPawn(uint64_t pawnPtr) noexcept {
        localPawnOverride_.store(MemoryUtils::StripTag(pawnPtr), std::memory_order_release);
    }

    void RegisterCharacterClass(uint64_t uClassPtr) noexcept {
        knownCharacterClass_.store(MemoryUtils::StripTag(uClassPtr), std::memory_order_release);
    }

    void SetActorsArrayResolver(ActorsArrayResolverFn fn, void* userData = nullptr) noexcept {
        ActorsResolverSlot slot;
        slot.fn = fn;
        slot.userData = userData;
        actorsResolver_.Write(slot);
        hasActorsResolver_.store(fn != nullptr, std::memory_order_release);
    }

    void ClearActorsArrayResolver() noexcept {
        SetActorsArrayResolver(nullptr, nullptr);
    }

    void ClearAll() noexcept {
        ClearCameraPOV();
        ClearViewMatrix();
        ClearActorsArrayResolver();
        uWorldOverride_.store(0, std::memory_order_release);
        namePoolOverride_.store(0, std::memory_order_release);
        localControllerOverride_.store(0, std::memory_order_release);
        localPawnOverride_.store(0, std::memory_order_release);
        knownCharacterClass_.store(0, std::memory_order_release);
    }

    // --- Consumer methods (called by UE4Context::BeginFrame / UpdateCamera / Providers) ---

    bool TryReadCameraPOV(FMinimalViewInfo& outPov) const noexcept {
        CameraPovPacket pkt;
        if (!cameraPov_.Read(pkt) || !pkt.active) {
            return false;
        }
        if (IsStale(pkt.timestampMs)) {
            return false;
        }
        outPov = pkt.pov;
        return true;
    }

    bool TryReadViewMatrix(std::array<float, 16>& outMat, MatrixLayout& outLayout) const noexcept {
        ViewMatrixPacket pkt;
        if (!viewMatrix_.Read(pkt) || !pkt.active) {
            return false;
        }
        if (IsStale(pkt.timestampMs)) {
            return false;
        }
        outMat = pkt.matrix;
        outLayout = pkt.layout;
        return true;
    }

    uint64_t GetUWorldOverride() const noexcept {
        return uWorldOverride_.load(std::memory_order_acquire);
    }

    uint64_t GetNamePoolOverride() const noexcept {
        return namePoolOverride_.load(std::memory_order_acquire);
    }

    uint64_t GetLocalControllerOverride() const noexcept {
        return localControllerOverride_.load(std::memory_order_acquire);
    }

    uint64_t GetLocalPawnOverride() const noexcept {
        return localPawnOverride_.load(std::memory_order_acquire);
    }

    uint64_t GetKnownCharacterClass() const noexcept {
        return knownCharacterClass_.load(std::memory_order_acquire);
    }

    bool HasActorsArrayResolver() const noexcept {
        return hasActorsResolver_.load(std::memory_order_acquire);
    }

    bool TryResolveLevelActors(
        uint64_t levelPtr,
        uint64_t& outActorsData,
        int32_t& outActorsCount) const noexcept
    {
        if (!hasActorsResolver_.load(std::memory_order_acquire)) {
            return false;
        }
        ActorsResolverSlot slot;
        if (!actorsResolver_.Read(slot) || slot.fn == nullptr) {
            return false;
        }
        return slot.fn(levelPtr, outActorsData, outActorsCount, slot.userData);
    }

private:
    LockFreeSnapshot<CameraPovPacket> cameraPov_;
    LockFreeSnapshot<ViewMatrixPacket> viewMatrix_;
    LockFreeSnapshot<ActorsResolverSlot> actorsResolver_;

    std::atomic<uint64_t> uWorldOverride_{0};
    std::atomic<uint64_t> namePoolOverride_{0};
    std::atomic<uint64_t> localControllerOverride_{0};
    std::atomic<uint64_t> localPawnOverride_{0};
    std::atomic<uint64_t> knownCharacterClass_{0};
    std::atomic<bool> hasActorsResolver_{false};
    std::atomic<int64_t> maxStaleMs_{500};

    static int64_t NowMs() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool IsStale(int64_t timestampMs) const noexcept {
        const int64_t limit = maxStaleMs_.load(std::memory_order_relaxed);
        if (limit <= 0) {
            return false;
        }
        return (NowMs() - timestampMs) > limit;
    }
};
