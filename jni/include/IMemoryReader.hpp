#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <istream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace MemoryUtils {

// ARM64 Top-Byte Ignore (TBI / MTE) mask: strips bits [63:56]
constexpr uint64_t ARM64_TBI_MASK = 0x00FFFFFFFFFFFFFFULL;

// Canonical Android ARM64 user-space valid pointer bounds (48-bit VA)
constexpr uint64_t MIN_VALID_PTR = 0x0000000010000000ULL;
constexpr uint64_t MAX_VALID_PTR = 0x00007FFFFFFFFFFFULL;

/**
 * Strip ARM64 TBI / MTE tag byte from a 64-bit pointer.
 */
inline uint64_t StripTag(uint64_t ptr) noexcept {
    return ptr & ARM64_TBI_MASK;
}

/**
 * Validate whether an address falls within canonical ARM64 user-space bounds
 * and satisfies alignment requirements (power of two, default 4 bytes).
 */
inline bool IsValidPtr(uint64_t ptr, uint32_t alignment = 4) noexcept {
    uint64_t clean = StripTag(ptr);
    if (clean < MIN_VALID_PTR || clean > MAX_VALID_PTR) {
        return false;
    }
    if (alignment > 1 && (clean & static_cast<uint64_t>(alignment - 1)) != 0) {
        return false;
    }
    return true;
}

/**
 * Zero-allocation UTF-16LE -> UTF-8 conversion directly into a fixed char buffer.
 * Always writes a trailing '\0' if outBufSize > 0. Returns number of UTF-8 bytes written.
 */
inline size_t Utf16ToUtf8Buf(
    const uint16_t* data,
    size_t maxUnits,
    char* outBuf,
    size_t outBufSize) noexcept
{
    if (!outBuf || outBufSize == 0) {
        return 0;
    }
    outBuf[0] = '\0';
    if (!data || maxUnits == 0 || outBufSize == 1) {
        return 0;
    }

    size_t written = 0;
    const size_t limit = outBufSize - 1;

    for (size_t i = 0; i < maxUnits; ++i) {
        uint32_t cp = data[i];
        if (cp == 0) {
            break;
        }

        // Decode UTF-16 surrogate pair
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 < maxUnits) {
                uint32_t low = data[i + 1];
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000U + ((cp - 0xD800U) << 10) + (low - 0xDC00U);
                    ++i;
                } else {
                    continue;
                }
            } else {
                break;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            continue;
        }

        if (cp <= 0x7FU) {
            if (written + 1 > limit) break;
            outBuf[written++] = static_cast<char>(cp);
        } else if (cp <= 0x7FFU) {
            if (written + 2 > limit) break;
            outBuf[written++] = static_cast<char>(0xC0U | (cp >> 6));
            outBuf[written++] = static_cast<char>(0x80U | (cp & 0x3FU));
        } else if (cp <= 0xFFFFU) {
            if (written + 3 > limit) break;
            outBuf[written++] = static_cast<char>(0xE0U | (cp >> 12));
            outBuf[written++] = static_cast<char>(0x80U | ((cp >> 6) & 0x3FU));
            outBuf[written++] = static_cast<char>(0x80U | (cp & 0x3FU));
        } else if (cp <= 0x10FFFFU) {
            if (written + 4 > limit) break;
            outBuf[written++] = static_cast<char>(0xF0U | (cp >> 18));
            outBuf[written++] = static_cast<char>(0x80U | ((cp >> 12) & 0x3FU));
            outBuf[written++] = static_cast<char>(0x80U | ((cp >> 6) & 0x3FU));
            outBuf[written++] = static_cast<char>(0x80U | (cp & 0x3FU));
        }
    }

    outBuf[written] = '\0';
    return written;
}

/**
 * Convert a bounded UTF-16LE buffer to a null-terminated UTF-8 std::string.
 */
inline std::string Utf16ToUtf8(const uint16_t* data, size_t maxUnits) {
    if (!data || maxUnits == 0) {
        return "";
    }
    char stackBuf[512];
    if (maxUnits <= 128) {
        size_t len = Utf16ToUtf8Buf(data, maxUnits, stackBuf, sizeof(stackBuf));
        return std::string(stackBuf, len);
    }
    std::string out(maxUnits * 4 + 1, '\0');
    size_t len = Utf16ToUtf8Buf(data, maxUnits, &out[0], out.size());
    out.resize(len);
    return out;
}

