#include "kernel/k_memory_posix.h"

#include "platform/posix/image.h" // map_contiguous_block / contiguous_base
#include "platform/posix/log.h"

#include <sys/mman.h>

#include <map>
#include <mutex>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr uint32_t PAGE_SIZE = 0x1000;
constexpr uint32_t CONTIG_LOW_RESERVED = 0x00100000; // physical 0-1 MB unused (kernel area on a real Xbox)
constexpr uint32_t GPU_INSTANCE_SIZE = 0x00010000;   // top 64 KB: NV2A instance memory
constexpr uint32_t POOL_MAGIC = 0x4C4F4F50;         // 'POOL'

struct PoolHeader {
    uint32_t magic, size, tag, pad;
};

struct ContigRun {
    uint32_t size;
};

std::mutex g_contig_mutex;
uint8_t *g_contig_base;
std::map<uint32_t, uint32_t> g_contig_free; // offset -> size
std::map<uint32_t, uint32_t> g_contig_used;

struct Region {
    uint32_t size;         // live (committed) bytes: what the query calls report
    uint32_t mapping_size; // what mmap gave us: what munmap has to be given, even after a decommit
    uint32_t protect;      // the protection currently applied (updated by MmSetAddressProtect)
    uint32_t initial_protect;
    uint32_t state; // MEM_COMMIT or MEM_RESERVE
};
std::mutex g_virt_mutex;
std::map<uint32_t, Region> g_regions; // base address -> region

// Where the next anonymous allocation goes when the game does not ask for an address. Under 2 GB, and
// high enough to stay clear of the image the loader mapped at 0x10000.
uintptr_t g_next_hint = 0x10000000;

uint32_t strip_cache_bits(uint32_t protect) { return protect & ~(PAGE_NOCACHE | PAGE_WRITECOMBINE); }

// Xbox protection to POSIX. PAGE_GUARD is dropped: on Windows it turns the next access into a
// STATUS_GUARD_PAGE_VIOLATION that the game's handler sees, and pretending to have it would mean
// catching every access to the range; the ranges the game guards are ones it also reads immediately,
// and it does not branch on the result. Recorded here so the omission is a decision, not a gap.
int posix_prot(uint32_t protect)
{
    const uint32_t p = strip_cache_bits(protect);
    int out = 0;
    switch (p & 0xFF) {
    case PAGE_NOACCESS: out = PROT_NONE; break;
    case PAGE_READONLY: out = PROT_READ; break;
    case PAGE_WRITECOPY:
    case PAGE_READWRITE: out = PROT_READ | PROT_WRITE; break;
    case PAGE_EXECUTE: out = PROT_EXEC; break;
    case PAGE_EXECUTE_READ: out = PROT_EXEC | PROT_READ; break;
    case PAGE_EXECUTE_READWRITE: out = PROT_EXEC | PROT_READ | PROT_WRITE; break;
    default: out = PROT_READ | PROT_WRITE; break;
    }
    return out;
}

uint32_t round_up_page(uint32_t v) { return (v + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); }

// The region containing `addr` as an iterator (so the caller gets both the base and the region), or
// end(). The table is the whole truth about virtual memory here: Linux will not tell us what it mapped
// for the game, and nothing else does.
std::map<uint32_t, Region>::iterator find_region(uint32_t addr)
{
    auto it = g_regions.upper_bound(addr);
    if (it == g_regions.begin()) return g_regions.end();
    --it;
    if (addr >= it->first + it->second.size) return g_regions.end();
    return it;
}

} // namespace

bool kmem_init()
{
    if (g_contig_base) return true;

    std::string err;
    if (!map_contiguous_block(err)) {
        logc("memory: %s", err.c_str());
        return false;
    }
    g_contig_base = (uint8_t *)(uintptr_t)contiguous_base();

    std::lock_guard<std::mutex> lock(g_contig_mutex);
    g_contig_free.clear();
    g_contig_used.clear();
    g_contig_free[CONTIG_LOW_RESERVED] = contiguous_size() - CONTIG_LOW_RESERVED - GPU_INSTANCE_SIZE;
    g_contig_used[contiguous_size() - GPU_INSTANCE_SIZE] = GPU_INSTANCE_SIZE;
    logf("memory: contiguous RAM %08x-%08x (%u MB), lowest 1 MB and the top 64 KB reserved",
         (uint32_t)(uintptr_t)g_contig_base, (uint32_t)(uintptr_t)(g_contig_base + contiguous_size()),
         contiguous_size() >> 20);
    return true;
}

