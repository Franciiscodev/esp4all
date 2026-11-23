#pragma once
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "IMemoryReader.hpp"
#include "UE4/UE4Structures.hpp"

/**
 * Abstract interface for UE4 name resolution with built-in O(1) FNameIndex -> std::string cache.
 * Supports both GNames (pre-UE4.23 chunked array) and FNamePool (UE4.23+ block allocator).
 */
class INameResolver {
protected:
    mutable uint64_t cachedPoolBase_ = 0;
    mutable std::unordered_map<uint32_t, std::string> cache_;
    static constexpr size_t MAX_CACHE_ENTRIES = 16384;

    virtual std::string ResolveUncached(uint64_t poolBase, uint32_t nameIndex) = 0;

public:
    virtual ~INameResolver() = default;

    /**
     * Resolve an FName index returning a const reference to the cached string
     * (zero heap allocations on cache hit).
     */
    const std::string& ResolveRef(uint64_t poolBase, uint32_t nameIndex) {
        static const std::string kEmpty;
        uint64_t cleanBase = MemoryUtils::StripTag(poolBase);
        if (!MemoryUtils::IsValidPtr(cleanBase)) {
            return kEmpty;
        }

        if (cleanBase != cachedPoolBase_) {
            cache_.clear();
            cachedPoolBase_ = cleanBase;
        }

        auto it = cache_.find(nameIndex);
        if (it != cache_.end()) {
            return it->second;
        }

        std::string resolved = ResolveUncached(cleanBase, nameIndex);
        if (resolved.empty()) {
            return kEmpty;
        }

        if (cache_.size() >= MAX_CACHE_ENTRIES) {
            cache_.clear();
        }
        auto inserted = cache_.emplace(nameIndex, std::move(resolved));
        return inserted.first->second;
    }

    /**
     * Zero-allocation resolve directly into a fixed char buffer (on cache hit).
     */
    bool ResolveInto(uint64_t poolBase, uint32_t nameIndex, char* outBuf, size_t outBufSize) {
        if (!outBuf || outBufSize == 0) return false;
        const std::string& ref = ResolveRef(poolBase, nameIndex);
        if (ref.empty()) {
            outBuf[0] = '\0';
            return false;
        }
        MemoryUtils::CopyStringSafe(outBuf, outBufSize, ref);
        return true;
    }

    /**
     * Convenience std::string resolver.
     */
    std::string Resolve(uint64_t poolBase, uint32_t nameIndex) {
        return ResolveRef(poolBase, nameIndex);
    }

    void ClearCache() noexcept {
        cache_.clear();
        cachedPoolBase_ = 0;
    }

    size_t GetCacheSize() const noexcept {
        return cache_.size();
    }
};

/**
 * FNamePool resolver for UE4.23+.
 * Uses 16-bit block index + 16-bit offset (multiplied by FNameEntryAllocator::Stride = 2).
 */