/**
 * Copy a C string safely into a fixed char buffer with guaranteed null termination.
 */
inline void CopyStringSafe(char* dst, size_t dstSize, const char* src) noexcept {
    if (!dst || dstSize == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strnlen(src, dstSize - 1);
    std::memcpy(dst, src, len);
    dst[len] = '\0';
}

inline void CopyStringSafe(char* dst, size_t dstSize, const std::string& src) noexcept {
    if (!dst || dstSize == 0) return;
    size_t len = std::min(src.size(), dstSize - 1);
    if (len > 0) {
        std::memcpy(dst, src.data(), len);
    }
    dst[len] = '\0';
}

/**
 * Parse a /proc/<pid>/maps stream and resolve a module's base address using
 * a configurable segment filter:
 *  - "r-xp" / "r-x": lowest start address of executable segments matching moduleName (default)
 *  - "first" / "any": first mapping matching moduleName (ELF header / r--p)
 *  - "bss" / ".bss": first anonymous rw-p mapping immediately following moduleName
 *  - any explicit permission string (e.g., "r--p", "rw-p"): lowest start matching perms
 */
inline uint64_t ParseMapsStream(
    std::istream& in,
    const std::string& moduleName,
    const std::string& segmentFilter = "r-xp")
{
    if (moduleName.empty()) {
        return 0;
    }

    const bool wantFirst = (segmentFilter == "first" || segmentFilter == "any" || segmentFilter.empty());
    const bool wantRxp   = (segmentFilter == "r-xp" || segmentFilter == "r-x");
    const bool wantBss   = (segmentFilter == "bss" || segmentFilter == ".bss");

    uint64_t bestAddr = std::numeric_limits<uint64_t>::max();
    bool found = false;
    bool seenModule = false;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }

        std::istringstream iss(line);
        std::string addrRange, perms, offset, dev, inode;
        if (!(iss >> addrRange >> perms >> offset >> dev >> inode)) {
            continue;
        }

        std::string pathname;
        std::getline(iss, pathname);
        const size_t pathStart = pathname.find_first_not_of(" \t");
        if (pathStart != std::string::npos) {
            pathname = pathname.substr(pathStart);
        } else {
            pathname.clear();
        }
        if (!pathname.empty() && pathname.back() == '\r') {
            pathname.pop_back();
        }

        const size_t dashPos = addrRange.find('-');
        if (dashPos == std::string::npos || dashPos == 0) {
            continue;
        }

        uint64_t startAddr = 0;
        try {
            startAddr = std::stoull(addrRange.substr(0, dashPos), nullptr, 16);
        } catch (...) {
            continue;
        }

        const bool matchesModule = (!pathname.empty() && pathname.find(moduleName) != std::string::npos);

        if (wantBss) {
            if (matchesModule) {
                seenModule = true;
                continue;
            }
            if (seenModule) {
                const bool isAnonBss = pathname.empty() || pathname.find("bss") != std::string::npos;
                if (perms.rfind("rw-", 0) == 0 && isAnonBss) {
                    return StripTag(startAddr);
                }
                if (!pathname.empty()) {
                    seenModule = false;
                }
            }
            continue;
        }

        if (!matchesModule) {
            continue;
        }

        if (wantFirst) {
            return StripTag(startAddr);
        }

        bool permMatch = false;
        if (wantRxp) {
            permMatch = (perms.rfind("r-x", 0) == 0);
        } else {
            permMatch = (perms == segmentFilter);
        }

        if (permMatch) {
            const uint64_t cleanStart = StripTag(startAddr);
            if (!found || cleanStart < bestAddr) {
                bestAddr = cleanStart;
                found = true;
            }
        }
    }

    return found ? bestAddr : 0;
}

} // namespace MemoryUtils

/**
 * Abstract interface for memory reading operations.
 * Decouples UE4 translation layer and entity providers from the specific
 * memory access mechanism (process_vm_readv, /proc/<pid>/mem, kernel driver, mock, or internal).
 */
class IMemoryReader {
public:
    virtual ~IMemoryReader() = default;

