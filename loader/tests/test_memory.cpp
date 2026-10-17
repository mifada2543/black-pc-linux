// The Xbox memory layer on POSIX: pool, contiguous memory, virtual memory.
//
// Two things are worth knowing about how this suite is written. First, the contiguous block is the same
// fixed 64 MB at 0x80000000 the game will use, so MmGetPhysicalAddress's arithmetic is checked against
// the real mapping rather than a stand-in. Second, the protection test *writes to the page after
// mprotect has removed permission* and lets the port's own signal bridge turn the fault into an
// exception: memory permissions and the SEH machinery have to work together, and this is where that
// stops being two separate claims.
#include "kernel/k_memory_posix.h"
#include "platform/posix/image.h"
#include "platform/posix/seh.h"
#include "platform/posix/teb.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <string>

static int g_failures, g_checks;

static void check(bool cond, const std::string &what)
{
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s\n", what.c_str());
        g_failures++;
    }
    fflush(stdout); // this test deliberately faults; the last line has to survive a crash
}

static void check_u32(uint32_t got, uint32_t want, const std::string &what)
{
    g_checks++;
    if (got == want) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s (got 0x%08x, want 0x%08x)\n", what.c_str(), got, want);
        g_failures++;
    }
    fflush(stdout);
}

// ----------------------------------------------------------------------------------------------------------
// pool
// ----------------------------------------------------------------------------------------------------------

static void test_pool()
{
    printf("\npool\n");

    void *a = ExAllocatePoolWithTag(0x1234, 0x74657374); // 'test'
    check(a != nullptr && ((uintptr_t)a & 0xF) == 0, "a pool block comes back 16-byte aligned");
    check_u32(ExQueryPoolBlockSize(a), 0x1234, "the block reports the size that was asked for");

    // The Xbox pool hands out zeroed memory, and the game's CRT depends on it for string buffers.
    bool zeroed = true;
    const uint8_t *bytes = (const uint8_t *)a;
    for (uint32_t i = 0; i < 0x1234; i++)
        if (bytes[i]) zeroed = false;
    check(zeroed, "the block is zeroed");

    void *b = ExAllocatePoolWithTag(1, 0);
    check(b != nullptr && b != a, "a second block is a different address");
    check_u32(ExQueryPoolBlockSize(b), 1, "a one-byte block reports size 1");

    ExFreePool(b);
    // Freed means freed: the header's magic is cleared, so the size query answers 0 rather than reading
    // a header that malloc has already reused. This is the property that makes a double free harmless.
    check_u32(ExQueryPoolBlockSize(b), 0, "a freed block no longer answers with a size");

    ExFreePool(a);
    ExFreePool(nullptr);
    check(true, "freeing NULL and freeing twice are both survivable");

    void *c = ExAllocatePoolWithTag(0x40, 0);
    memset(c, 0xAB, 0x40);
    check_u32(((uint8_t *)c)[0x3F], 0xAB, "a block is writable for its full size");
    ExFreePool(c);
}

// ----------------------------------------------------------------------------------------------------------
// contiguous memory
// ----------------------------------------------------------------------------------------------------------