class FNamePoolResolver : public INameResolver {
private:
    IMemoryReader& memory;
    FNamePoolConfig config;

protected:
    std::string ResolveUncached(uint64_t poolBase, uint32_t nameIndex) override {
        const uint32_t blockIndex  = nameIndex >> 16;
        const uint32_t blockOffset = nameIndex & 0xFFFFU;

        if (blockIndex >= UE4Constants::FNamePool_MaxBlocks) {
            return "";
        }

        const uint64_t blocksArrayPtr = poolBase + config.FNameEntryAllocator_Blocks;
        const uint64_t blockPtr = memory.ReadPointer(
            blocksArrayPtr + static_cast<uint64_t>(blockIndex) * 8ULL);

        if (!MemoryUtils::IsValidPtr(blockPtr)) {
            return "";
        }

        const uint32_t stride = (config.FNameEntryAllocator_Stride > 0)
            ? config.FNameEntryAllocator_Stride
            : UE4Constants::FNameEntry_DefaultStride;
        const uint64_t entryPtr = blockPtr + static_cast<uint64_t>(blockOffset) * static_cast<uint64_t>(stride);

        uint16_t header = 0;
        if (!memory.ReadInto<uint16_t>(entryPtr, header)) {
            return "";
        }

        const bool isWide = (header & UE4Constants::FNameEntry_WideBit) != 0;
        const uint16_t length = (header >> UE4Constants::FNameEntry_LenShift) & UE4Constants::FNameEntry_LenMask;

        if (length == 0 || length > 512) {
            return "";
        }

        const uint64_t stringPtr = entryPtr + config.FNameEntry_HeaderSize;

        if (isWide) {
            uint16_t stackWide[128] = {};
            if (length <= 128) {
                if (!memory.ReadBuffer(stringPtr, stackWide, static_cast<size_t>(length) * sizeof(uint16_t))) {
                    return "";
                }
                return MemoryUtils::Utf16ToUtf8(stackWide, length);
            }
            std::vector<uint16_t> wideStr(length, 0);
            if (!memory.ReadBuffer(stringPtr, wideStr.data(), static_cast<size_t>(length) * sizeof(uint16_t))) {
                return "";
            }
            return MemoryUtils::Utf16ToUtf8(wideStr.data(), length);
        } else {
            char stackAnsi[129] = {};
            if (length <= 128) {
                if (!memory.ReadBuffer(stringPtr, stackAnsi, length)) {
                    return "";
                }
                stackAnsi[length] = '\0';
                return std::string(stackAnsi, strnlen(stackAnsi, length));
            }
            std::vector<char> ansiStr(static_cast<size_t>(length) + 1, '\0');
            if (!memory.ReadBuffer(stringPtr, ansiStr.data(), length)) {
                return "";
            }
            ansiStr[length] = '\0';
            return std::string(ansiStr.data(), strnlen(ansiStr.data(), length));
        }
    }

public:
    FNamePoolResolver(IMemoryReader& mem, const FNamePoolConfig& cfg)
        : memory(mem), config(cfg) {}
};

/**
 * Legacy GNames (TNameEntryArray) resolver for pre-UE4.23 games.
 */
class GNamesResolver : public INameResolver {
private:
    IMemoryReader& memory;
    FNamePoolConfig config;

protected:
    std::string ResolveUncached(uint64_t gnamesBase, uint32_t nameIndex) override {
        if (config.ChunkSize == 0 || config.EntryStride == 0) {
            return "";
        }

        const uint32_t chunkIndex = nameIndex / config.ChunkSize;
        const uint32_t entryIndex = nameIndex % config.ChunkSize;

        if (chunkIndex >= 256) {
            return "";
        }

        const uint64_t chunkPtr = memory.ReadPointer(
            gnamesBase + static_cast<uint64_t>(chunkIndex) * config.EntryStride);
        if (!MemoryUtils::IsValidPtr(chunkPtr)) {
            return "";
        }

        const uint64_t entryPtr = memory.ReadPointer(
            chunkPtr + static_cast<uint64_t>(entryIndex) * config.EntryStride);
        if (!MemoryUtils::IsValidPtr(entryPtr)) {
            return "";
        }

        char name[128] = {};
        const uint64_t strAddr = entryPtr + config.NameStringOffset;

        size_t toRead = 64;
        bool ok = memory.ReadBuffer(strAddr, name, toRead);
        while (!ok && toRead > 16) {
            toRead /= 2;
            std::memset(name, 0, sizeof(name));
            ok = memory.ReadBuffer(strAddr, name, toRead);
        }

        if (!ok) {
            return "";
        }

        name[toRead] = '\0';
        return std::string(name);
    }

public:
    GNamesResolver(IMemoryReader& mem, const FNamePoolConfig& cfg)
        : memory(mem), config(cfg) {}
};

/**
 * Factory for creating the configured name resolver.
 */
class NameResolverFactory {
public:
    static std::unique_ptr<INameResolver> Create(
        IMemoryReader& memory,
        const FNamePoolConfig& config)
    {
        if (config.type == FNamePoolConfig::Type::FNamePool) {
            return std::make_unique<FNamePoolResolver>(memory, config);
        }
        return std::make_unique<GNamesResolver>(memory, config);
    }
};