// ----------------------------------------------------------------------------------------------------------
// pool
// ----------------------------------------------------------------------------------------------------------

extern "C" void *__attribute__((stdcall)) ExAllocatePoolWithTag(uint32_t NumberOfBytes, uint32_t Tag)
{
    if (!NumberOfBytes) NumberOfBytes = 1;
    // 16-byte aligned user block with the header right before it; pad holds the raw malloc pointer.
    uint8_t *raw = (uint8_t *)malloc(NumberOfBytes + sizeof(PoolHeader) + 16);
    if (!raw) return nullptr;
    uint8_t *user = (uint8_t *)(((uintptr_t)raw + sizeof(PoolHeader) + 15) & ~(uintptr_t)15);
    PoolHeader *h = (PoolHeader *)user - 1;
    h->magic = POOL_MAGIC;
    h->size = NumberOfBytes;
    h->tag = Tag;
    h->pad = (uint32_t)(uintptr_t)raw;
    memset(user, 0, NumberOfBytes); // the pool hands out zeroed memory on the Xbox
    return user;
}

extern "C" void __attribute__((stdcall)) ExFreePool(void *P)
{
    if (!P) return;
    PoolHeader *h = (PoolHeader *)P - 1;
    if (h->magic != POOL_MAGIC) {
        // Freeing something that is not a pool block, or freeing it twice: the second free has a
        // cleared magic. Neither may read or write freed memory, which is what the Xbox kernel's
        // debug flags caught and release builds did not.
        logc("memory: ExFreePool(%p): not a pool block (already freed?)", P);
        return;
    }
    h->magic = 0;
    free((void *)(uintptr_t)h->pad);
}

extern "C" uint32_t __attribute__((stdcall)) ExQueryPoolBlockSize(void *PoolBlock)
{
    if (!PoolBlock) return 0;
    PoolHeader *h = (PoolHeader *)PoolBlock - 1;
    return h->magic == POOL_MAGIC ? h->size : 0;
}

// ----------------------------------------------------------------------------------------------------------
// contiguous memory
// ----------------------------------------------------------------------------------------------------------

extern "C" void *__attribute__((stdcall)) MmAllocateContiguousMemoryEx(uint32_t NumberOfBytes, uint32_t LowestAcceptableAddress,
                                                        uint32_t HighestAcceptableAddress, uint32_t Alignment,
                                                        uint32_t ProtectionType)
{
    if (!g_contig_base && !kmem_init()) return nullptr;

    const uint32_t size = NumberOfBytes ? round_up_page(NumberOfBytes) : PAGE_SIZE;
    uint32_t align = Alignment < PAGE_SIZE ? PAGE_SIZE : Alignment;
    if (align & (align - 1)) { // round up to a power of two, as the reference does
        uint32_t a = PAGE_SIZE;
        while (a < align) a <<= 1;
        align = a;
    }

    void *result = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_contig_mutex);
        for (auto it = g_contig_free.begin(); it != g_contig_free.end() && !result; ++it) {
            const uint32_t b = it->first, s = it->second;
            uint32_t start = (b + align - 1) & ~(align - 1);
            if (start < LowestAcceptableAddress) start = (LowestAcceptableAddress + align - 1) & ~(align - 1);
            if (start + size > b + s) continue;
            if ((uint64_t)start + size - 1 > HighestAcceptableAddress) continue;
            g_contig_free.erase(it);
            if (start > b) g_contig_free[b] = start - b;
            if (start + size < b + s) g_contig_free[start + size] = b + s - (start + size);
            g_contig_used[start] = size;
            result = g_contig_base + start;
        }
    }
    if (!result) {
        logc("memory: MmAllocateContiguousMemoryEx: no run of 0x%x bytes for 0x%x..0x%x", size,
             LowestAcceptableAddress, HighestAcceptableAddress);
        return nullptr;
    }
    // mmap gave us zero pages; the Xbox hands out zeroed memory too, so nothing to do here.
    (void)ProtectionType;
    logf("memory: contiguous %u bytes at %p (alignment 0x%x)", NumberOfBytes, result, Alignment);
    return result;
}

extern "C" void *__attribute__((stdcall)) MmAllocateContiguousMemory(uint32_t NumberOfBytes)
{
    return MmAllocateContiguousMemoryEx(NumberOfBytes, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);
}

