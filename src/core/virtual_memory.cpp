// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/error.h"
#include "common/logging/log.h"
#include "core/virtual_memory.h"

#ifdef _WIN64
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#if !defined(_WIN64)
enum PosixPageProtection {
    PAGE_NOACCESS = 0,
    PAGE_READONLY = PROT_READ,
    PAGE_READWRITE = PROT_READ | PROT_WRITE,
    PAGE_EXECUTE = PROT_EXEC,
    PAGE_EXECUTE_READ = PROT_EXEC | PROT_READ,
    PAGE_EXECUTE_READWRITE = PROT_EXEC | PROT_READ | PROT_WRITE
};
#endif

namespace VirtualMemory {
static u32 convertMemoryMode(MemoryMode mode) {
    switch (mode) {
    case MemoryMode::Read:
        return PAGE_READONLY;
    case MemoryMode::Write:
    case MemoryMode::ReadWrite:
        return PAGE_READWRITE;

    case MemoryMode::Execute:
        return PAGE_EXECUTE;
    case MemoryMode::ExecuteRead:
        return PAGE_EXECUTE_READ;
    case MemoryMode::ExecuteWrite:
    case MemoryMode::ExecuteReadWrite:
        return PAGE_EXECUTE_READWRITE;

    case MemoryMode::NoAccess:
        return PAGE_NOACCESS;
    default:
        return PAGE_NOACCESS;
    }
}
static MemoryMode convertMemoryMode(u32 mode) {
    switch (mode) {
    case PAGE_NOACCESS:
        return MemoryMode::NoAccess;
    case PAGE_READONLY:
        return MemoryMode::Read;
    case PAGE_READWRITE:
        return MemoryMode::ReadWrite;
    case PAGE_EXECUTE:
        return MemoryMode::Execute;
    case PAGE_EXECUTE_READ:
        return MemoryMode::ExecuteRead;
    case PAGE_EXECUTE_READWRITE:
        return MemoryMode::ExecuteReadWrite;
    default:
        return MemoryMode::NoAccess;
    }
}

u64 memory_alloc(u64 address, u64 size, MemoryMode mode) {
#ifdef _WIN64
    auto ptr = reinterpret_cast<uintptr_t>(VirtualAlloc(
        reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
        static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE), convertMemoryMode(mode)));

    if (ptr == 0) {
        auto err = static_cast<u32>(GetLastError());
        LOG_ERROR(Common_Memory, "VirtualAlloc() failed: 0x{:X}", err);
    }
#else
    auto ptr = reinterpret_cast<uintptr_t>(
        mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(address)), size,
             PROT_EXEC | PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));

    if (ptr == reinterpret_cast<uintptr_t> MAP_FAILED) {
        LOG_ERROR(Common_Memory, "mmap() failed: {}", std::strerror(errno));
    }
#endif
    return ptr;
}
bool memory_protect(u64 address, u64 size, MemoryMode mode, MemoryMode* old_mode) {
#ifdef _WIN64
    DWORD old_protect = 0;
    if (VirtualProtect(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
                       convertMemoryMode(mode), &old_protect) == 0) {
        auto err = static_cast<u32>(GetLastError());
        LOG_ERROR(Common_Memory, "VirtualProtect() failed: 0x{:X}", err);
        return false;
    }
    if (old_mode != nullptr) {
        *old_mode = convertMemoryMode(old_protect);
    }
    return true;
#else
    int ret = mprotect(reinterpret_cast<void*>(address), size, convertMemoryMode(mode));
    if (ret != 0) {
        const auto error = Common::GetLastErrorMsg();
        ASSERT(false);
    }
    return true;
#endif
}

bool memory_flush(u64 address, u64 size) {
#ifdef _WIN64
    if (::FlushInstructionCache(GetCurrentProcess(),
                                reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)),
                                size) == 0) {
        auto err = static_cast<u32>(GetLastError());
        LOG_ERROR(Common_Memory, "FlushInstructionCache() failed: 0x{:X}", err);
        return false;
    }
    return true;
#else // linux probably doesn't have something similar
    return true;
#endif
}
bool memory_patch(u64 vaddr, u64 value) {
    MemoryMode old_mode{};
    // memory_protect(vaddr, 8, MemoryMode::ReadWrite, &old_mode);

#if defined(__APPLE__) && defined(__aarch64__)
    pthread_jit_write_protect_np(false);
#endif

    auto* ptr = reinterpret_cast<uint64_t*>(vaddr);

    bool ret = (*ptr != value);

    *ptr = value;

    // memory_protect(vaddr, 8, old_mode, nullptr);

    // if mode is executable flush it so insure that cpu finds it
    if (containsExecuteMode(old_mode)) {
        memory_flush(vaddr, 8);
    }

#if defined(__APPLE__) && defined(__aarch64__)
    pthread_jit_write_protect_np(true);
#endif

    return ret;
}
static u64 AlignUp(u64 pos, u64 align) {
    return (align != 0 ? (pos + (align - 1)) & ~(align - 1) : pos);
}

u64 memory_alloc_aligned(u64 address, u64 size, MemoryMode mode, u64 alignment) {
#ifdef _WIN64
    // try allocate aligned address inside user area
    MEM_ADDRESS_REQUIREMENTS req{};
    MEM_EXTENDED_PARAMETER param{};
    req.LowestStartingAddress =
        (address == 0 ? reinterpret_cast<PVOID>(USER_MIN)
                      : reinterpret_cast<PVOID>(AlignUp(address, alignment)));
    req.HighestEndingAddress = reinterpret_cast<PVOID>(USER_MAX);
    req.Alignment = alignment;
    param.Type = MemExtendedParameterAddressRequirements;
    param.Pointer = &req;

    auto ptr = reinterpret_cast<uintptr_t>(
        VirtualAlloc2(GetCurrentProcess(), nullptr, size,
                      static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE),
                      convertMemoryMode(mode), &param, 1));

    if (ptr == 0) {
        auto err = static_cast<u32>(GetLastError());
        LOG_ERROR(Common_Memory, "VirtualAlloc2() failed: 0x{:X}", err);
    }
    return ptr;
#else
    void* hint_address = address == 0 ? reinterpret_cast<void*>(USER_MIN)
                                      : reinterpret_cast<void*>(AlignUp(address, alignment));
    void* ptr = mmap(hint_address, size, convertMemoryMode(mode), MAP_ANON | MAP_PRIVATE, -1, 0);
    ASSERT(ptr != MAP_FAILED);
    return reinterpret_cast<u64>(ptr);
#endif
}
} // namespace VirtualMemory
