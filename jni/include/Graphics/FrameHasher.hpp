#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esp4all {

/**
 * Zero-allocation 64-bit FNV-1a streaming hasher used for ImDrawData dirty-frame fingerprinting.
 * Host-testable C++17 (zero platform or ImGui dependencies).
 */
class FrameHasher {
public:
    static constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
    static constexpr uint64_t kPrime = 1099511628211ULL;

    FrameHasher() noexcept = default;

    void MixBytes(const void* data, size_t size) noexcept {
        if (!data || size == 0) {
            return;
        }
        const auto* bytes = static_cast<const uint8_t*>(data);
        while (size >= 8) {
            uint64_t word = 0;
            std::memcpy(&word, bytes, 8);
            MixU64(word);
            bytes += 8;
            size -= 8;
        }
        while (size > 0) {
            hash_ ^= static_cast<uint64_t>(*bytes++);
            hash_ *= kPrime;
            --size;
        }
    }

    void MixU64(uint64_t value) noexcept {
        hash_ ^= value;
        hash_ *= kPrime;
    }

    void MixF32(float value) noexcept {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        MixU64(static_cast<uint64_t>(bits));
    }

    uint64_t Finish() const noexcept {
        // Never return 0 so 0 can safely serve as the "uninitialized / forced redraw" sentinel.
        return (hash_ == 0ULL) ? 1ULL : hash_;
    }

private:
    uint64_t hash_ = kOffsetBasis;
};

} // namespace esp4all
