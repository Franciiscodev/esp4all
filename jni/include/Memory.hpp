#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

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

// Memory access backend selection
enum class MemoryAccessMode {
    PROCESS_VM_READV,  // Default for rooted Android: SYS_process_vm_readv
    PROC_MEM,          // Fallback: pread64 on /proc/<pid>/mem
    KERNEL_DRIVER      // Optional: custom ditPro kernel syscall
};

inline const char* MemoryAccessModeToString(MemoryAccessMode mode) noexcept {
    switch (mode) {
        case MemoryAccessMode::PROCESS_VM_READV: return "PROCESS_VM_READV";
        case MemoryAccessMode::PROC_MEM:         return "PROC_MEM (pread64)";
        case MemoryAccessMode::KERNEL_DRIVER:    return "KERNEL_DRIVER (ditPro)";
        default:                                 return "UNKNOWN";
    }
}

/**
 * Userspace root memory reader using SYS_process_vm_readv (no file descriptor needed).
 */
class ProcessVmMemoryReader : public IMemoryReader {
private:
    uint32_t pid_ = 0;
    uint64_t moduleBase_ = 0;

public:
    ProcessVmMemoryReader() = default;
    ProcessVmMemoryReader(uint32_t pid, uint64_t base = 0)
        : pid_(pid), moduleBase_(base) {}

    void SetPid(uint32_t pid) noexcept { pid_ = pid; }
    void SetModuleBase(uint64_t base) noexcept { moduleBase_ = base; }

    uint32_t GetPid() const override { return pid_; }
    uint64_t GetModuleBase() const override { return moduleBase_; }

    bool ReadBuffer(uint64_t address, void* buffer, size_t size) override {
        if (pid_ == 0 || buffer == nullptr || size == 0) {
            return false;
        }
        uint64_t cleanAddr = MemoryUtils::StripTag(address);
        if (!MemoryUtils::IsValidPtr(cleanAddr, 1)) {
            return false;
        }

#if (defined(__linux__) || defined(__ANDROID__)) && defined(SYS_process_vm_readv)
        struct iovec local_iov{};
        local_iov.iov_base = buffer;
        local_iov.iov_len = size;

        struct iovec remote_iov{};
        remote_iov.iov_base = reinterpret_cast<void*>(static_cast<uintptr_t>(cleanAddr));
        remote_iov.iov_len = size;

        ssize_t bytesRead = syscall(
            SYS_process_vm_readv,
            static_cast<pid_t>(pid_),
            &local_iov, 1UL,
            &remote_iov, 1UL,
            0UL);
        return bytesRead == static_cast<ssize_t>(size);
#else
        (void)cleanAddr;
        return false;
#endif
    }
};

/**
 * Standard Linux /proc/<pid>/mem memory reader using atomic pread64 (thread-safe).
 */
class ProcMemReader : public IMemoryReader {
private:
    uint32_t pid_ = 0;
    uint64_t moduleBase_ = 0;
    int memfd_ = -1;
    uint32_t openedPid_ = 0;
    uint32_t lastFailedPid_ = 0;

public:
    ProcMemReader() = default;
    ProcMemReader(uint32_t pid, uint64_t base = 0)
        : pid_(pid), moduleBase_(base) {}

    ~ProcMemReader() override {
        Close();
    }

    ProcMemReader(const ProcMemReader&) = delete;
    ProcMemReader& operator=(const ProcMemReader&) = delete;

    void Close() noexcept {
        if (memfd_ >= 0) {
            close(memfd_);
            memfd_ = -1;
            openedPid_ = 0;
        }
    }

    void SetPid(uint32_t pid) noexcept {
        if (pid_ != pid) {
            Close();
            pid_ = pid;
            lastFailedPid_ = 0;
        }
    }

    void SetModuleBase(uint64_t base) noexcept { moduleBase_ = base; }

    uint32_t GetPid() const override { return pid_; }
    uint64_t GetModuleBase() const override { return moduleBase_; }