extern "C" void __attribute__((stdcall)) MmFreeContiguousMemory(void *BaseAddress)
{
    if (!g_contig_base || !BaseAddress) return;
    const uint32_t off = (uint32_t)((uint8_t *)BaseAddress - g_contig_base);
    std::lock_guard<std::mutex> lock(g_contig_mutex);
    auto it = g_contig_used.find(off);
    if (it == g_contig_used.end()) {
        logc("memory: MmFreeContiguousMemory(%p): not a contiguous allocation", BaseAddress);
        return;
    }
    uint32_t size = it->second;
    g_contig_used.erase(it);
    // Coalesce with the neighbours, or the free list fragments until a large request cannot be met.
    auto next = g_contig_free.lower_bound(off);
    if (next != g_contig_free.end() && next->first == off + size) {
        size += next->second;
        next = g_contig_free.erase(next);
    }
    if (next != g_contig_free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == off) {
            prev->second += size;
            return;
        }
    }
    g_contig_free[off] = size;
}

extern "C" void *__attribute__((stdcall)) MmClaimGpuInstanceMemory(uint32_t NumberOfBytes, uint32_t *NumberOfPaddingBytes)
{
    (void)NumberOfBytes;
    if (NumberOfPaddingBytes) *NumberOfPaddingBytes = 0;
    // The end of the instance area: the GPU driver works downwards from here, and the top 64 KB were
    // reserved at init so nothing else can take them.
    return g_contig_base + contiguous_size();
}

extern "C" uint32_t __attribute__((stdcall)) MmGetPhysicalAddress(void *BaseAddress)
{
    uint8_t *p = (uint8_t *)BaseAddress;
    if (g_contig_base && p >= g_contig_base && p <= g_contig_base + contiguous_size())
        return (uint32_t)(p - g_contig_base);
    // Not in the contiguous block: there is no physical address for it, and the game only asks about
    // memory it obtained from MmAllocateContiguousMemory. Masking keeps the answer in the shape the
    // caller expects instead of returning the virtual address, which on Xbox always looks like one.
    return (uint32_t)(uintptr_t)p & 0x7FFFFFFF;
}

extern "C" void __attribute__((stdcall)) MmLockUnlockBufferPages(void *BaseAddress, uint32_t NumberOfBytes, uint8_t UnlockPages)
{
    // Page locking exists on Windows to pin DMA-able memory. Here the mapping is already resident for
    // the whole process and the GPU side is Vulkan, so there is nothing to lock; saying so once is
    // better than pretending.
    static bool noted = false;
    if (!noted) {
        noted = true;
        logf("memory: MmLockUnlockBufferPages(%p, 0x%x, %u) is a no-op on Linux: pages are never unmapped "
             "and the GPU reads host memory directly",
             BaseAddress, NumberOfBytes, UnlockPages);
    }
}

extern "C" void __attribute__((stdcall)) MmLockUnlockPhysicalPage(uint32_t PhysicalAddress, uint8_t UnlockPage)
{
    static bool noted = false;
    if (!noted) {
        noted = true;
        logf("memory: MmLockUnlockPhysicalPage(%08x, %u) is a no-op on Linux, as above", PhysicalAddress, UnlockPage);
    }
}

extern "C" void __attribute__((stdcall)) MmPersistContiguousMemory(void *BaseAddress, uint32_t NumberOfBytes, uint8_t Persist)
{
    // Persisting means the memory survives a reboot. Nothing here unmaps, so it already does.
    static bool noted = false;
    if (!noted) {
        noted = true;
        logf("memory: MmPersistContiguousMemory(%p, 0x%x, %u): kept mapped, nothing to persist", BaseAddress,
             NumberOfBytes, Persist);
    }
}

// ----------------------------------------------------------------------------------------------------------
// protect / query
// ----------------------------------------------------------------------------------------------------------

extern "C" uint32_t __attribute__((stdcall)) MmQueryAddressProtect(void *VirtualAddress)
{
    std::lock_guard<std::mutex> lock(g_virt_mutex);
    auto it = find_region((uint32_t)(uintptr_t)VirtualAddress);
    return it == g_regions.end() ? 0 : it->second.protect;
}