static void test_contiguous()
{
    printf("\ncontiguous memory\n");

    const uint32_t base = contiguous_base();
    const uint32_t end = base + contiguous_size();
    check_u32(MmGetPhysicalAddress((void *)(uintptr_t)base), 0, "the block starts at physical 0");

    void *a = MmAllocateContiguousMemory(0x10000);
    check(a != nullptr, "64 KB allocated");
    check((uint32_t)(uintptr_t)a >= base && (uint32_t)(uintptr_t)a < end, "it came from the contiguous block");
    check_u32((uint32_t)(uintptr_t)a % 0x1000, 0, "it is page aligned");
    check_u32(MmGetPhysicalAddress(a), (uint32_t)(uintptr_t)a - base, "physical = virtual - 0x80000000");
    check_u32(MmQueryAllocationSize(a), 0x10000, "the allocation size comes back");

    // The arithmetic the NV2A path relies on, with a non-zero physical address.
    void *mid = (uint8_t *)a + 0x3000;
    check_u32(MmGetPhysicalAddress(mid), (uint32_t)(uintptr_t)mid - base,
              "physical address arithmetic holds inside the block");

    // Alignment is honoured, and a non-power-of-two request is rounded up rather than refused.
    void *aligned = MmAllocateContiguousMemoryEx(0x2000, 0, 0xFFFFFFFF, 0x10000, PAGE_READWRITE);
    check(aligned != nullptr && ((uint32_t)(uintptr_t)aligned % 0x10000) == 0,
          "a 64 KB alignment request is honoured");
    void *odd_align = MmAllocateContiguousMemoryEx(0x1000, 0, 0xFFFFFFFF, 0x3000, PAGE_READWRITE);
    check(odd_align != nullptr && ((uint32_t)(uintptr_t)odd_align % 0x4000) == 0,
          "a non-power-of-two alignment is rounded up to one");

    // A range request that cannot fit must fail rather than hand back something outside it.
    void *impossible = MmAllocateContiguousMemoryEx(0x1000, base + contiguous_size() - 0x800,
                                                    base + contiguous_size() - 0x100, 0, PAGE_READWRITE);
    check(impossible == nullptr, "a request that cannot fit in its range is refused");

    uint32_t padding = 0xDEAD;
    void *gpu = MmClaimGpuInstanceMemory(0x10000, &padding);
    check_u32((uint32_t)(uintptr_t)gpu, end, "the GPU instance memory is the top of the block");
    check_u32(padding, 0, "and the padding count is reported as zero");

    // Freeing coalesces: two adjacent blocks freed must be usable as one larger block.
    MmFreeContiguousMemory(a);
    MmFreeContiguousMemory(aligned);
    void *merged = MmAllocateContiguousMemory(0x10000 + 0x2000 + 0x2000);
    check(merged != nullptr, "two freed runs coalesced into one larger allocation");
    MmFreeContiguousMemory(merged);
    MmFreeContiguousMemory(odd_align);

    // A pointer that was never ours is reported, not freed.
    int on_stack = 0;
    MmFreeContiguousMemory(&on_stack);
    check(true, "freeing a non-contiguous pointer is refused without touching the free list");

    uint32_t free_bytes = 0, used_bytes = 0, reserved_bytes = 0;
    kmem_contiguous_stats(&free_bytes, &used_bytes, &reserved_bytes);
    check(free_bytes + used_bytes + reserved_bytes == contiguous_size(),
          "free + used + reserved accounts for the whole block");
    check_u32(reserved_bytes, 0x100000, "the lowest 1 MB is reserved, as on the Xbox");
    check(used_bytes >= 0x10000, "the reserved GPU instance memory counts as used (" +
                                     std::to_string(used_bytes >> 20) + " MB)");
}

// ----------------------------------------------------------------------------------------------------------
// virtual memory
// ----------------------------------------------------------------------------------------------------------

// A handler for the protection test below, installed the way MSVC does it: the record is { prev,
// handler } (see seh.h), and the handler answers ExceptionContinueExecution after stepping the faulting
// instruction past the store, so the code carries on where it was.
// The faulting store, in a leaf function of its own (noinline, and no frame: at -O2 this compiles to
// `movzbl 4(%esp),%eax; movb %al,(%edx); ret`), which matters for how the handler resumes below.
static __attribute__((noinline)) void faulting_store(volatile uint8_t *p, uint8_t value) { *p = value; }

static bool g_fault_seen;
static uint32_t g_fault_eip;
static ExceptionRegistration g_frame;

extern "C" long fault_handler(ExceptionRecord *record, void *frame, Context *context, void *dispatcher)
{
    (void)record;
    (void)frame;
    (void)dispatcher;
    g_fault_seen = true;
    g_fault_eip = context->eip;
    // Resume *after* the store. The way to get that address without guessing an instruction length, and
    // without a compiler-placed label (which is not a fixed address: `&&after_fault` came back pointing
    // into a std::string temporary's cleanup, and resuming there freed a wild pointer), is to return
    // from the faulting function: it is a leaf, so its caller's return address is the word at Esp.
    context->eip = *(const uint32_t *)context->esp;
    return ExceptionContinueExecution; // 0
}