    bool ReadBuffer(uint64_t address, void* buffer, size_t size) override {
        if (pid_ == 0 || buffer == nullptr || size == 0) {
            return false;
        }
        uint64_t cleanAddr = MemoryUtils::StripTag(address);
        if (!MemoryUtils::IsValidPtr(cleanAddr, 1)) {
            return false;
        }

        if (memfd_ < 0 || openedPid_ != pid_) {
            Close();
            std::string memPath = "/proc/" + std::to_string(pid_) + "/mem";
            memfd_ = open(memPath.c_str(), O_RDONLY | O_CLOEXEC);
            if (memfd_ < 0) {
                if (lastFailedPid_ != pid_) {
                    std::cerr << "[ProcMemReader] Failed to open " << memPath
                              << ": " << strerror(errno) << std::endl;
                    lastFailedPid_ = pid_;
                }
                return false;
            }
            openedPid_ = pid_;
        }

#if defined(__linux__) || defined(__ANDROID__)
        ssize_t bytesRead = pread64(memfd_, buffer, size, static_cast<off64_t>(cleanAddr));
#else
        ssize_t bytesRead = pread(memfd_, buffer, size, static_cast<off_t>(cleanAddr));
#endif
        return bytesRead == static_cast<ssize_t>(size);
    }
};

/**
 * Optional custom kernel driver reader (ditPro SYS_lookup_dcookie).
 * Validates actual syscall return value (0 on success, -1 on failure).
 */
class KernelDriverReader : public IMemoryReader {
private:
    struct DitproPacket {
        int read_write;
        int pid;
        uintptr_t addr;
        void* buffer;
        size_t size;
        int wendi;
        DitproPacket() : read_write(0x400), pid(0), addr(0), buffer(nullptr), size(0), wendi(616) {}
    };

    uint32_t pid_ = 0;
    uint64_t moduleBase_ = 0;

public:
    KernelDriverReader() = default;
    KernelDriverReader(uint32_t pid, uint64_t base = 0)
        : pid_(pid), moduleBase_(base) {}

    void SetPid(uint32_t pid) noexcept { pid_ = pid; }
    void SetModuleBase(uint64_t base) noexcept { moduleBase_ = base; }

    uint32_t GetPid() const override { return pid_; }
    uint64_t GetModuleBase() const override { return moduleBase_; }

    bool ReadBuffer(uint64_t address, void* buffer, size_t size) override {
        if (pid_ == 0 || buffer == nullptr || size == 0) {
            return false;
        }
        uint64_t cleanAddr = MemoryUtils::StripTag(address);
        if (!MemoryUtils::IsValidPtr(cleanAddr, 1)) {
            return false;
        }

#if (defined(__linux__) || defined(__ANDROID__)) && defined(SYS_lookup_dcookie)
        DitproPacket pkt;
        pkt.read_write = 0x400;
        pkt.pid = static_cast<int>(pid_);
        pkt.addr = static_cast<uintptr_t>(cleanAddr);
        pkt.buffer = buffer;
        pkt.size = size;
        pkt.wendi = 616;

        long ret = syscall(SYS_lookup_dcookie, &pkt);
        return ret == 0;
#else
        (void)cleanAddr;
        return false;
#endif
    }
};

/**
 * Unified memory facade supporting runtime switching across
 * PROCESS_VM_READV (default), PROC_MEM (pread64), and KERNEL_DRIVER.
 */
class MemoryTool : public IMemoryReader {
private:
    uint32_t pid = 0;
    uint64_t moduleBase = 0;
    MemoryAccessMode accessMode = MemoryAccessMode::PROCESS_VM_READV;

    ProcessVmMemoryReader processVmReader;
    ProcMemReader procMemReader;
    KernelDriverReader kernelDriverReader;

    void SyncBackends() noexcept {
        processVmReader.SetPid(pid);
        processVmReader.SetModuleBase(moduleBase);
        procMemReader.SetPid(pid);
        procMemReader.SetModuleBase(moduleBase);
        kernelDriverReader.SetPid(pid);
        kernelDriverReader.SetModuleBase(moduleBase);
    }

    static bool IsAllDigits(const char* s) noexcept {
        if (!s || *s == '\0') {
            return false;
        }
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
            if (!std::isdigit(*p)) {
                return false;
            }
        }
        return true;
    }

