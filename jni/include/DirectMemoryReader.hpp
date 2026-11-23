#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

#if defined(__linux__) || defined(__ANDROID__)
#include <sys/syscall.h>
#include <sys/uio.h>
#ifndef SYS_process_vm_readv
#if defined(__aarch64__)
#define SYS_process_vm_readv 270
#elif defined(__arm__)
#define SYS_process_vm_readv 376
#elif defined(__x86_64__)
#define SYS_process_vm_readv 310
#elif defined(__i386__)
#define SYS_process_vm_readv 347
#endif
#endif
#endif

#include "IMemoryReader.hpp"

/**
 * Memory read strategy for in-process (internal) execution.
 */
enum class DirectReadMode {
    DIRECT_MEMCPY, // Sub-microsecond direct deref via memcpy (safe inside synchronized engine tick)
    SAFE_SELF_VM   // Kernel-validated self process_vm_readv (SIGSEGV-proof on stale pointers, Linux/Android)
};

inline const char* DirectReadModeToString(DirectReadMode mode) noexcept {
    switch (mode) {
        case DirectReadMode::SAFE_SELF_VM:  return "SAFE_SELF_VM";
        case DirectReadMode::DIRECT_MEMCPY: return "DIRECT_MEMCPY";
    }
    return "UNKNOWN";
}

/**
 * In-process IMemoryReader implementation for internal mode.
 *
 * Allows UE4Context, EntityProviders, and EspEngine to run directly inside
 * the host process address space without changing any engine/ESP code.
 */
class DirectMemoryReader : public IMemoryReader {
private:
    uint32_t pid_ = 0;
    uint64_t moduleBase_ = 0;
    DirectReadMode mode_ = DirectReadMode::DIRECT_MEMCPY;
    std::vector<std::pair<uint64_t, uint64_t>> execRanges_;

public:
    explicit DirectMemoryReader(DirectReadMode mode = DirectReadMode::DIRECT_MEMCPY) noexcept
        : pid_(static_cast<uint32_t>(getpid())), mode_(mode) {}

    DirectMemoryReader(uint64_t moduleBase, DirectReadMode mode = DirectReadMode::DIRECT_MEMCPY) noexcept
        : pid_(static_cast<uint32_t>(getpid())),
          moduleBase_(MemoryUtils::StripTag(moduleBase)),
          mode_(mode) {}

    void SetMode(DirectReadMode mode) noexcept { mode_ = mode; }
    DirectReadMode GetMode() const noexcept { return mode_; }

    void CycleMode() noexcept {
        mode_ = (mode_ == DirectReadMode::SAFE_SELF_VM)
            ? DirectReadMode::DIRECT_MEMCPY
            : DirectReadMode::SAFE_SELF_VM;
    }

    void SetModuleBase(uint64_t base) noexcept {
        moduleBase_ = MemoryUtils::StripTag(base);
    }

    /**
     * Returns true if addr lies inside an executable segment captured by AttachSelfModule().
     * Used as a safety guard before invoking JSON-configured native getter function offsets.
     */
    bool IsExecutableAddress(uint64_t addr) const noexcept {
        const uint64_t clean = MemoryUtils::StripTag(addr);
        if ((clean & 3ULL) != 0ULL) {
            return false; // ARM64 instructions are strictly 4-byte aligned
        }
        for (const auto& r : execRanges_) {
            if (clean >= r.first && (clean + 4ULL) <= r.second) {
                return true;
            }
        }
        return false;
    }

    /**
     * Resolve a loaded library's base address inside the current process
     * by parsing /proc/self/maps with the configured segment filter ("r-xp" default),
     * and record all executable ranges of that module for safe function-pointer validation.
     */
    uint64_t AttachSelfModule(
        const std::string& moduleName,
        const std::string& segmentFilter = "r-xp")
    {
        pid_ = static_cast<uint32_t>(getpid());
        execRanges_.clear();
        if (moduleName.empty()) {
            moduleBase_ = 0;
            return 0;
        }

        std::ifstream file("/proc/self/maps", std::ios::in);
        if (!file.is_open()) {
            moduleBase_ = 0;
            return 0;
        }

        moduleBase_ = MemoryUtils::ParseMapsStream(file, moduleName, segmentFilter);

        // Second pass over /proc/self/maps to collect executable segments for this module
        file.clear();
        file.seekg(0);
        std::string line;
        while (std::getline(file, line)) {
            if (line.find(moduleName) == std::string::npos) {
                continue;
            }
            unsigned long long start = 0;
            unsigned long long end = 0;
            char perms[8] = {};
            if (std::sscanf(line.c_str(), "%llx-%llx %7s", &start, &end, perms) == 3) {
                if (end > start && std::strchr(perms, 'x') != nullptr) {
                    execRanges_.emplace_back(static_cast<uint64_t>(start), static_cast<uint64_t>(end));
                }
            }
        }
        return moduleBase_;
    }

    bool ReadBuffer(uint64_t address, void* buffer, size_t size) override {
        if (buffer == nullptr || size == 0) {
            return false;
        }

        const uint64_t cleanAddr = MemoryUtils::StripTag(address);
        if (!MemoryUtils::IsValidPtr(cleanAddr, 1)) {
            return false;
        }

        if (mode_ == DirectReadMode::SAFE_SELF_VM) {
#if (defined(__linux__) || defined(__ANDROID__)) && defined(SYS_process_vm_readv)
            struct iovec local_iov{};
            local_iov.iov_base = buffer;
            local_iov.iov_len = size;

            struct iovec remote_iov{};
            remote_iov.iov_base = reinterpret_cast<void*>(static_cast<uintptr_t>(cleanAddr));
            remote_iov.iov_len = size;

            ssize_t n = syscall(
                SYS_process_vm_readv,
                static_cast<pid_t>(pid_),
                &local_iov, 1UL,
                &remote_iov, 1UL,
                0UL);
            return n == static_cast<ssize_t>(size);
#else
            // Fallback to direct memcpy on non-Linux hosts (e.g. macOS unit tests)
            std::memcpy(buffer, reinterpret_cast<const void*>(static_cast<uintptr_t>(cleanAddr)), size);
            return true;
#endif
        }

        std::memcpy(buffer, reinterpret_cast<const void*>(static_cast<uintptr_t>(cleanAddr)), size);
        return true;
    }

    uint64_t GetModuleBase() const override { return moduleBase_; }
    uint32_t GetPid() const override { return pid_; }
};