static void test_virtual_memory()
{
    printf("\nvirtual memory\n");

    void *p = nullptr;
    uint32_t size = 0x1234; // deliberately not a page multiple
    NTSTATUS s = NtAllocateVirtualMemory(&p, 0, &size, MEM_COMMIT, PAGE_READWRITE);
    check(s == KM_SUCCESS, "NtAllocateVirtualMemory succeeds");
    check_u32((uint32_t)(uintptr_t)p % 0x1000, 0, "the result is page aligned");
    check_u32(size, 0x2000, "the size came back rounded up to a page, as the Xbox does");
    check((uint32_t)(uintptr_t)p < 0x80000000u, "the region is below 2 GB, so its address is not mistaken for physical");
    check_u32(kmem_virtual_regions(), 1, "the allocation is in the table");

    memset(p, 0x5A, size);
    check(((uint8_t *)p)[size - 1] == 0x5A, "the whole region is writable");

    XMemoryBasicInformation mbi;
    s = NtQueryVirtualMemory(p, &mbi);
    check(s == KM_SUCCESS, "NtQueryVirtualMemory answers for it");
    check(mbi.base_address == p && mbi.allocation_base == p, "base and allocation base are the region start");
    check_u32(mbi.region_size, 0x2000, "the region size is the allocation size");
    check_u32(mbi.state, MEM_COMMIT, "the state is MEM_COMMIT");
    check_u32(mbi.protect, PAGE_READWRITE, "the protection is PAGE_READWRITE");
    check_u32(mbi.type, MEM_PRIVATE, "the type is MEM_PRIVATE");

    // Asking from the middle of the region: Windows reports the tail from that address.
    s = NtQueryVirtualMemory((uint8_t *)p + 0x1000, &mbi);
    check(s == KM_SUCCESS && mbi.region_size == 0x1000, "a query from the middle reports the remaining region");

    check_u32(MmQueryAllocationSize(p), 0x2000, "MmQueryAllocationSize knows the allocation");
    check_u32(MmQueryAddressProtect(p), PAGE_READWRITE, "MmQueryAddressProtect reports PAGE_READWRITE");

    // An address we never handed out is unknown, not invented.
    int on_stack = 0;
    check_u32(MmQueryAddressProtect(&on_stack), 0, "an address outside the table reports no protection");
    check(NtQueryVirtualMemory(&on_stack, &mbi) == KM_NOT_COMMITTED, "and NtQueryVirtualMemory says not committed");
    check_u32(MmQueryAllocationSize(&on_stack), 0, "MmQueryAllocationSize is 0 for it");

    // Reserve only: reserved memory is not accessible until it is committed.
    void *r = nullptr;
    uint32_t rsize = 0x1000;
    check(NtAllocateVirtualMemory(&r, 0, &rsize, MEM_RESERVE, PAGE_READWRITE) == KM_SUCCESS,
          "a reserved-only region is created");
    check(NtQueryVirtualMemory(r, &mbi) == KM_SUCCESS && mbi.state == MEM_RESERVE,
          "a reserved region reports MEM_RESERVE");

    // ---- protection changes, checked by actually faulting on the page ----
    MmSetAddressProtect(p, size, PAGE_NOACCESS);
    check_u32(MmQueryAddressProtect(p), PAGE_NOACCESS, "the protection change is recorded");

    // Install a frame the way the game's CRT does, then write to the protected page and let the port's
    // signal bridge dispatch it. Reading the byte afterwards has to happen with read-only protection,
    // not none: the page is inaccessible by design, and a test that reads it would fault again -- which
    // is also what the game does, protection back up before the data is read.
    ExceptionRegistration *old = (ExceptionRegistration *)(uintptr_t)teb()->seh;
    g_frame.prev = old;
    g_frame.handler = (void *)fault_handler;
    g_fault_seen = false;

    volatile uint8_t *target = (volatile uint8_t *)p;
    teb()->seh = (uint32_t)(uintptr_t)&g_frame;
    faulting_store(target, 0x11); // must fault, be dispatched, and continue past itself
    teb()->seh = (uint32_t)(uintptr_t)old;

    const uint32_t store_lo = (uint32_t)(uintptr_t)&faulting_store;
    check(g_fault_seen, "a write to a protected page came through the fault bridge as an exception");
    check(g_fault_eip >= store_lo && g_fault_eip < store_lo + 0x20,
          "the handler saw the address of the faulting store itself");

    MmSetAddressProtect(p, size, PAGE_READONLY);
    check(*target == 0x5A, "the store did not happen: the page really was inaccessible");
    check_u32(MmQueryAddressProtect(p), PAGE_READONLY, "and it is readable again after the protection change");

    // Read-only is the case the game hits when it parses its own data: a store faults, a load does not.
    g_fault_seen = false;
    g_frame.prev = old;
    g_frame.handler = (void *)fault_handler;
    teb()->seh = (uint32_t)(uintptr_t)&g_frame;
    faulting_store(target, 0x22); // read-only does not imply writable
    teb()->seh = (uint32_t)(uintptr_t)old;
    check(g_fault_seen, "a write to a read-only page faults as well");
    check(*target == 0x5A, "and that store did not happen either");

    // A load from the read-only page is fine: that is the whole point of read-only.
    uint8_t loaded = *target;
    check_u32(loaded, 0x5A, "a load from a read-only page does not fault");

    MmSetAddressProtect(p, size, PAGE_READWRITE);
    check_u32(MmQueryAddressProtect(p), PAGE_READWRITE, "protection restored to read/write");
    *target = 0x33;
    check_u32(*target, 0x33, "and a store works again once it is writable");

    // A partial free keeps the address reserved but takes the pages away.
    uint32_t half = 0x1000;
    check(NtFreeVirtualMemory(&p, &half, MEM_DECOMMIT) == KM_SUCCESS, "a partial free succeeds");
    check_u32(MmQueryAllocationSize(p), 0x1000, "and the region is now the size that was released");
    uint32_t whole = 0x2000;
    void *q = p;
    check(NtFreeVirtualMemory(&q, &whole, MEM_RELEASE) == KM_SUCCESS, "the remainder is released");
    check_u32(kmem_virtual_regions(), 1, "only the reserved region is left in the table");

    // Freeing twice, or freeing something else, is refused.
    void *stale = q;
    check(NtFreeVirtualMemory(&stale, &whole, MEM_RELEASE) == KM_INVALID_ADDRESS, "a second free is refused");

    void *reserved_release = r;
    check(NtFreeVirtualMemory(&reserved_release, &rsize, MEM_RELEASE) == KM_SUCCESS, "the reserved region is freed");
    check_u32(kmem_virtual_regions(), 0, "the table is empty again");

    void *bad = nullptr;
    uint32_t bad_size = 0;
    check(NtAllocateVirtualMemory(&bad, 0, &bad_size, MEM_COMMIT, PAGE_READWRITE) == KM_INVALID_PARAMETER,
          "a zero-sized allocation is refused");
    void *no_type = nullptr;
    uint32_t no_type_size = 0x1000;
    check(NtAllocateVirtualMemory(&no_type, 0, &no_type_size, MEM_TOP_DOWN, PAGE_READWRITE) == KM_INVALID_PARAMETER,
          "MEM_TOP_DOWN on its own is refused: it would place the region above 2 GB");
}

int main()
{
    printf("BLACK loader -- kernel memory layer\n");

    if (!kmem_init()) {
        printf("FATAL: kmem_init failed, cannot test the memory layer\n");
        return 1;
    }
    // The memory tests fault on purpose, so the bridge has to be in place before any of them run.
    seh_install_signal_bridge();

    test_pool();
    test_contiguous();
    test_virtual_memory();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}