extern "C" uint32_t __attribute__((stdcall)) MmQueryAllocationSize(void *BaseAddress)
{
    std::lock_guard<std::mutex> lock(g_virt_mutex);
    auto it = find_region((uint32_t)(uintptr_t)BaseAddress);
    if (it != g_regions.end()) return it->second.size;

    // Contiguous memory is not in the table; it has its own bookkeeping.
    uint8_t *p = (uint8_t *)BaseAddress;
    if (g_contig_base && p >= g_contig_base && p < g_contig_base + contiguous_size()) {
        std::lock_guard<std::mutex> clock(g_contig_mutex);
        auto it = g_contig_used.find((uint32_t)(p - g_contig_base));
        return it == g_contig_used.end() ? 0 : it->second;
    }
    return 0;
}

extern "C" void __attribute__((stdcall)) MmSetAddressProtect(void *BaseAddress, uint32_t NumberOfBytes, uint32_t NewProtect)
{
    if (!BaseAddress || !NumberOfBytes) return;
    void *aligned = (void *)((uintptr_t)BaseAddress & ~(uintptr_t)0xFFF);
    const size_t len = ((uintptr_t)BaseAddress - (uintptr_t)aligned) + NumberOfBytes;

    if (mprotect(aligned, len, posix_prot(NewProtect)) != 0) {
        logc("memory: MmSetAddressProtect(%p, 0x%x, 0x%x): mprotect failed", BaseAddress, NumberOfBytes, NewProtect);
        return;
    }

    std::lock_guard<std::mutex> lock(g_virt_mutex);
    auto it = find_region((uint32_t)(uintptr_t)BaseAddress);
    if (it != g_regions.end()) {
        it->second.protect = strip_cache_bits(NewProtect);
        return;
    }
    // Contiguous memory is outside the table, and mprotect already did the work there.
    uint8_t *p = (uint8_t *)BaseAddress;
    if (g_contig_base && p >= g_contig_base && p < g_contig_base + contiguous_size()) return;
    logf("memory: MmSetAddressProtect(%p, 0x%x): not an allocation of ours, protection changed anyway",
         BaseAddress, NumberOfBytes);
}

// ----------------------------------------------------------------------------------------------------------
// virtual memory
// ----------------------------------------------------------------------------------------------------------

