// Memory: pool, contiguous ("physical") memory, virtual memory. The POSIX half of
// loader/src/kernel/k_memory.cpp, which stays the reference for the Xbox-visible behaviour.
//
// What carries over unchanged, because the game depends on it:
//
//   * the 64 MB contiguous block at 0x80000000 and the rule physical = virtual - 0x80000000, because
//     the game and the NV2A path convert addresses with that subtraction;
//   * the pool block layout (a header just below the user's block, 16-byte aligned), so a block's size
//     is still answerable after the game has forgotten it;
//   * the free-list bookkeeping, including the coalescing on free and the lowest-1 MB reserve.
//
// What cannot carry over, and what this file does instead:
//
//   * Windows' VirtualQuery has no POSIX equivalent, so this module keeps its own table of what it
//     allocated and answers NtQueryVirtualMemory / MmQueryAddressProtect / MmQueryAllocationSize from
//     that. Addresses the table does not know are reported as unknown (0), not invented.
//   * VirtualProtect becomes mprotect, with the Xbox cache bits (PAGE_NOCACHE / PAGE_WRITECOMBINE)
//     stripped exactly as the reference strips them.
//   * Protect changes are recorded per allocation, not per sub-range: the game sets protection over a
//     whole allocation, and tracking every subrange would cost a data structure that buys nothing.
#pragma once

#include <stdint.h>

// ---- Xbox kernel ABI pieces this module needs, defined locally (xfile.h does the same for its own) --

using NTSTATUS = int32_t;

#define KM_SUCCESS ((NTSTATUS)0x00000000)
#define KM_INVALID_PARAMETER ((NTSTATUS)0xC000000D) // STATUS_INVALID_PARAMETER
#define KM_NO_MEMORY ((NTSTATUS)0xC0000017)
#define KM_INVALID_ADDRESS ((NTSTATUS)0xC0000018)
#define KM_NOT_COMMITTED ((NTSTATUS)0xC0000044)

// MEMORY_BASIC_INFORMATION as MSVC-compiled code reads it: BaseAddress, AllocationBase,
// AllocationProtect, a padding word on x86, RegionSize, State, Protect, Type.
struct XMemoryBasicInformation {
    void *base_address;
    void *allocation_base;
    uint32_t allocation_protect;
    uint32_t pad;
    uint32_t region_size;
    uint32_t state;
    uint32_t protect;
    uint32_t type;
};
static_assert(sizeof(XMemoryBasicInformation) == 32, "MEMORY_BASIC_INFORMATION is 32 bytes on x86");

enum : uint32_t {
    MEM_NOACCESS = 0x01,
    MEM_READONLY = 0x02,
    MEM_READWRITE = 0x04,
    MEM_WRITECOPY = 0x08,
    MEM_EXECUTE = 0x10,
    MEM_EXECUTE_READ = 0x20,
    MEM_EXECUTE_READWRITE = 0x40,
    MEM_GUARD = 0x100,
    MEM_NOCACHE = 0x200,
    MEM_WRITECOMBINE = 0x400,
};

enum : uint32_t {
    PAGE_NOACCESS = 0x01,
    PAGE_READONLY = 0x02,
    PAGE_READWRITE = 0x04,
    PAGE_WRITECOPY = 0x08,
    PAGE_EXECUTE = 0x10,
    PAGE_EXECUTE_READ = 0x20,
    PAGE_EXECUTE_READWRITE = 0x40,
    PAGE_GUARD = 0x100,
    PAGE_NOCACHE = 0x200,
    PAGE_WRITECOMBINE = 0x400,
};

enum : uint32_t {
    MEM_COMMIT = 0x1000,
    MEM_RESERVE = 0x2000,
    MEM_DECOMMIT = 0x4000,
    MEM_RELEASE = 0x8000,
    MEM_RESET = 0x80000,
    MEM_TOP_DOWN = 0x100000,
    MEM_NOZERO = 0x800000, // Xbox-only, dropped
    MEM_FREE = 0x10000,
    MEM_PRIVATE = 0x20000,
    MEM_MAPPED = 0x40000,
    MEM_IMAGE = 0x1000000,
};

// The game calls these through the import table, so the linkage and calling convention are part of the
// ABI: C linkage, __attribute__((stdcall)), callee cleans the stack.

// ---- pool --------------------------------------------------------------------------------------------------

extern "C" void *__attribute__((stdcall)) ExAllocatePoolWithTag(uint32_t NumberOfBytes, uint32_t Tag);
extern "C" void __attribute__((stdcall)) ExFreePool(void *P);
extern "C" uint32_t __attribute__((stdcall)) ExQueryPoolBlockSize(void *PoolBlock);

// ---- contiguous memory ------------------------------------------------------------------------------------

extern "C" void *__attribute__((stdcall)) MmAllocateContiguousMemory(uint32_t NumberOfBytes);
extern "C" void *__attribute__((stdcall)) MmAllocateContiguousMemoryEx(uint32_t NumberOfBytes, uint32_t LowestAcceptableAddress,
                                                       uint32_t HighestAcceptableAddress, uint32_t Alignment,
                                                       uint32_t ProtectionType);
extern "C" void __attribute__((stdcall)) MmFreeContiguousMemory(void *BaseAddress);
extern "C" void *__attribute__((stdcall)) MmClaimGpuInstanceMemory(uint32_t NumberOfBytes, uint32_t *NumberOfPaddingBytes);
extern "C" uint32_t __attribute__((stdcall)) MmGetPhysicalAddress(void *BaseAddress);
extern "C" void __attribute__((stdcall)) MmLockUnlockBufferPages(void *BaseAddress, uint32_t NumberOfBytes, uint8_t UnlockPages);
extern "C" void __attribute__((stdcall)) MmLockUnlockPhysicalPage(uint32_t PhysicalAddress, uint8_t UnlockPage);
extern "C" void __attribute__((stdcall)) MmPersistContiguousMemory(void *BaseAddress, uint32_t NumberOfBytes, uint8_t Persist);

// ---- protect / query --------------------------------------------------------------------------------------

extern "C" uint32_t __attribute__((stdcall)) MmQueryAddressProtect(void *VirtualAddress);
extern "C" uint32_t __attribute__((stdcall)) MmQueryAllocationSize(void *BaseAddress);
extern "C" void __attribute__((stdcall)) MmSetAddressProtect(void *BaseAddress, uint32_t NumberOfBytes, uint32_t NewProtect);

// ---- virtual memory ---------------------------------------------------------------------------------------

extern "C" NTSTATUS __attribute__((stdcall)) NtAllocateVirtualMemory(void **BaseAddress, uint32_t ZeroBits, uint32_t *RegionSize,
                                                      uint32_t AllocationType, uint32_t Protect);
extern "C" NTSTATUS __attribute__((stdcall)) NtFreeVirtualMemory(void **BaseAddress, uint32_t *RegionSize, uint32_t FreeType);
extern "C" NTSTATUS __attribute__((stdcall)) NtQueryVirtualMemory(void *BaseAddress, XMemoryBasicInformation *Buffer);

// Map the contiguous block and prepare the pool. Safe to call twice. Returns false only if the
// contiguous block could not be mapped, which is the one thing the game cannot work without.
bool kmem_init();

// The contiguous block's accounting: free, in use, and the lowest megabyte that is reserved outright and
// belongs to neither. For the tests and --info.
void kmem_contiguous_stats(uint32_t *free_bytes, uint32_t *used_bytes, uint32_t *reserved_bytes);
// How many allocations kmem is tracking in its virtual-memory table.
uint32_t kmem_virtual_regions();