public:
    MemoryTool() {
#ifdef USE_KERNEL_DRIVER
        accessMode = MemoryAccessMode::KERNEL_DRIVER;
#elif defined(USE_PROC_MEM)
        accessMode = MemoryAccessMode::PROC_MEM;
#else
        accessMode = MemoryAccessMode::PROCESS_VM_READV;
#endif
        std::cout << "[MemoryTool] Initialized - Mode: "
                  << MemoryAccessModeToString(accessMode) << std::endl;
    }

    ~MemoryTool() override {
        procMemReader.Close();
    }

    void SetAccessMode(MemoryAccessMode mode) {
        accessMode = mode;
        std::cout << "[MemoryTool] Memory access mode changed to: "
                  << MemoryAccessModeToString(accessMode) << std::endl;
    }

    void CycleAccessMode() {
        switch (accessMode) {
            case MemoryAccessMode::PROCESS_VM_READV:
                SetAccessMode(MemoryAccessMode::PROC_MEM);
                break;
            case MemoryAccessMode::PROC_MEM:
                SetAccessMode(MemoryAccessMode::KERNEL_DRIVER);
                break;
            case MemoryAccessMode::KERNEL_DRIVER:
                SetAccessMode(MemoryAccessMode::PROCESS_VM_READV);
                break;
        }
    }

    MemoryAccessMode GetAccessMode() const noexcept {
        return accessMode;
    }

    void SetPid(uint32_t newPid) noexcept {
        pid = newPid;
        if (pid == 0) {
            moduleBase = 0;
        }
        SyncBackends();
    }

    void SetModuleBase(uint64_t base) noexcept {
        moduleBase = MemoryUtils::StripTag(base);
        SyncBackends();
    }

    bool ReadBuffer(uint64_t address, void* buffer, size_t size) override {
        switch (accessMode) {
            case MemoryAccessMode::PROCESS_VM_READV:
                return processVmReader.ReadBuffer(address, buffer, size);
            case MemoryAccessMode::PROC_MEM:
                return procMemReader.ReadBuffer(address, buffer, size);
            case MemoryAccessMode::KERNEL_DRIVER:
                return kernelDriverReader.ReadBuffer(address, buffer, size);
            default:
                return false;
        }
    }

    uint64_t GetModuleBase() const override {
        return moduleBase;
    }

    uint32_t GetPid() const override {
        return pid;
    }

    bool read(uintptr_t addr, void* buffer, size_t size) {
        return ReadBuffer(static_cast<uint64_t>(addr), buffer, size);
    }

    template<typename T>
    T read(uint64_t address) {
        return IMemoryReader::Read<T>(address);
    }

    void getName(char* buf, size_t bufSize, uint64_t namePtr) {
        ReadUtf16StringInto(namePtr, buf, bufSize, 32);
    }

    void getName(char* buf, uint64_t namePtr) {
        getName(buf, 32, namePtr);
    }

    /**
     * Locate target process PID by package name in /proc/<pid>/cmdline.
     * Never corrupts this->pid while scanning; sets pid = 0 if not found.
     */
    uint32_t GetPid(const std::string& packageName) {
        if (packageName.empty()) {
            SetPid(0);
            return 0;
        }

        DIR* dir = opendir("/proc");
        if (!dir) {
            SetPid(0);
            return 0;
        }

        uint32_t foundPid = 0;
        struct dirent* ent = nullptr;
        while ((ent = readdir(dir)) != nullptr) {
            if (!IsAllDigits(ent->d_name)) {
                continue;
            }

            uint32_t candidatePid = 0;
            try {
                candidatePid = static_cast<uint32_t>(std::stoul(ent->d_name));
            } catch (...) {
                continue;
            }
            if (candidatePid == 0) {
                continue;
            }

            std::string cmdPath = "/proc/" + std::string(ent->d_name) + "/cmdline";
            std::ifstream file(cmdPath, std::ios::in | std::ios::binary);
            if (!file.is_open()) {
                continue;
            }

            std::string cmdline;
            std::getline(file, cmdline, '\0');
            if (cmdline == packageName) {
                foundPid = candidatePid;
                break;
            }
        }

        closedir(dir);
        SetPid(foundPid);
        return foundPid;
    }

    /**
     * Resolve module base address from /proc/<pid>/maps with configurable segment filter.
     */
    uint64_t GetBaseAddress(const std::string& moduleName, const std::string& segmentFilter = "r-xp") {
        if (pid == 0 || moduleName.empty()) {
            SetModuleBase(0);
            return 0;
        }

        std::string mapsPath = "/proc/" + std::to_string(pid) + "/maps";
        std::ifstream file(mapsPath, std::ios::in);
        if (!file.is_open()) {
            SetModuleBase(0);
            return 0;
        }

        uint64_t address = MemoryUtils::ParseMapsStream(file, moduleName, segmentFilter);
        SetModuleBase(address);
        return address;
    }
};