extern "C" NTSTATUS __attribute__((stdcall)) NtAllocateVirtualMemory(void **BaseAddress, uint32_t ZeroBits, uint32_t *RegionSize,
                                                      uint32_t AllocationType, uint32_t Protect)
{
    if (!BaseAddress || !RegionSize || !*RegionSize) return KM_INVALID_PARAMETER;
    (void)ZeroBits;

    // Keep COMMIT/RESERVE/RESET. Drop MEM_TOP_DOWN (it would place the mapping above 2 GB, which Xbox
    // code reads as a physical address) and the Xbox-only MEM_NOZERO.
    const uint32_t type = AllocationType & (MEM_COMMIT | MEM_RESERVE | MEM_RESET);
    if (!type) return KM_INVALID_PARAMETER;
    const uint32_t size = round_up_page(*RegionSize);

    void *hint = *BaseAddress ? (void *)((uintptr_t)*BaseAddress & ~(uintptr_t)0xFFF) : nullptr;
    const int prot = posix_prot(Protect) | ((type & MEM_COMMIT) ? 0 : PROT_NONE);

    void *got = MAP_FAILED;
    if (hint) {
        got = mmap(hint, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    } else {
        // No address asked for: pick one below 2 GB rather than letting the kernel put it wherever it
        // likes. Linux hands out 32-bit mmap addresses around 0xf0000000, and the game's own code reads
        // an address's high bit as physical/kernel space the way it did on the Xbox -- an allocation up
        // there would be mistaken for one. The hint cursor starts at 256 MB, well clear of the image at
        // 0x10000 and of the contiguous block at 0x80000000, and MAP_FIXED_NOREPLACE means a hint that
        // collides with something already mapped (the loader's own image, say) is refused rather than
        // silently overwriting it.
        for (uint32_t addr = (uint32_t)(g_next_hint & ~(uintptr_t)0xFFFF); addr < 0x7F000000u;
             addr += 0x1000000u) {
            void *p = mmap((void *)(uintptr_t)addr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                           -1, 0);
            if (p == MAP_FAILED) continue; // taken, or not a hint we may have
            got = p;
            g_next_hint = addr + size;
            break;
        }
    }
    if (got == MAP_FAILED) {
        logf("memory: NtAllocateVirtualMemory(%p, 0x%x) failed", hint ? hint : nullptr, size);
        return KM_NO_MEMORY;
    }

    Region r;
    r.size = size;
    r.mapping_size = size;
    r.protect = strip_cache_bits(Protect);
    r.initial_protect = r.protect;
    r.state = (type & MEM_COMMIT) ? MEM_COMMIT : MEM_RESERVE;
    {
        std::lock_guard<std::mutex> lock(g_virt_mutex);
        g_regions[(uint32_t)(uintptr_t)got] = r;
    }
    *BaseAddress = got;
    *RegionSize = size;
    return KM_SUCCESS;
}

extern "C" NTSTATUS __attribute__((stdcall)) NtFreeVirtualMemory(void **BaseAddress, uint32_t *RegionSize, uint32_t FreeType)
{
    if (!BaseAddress || !*BaseAddress) return KM_INVALID_PARAMETER;
    const uint32_t base = (uint32_t)(uintptr_t)*BaseAddress;

    std::lock_guard<std::mutex> lock(g_virt_mutex);
    auto it = g_regions.find(base);
    if (it == g_regions.end()) return KM_INVALID_ADDRESS; // not ours, or already freed

    // MEM_RELEASE gives back the whole reservation -- the original mapping -- whatever is still committed
    // in it, and whatever size the caller passes. That is the case after a partial decommit: the game
    // releases with the size it allocated, not with the part that is left.
    if (FreeType == MEM_RELEASE) {
        const uint32_t mapping = it->second.mapping_size;
        munmap((void *)(uintptr_t)base, mapping);
        g_regions.erase(it);
        if (RegionSize) *RegionSize = mapping;
        return KM_SUCCESS;
    }

    const uint32_t size = RegionSize && *RegionSize ? *RegionSize : it->second.size;
    if (size > it->second.size) return KM_INVALID_PARAMETER;
    if (size == 0) return KM_INVALID_PARAMETER;

    // A partial free: give the pages back as inaccessible, which is what MEM_DECOMMIT means to the
    // caller -- the address is still reserved, and still ours, and the reservation is only the live part.
    if (mprotect((void *)(uintptr_t)base, size, PROT_NONE) != 0) return KM_NO_MEMORY;
    Region head = it->second;
    head.size = size;
    head.protect = PAGE_NOACCESS;
    head.state = MEM_RESERVE;
    it->second = head;
    if (RegionSize) *RegionSize = size;
    return KM_SUCCESS;
}

extern "C" NTSTATUS __attribute__((stdcall)) NtQueryVirtualMemory(void *BaseAddress, XMemoryBasicInformation *Buffer)
{
    if (!BaseAddress || !Buffer) return KM_INVALID_PARAMETER;
    const uint32_t addr = (uint32_t)(uintptr_t)BaseAddress;

    std::lock_guard<std::mutex> lock(g_virt_mutex);
    auto it = find_region(addr);
    if (it == g_regions.end()) return KM_NOT_COMMITTED;

    // One region per allocation, so the allocation base is the region's base and the size that remains
    // from `addr` to the end of it -- which is what VirtualQuery reports for the tail of a region.
    const uint32_t base = it->first;
    const Region &r = it->second;
    memset(Buffer, 0, sizeof *Buffer);
    Buffer->base_address = (void *)(uintptr_t)base;
    Buffer->allocation_base = (void *)(uintptr_t)base;
    Buffer->allocation_protect = r.initial_protect;
    Buffer->region_size = r.size - (addr - base);
    Buffer->state = r.state;
    Buffer->protect = r.protect;
    Buffer->type = MEM_PRIVATE;
    return KM_SUCCESS;
}

void kmem_contiguous_stats(uint32_t *free_bytes, uint32_t *used_bytes, uint32_t *reserved_bytes)
{
    std::lock_guard<std::mutex> lock(g_contig_mutex);
    uint32_t free_total = 0, used_total = 0;
    for (const auto &kv : g_contig_free) free_total += kv.second;
    for (const auto &kv : g_contig_used) used_total += kv.second;
    if (free_bytes) *free_bytes = free_total;
    if (used_bytes) *used_bytes = used_total;
    // The low megabyte is held back at init and tracked by neither list, because neither list is where
    // it came from: it is not a free run (nothing may take it) and not an allocation either.
    if (reserved_bytes) *reserved_bytes = g_contig_base ? CONTIG_LOW_RESERVED : 0;
}

uint32_t kmem_virtual_regions()
{
    std::lock_guard<std::mutex> lock(g_virt_mutex);
    return (uint32_t)g_regions.size();
}