    /**
     * Read raw bytes from target process memory.
     */
    virtual bool ReadBuffer(uint64_t address, void* buffer, size_t size) = 0;

    /**
     * Read a typed value from target process memory with explicit status.
     */
    template<typename T>
    bool ReadInto(uint64_t address, T& out) {
        static_assert(std::is_trivially_copyable<T>::value,
                      "T must be trivially copyable");
        T value{};
        if (ReadBuffer(MemoryUtils::StripTag(address), &value, sizeof(T))) {
            out = value;
            return true;
        }
        return false;
    }

    /**
     * Read a typed value from target process memory (zero-initialized on failure).
     */
    template<typename T>
    T Read(uint64_t address) {
        T value{};
        ReadInto<T>(address, value);
        return value;
    }

    /**
     * Read a 64-bit pointer and strip ARM64 TBI/MTE tag bits.
     */
    uint64_t ReadPointer(uint64_t address) {
        uint64_t raw = 0;
        if (!ReadInto<uint64_t>(address, raw)) {
            return 0;
        }
        return MemoryUtils::StripTag(raw);
    }

    /**
     * Zero-allocation bounded UTF-16LE -> UTF-8 string reader into a caller buffer.
     */
    bool ReadUtf16StringInto(
        uint64_t address,
        char* outBuf,
        size_t outBufSize,
        size_t maxChars = 32)
    {
        if (!outBuf || outBufSize == 0) return false;
        outBuf[0] = '\0';

        uint64_t clean = MemoryUtils::StripTag(address);
        if (!MemoryUtils::IsValidPtr(clean, 2) || maxChars == 0) {
            return false;
        }

        uint16_t buf16[64] = {};
        size_t toRead = std::min<size_t>(maxChars, 64);

        bool ok = ReadBuffer(clean, buf16, toRead * sizeof(uint16_t));
        while (!ok && toRead > 8) {
            toRead /= 2;
            std::memset(buf16, 0, sizeof(buf16));
            ok = ReadBuffer(clean, buf16, toRead * sizeof(uint16_t));
        }

        if (!ok) {
            return false;
        }

        size_t len = MemoryUtils::Utf16ToUtf8Buf(buf16, toRead, outBuf, outBufSize);
        return len > 0;
    }

    /**
     * Zero-allocation bounded ANSI/UTF-8 string reader into a caller buffer.
     */
    bool ReadAnsiStringInto(
        uint64_t address,
        char* outBuf,
        size_t outBufSize,
        size_t maxChars = 64)
    {
        if (!outBuf || outBufSize == 0) return false;
        outBuf[0] = '\0';

        uint64_t clean = MemoryUtils::StripTag(address);
        if (!MemoryUtils::IsValidPtr(clean, 1) || maxChars == 0) {
            return false;
        }

        char raw[128] = {};
        size_t toRead = std::min<size_t>(std::min<size_t>(maxChars, sizeof(raw) - 1), outBufSize - 1);
        if (toRead == 0) return false;

        bool ok = ReadBuffer(clean, raw, toRead);
        while (!ok && toRead > 16) {
            toRead /= 2;
            std::memset(raw, 0, sizeof(raw));
            ok = ReadBuffer(clean, raw, toRead);
        }

        if (!ok) {
            return false;
        }

        raw[toRead] = '\0';
        MemoryUtils::CopyStringSafe(outBuf, outBufSize, raw);
        return outBuf[0] != '\0';
    }

    /**
     * Convenience std::string UTF-16LE reader.
     */
    std::string ReadUtf16String(uint64_t address, size_t maxChars = 32) {
        char buf[256] = {};
        if (!ReadUtf16StringInto(address, buf, sizeof(buf), maxChars)) {
            return "";
        }
        return std::string(buf);
    }

    /**
     * Convenience std::string ANSI reader.
     */
    std::string ReadAnsiString(uint64_t address, size_t maxChars = 64) {
        char buf[128] = {};
        if (!ReadAnsiStringInto(address, buf, sizeof(buf), maxChars)) {
            return "";
        }
        return std::string(buf);
    }

    virtual uint64_t GetModuleBase() const = 0;
    virtual uint32_t GetPid() const = 0;
};
