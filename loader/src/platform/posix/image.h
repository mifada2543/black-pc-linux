// Mapping an XBE into the address space the game expects, on Linux.
//
// What the Windows port needed and this one does not
// ---------------------------------------------------
// On Windows black.exe is a stub linked at 0x10000 with a 7.9 MB BSS, so the loader's own image owns
// the XBE's range before Windows can place anything there. Windows also re-reads the main module's
// PE header when CreateThread sets up a stack, so image.cpp had to splice a fresh PE header back over
// the XBE bytes it had just copied.
//
// Linux has neither problem: nothing reads the executable's header after execve, and one
// mmap(MAP_FIXED) at 0x10000 taken before anything else is allocated gives the same guarantee. So
// there is no stub and no header splicing here -- see .agents/plan.md section 3.1.
//
// The mapping itself is the XDK's own algorithm: map the whole image at the base, copy the headers,
// then copy each section to its virtual address, zeroing the BSS tail where raw_size < vsize.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "platform/posix/xbe.h"

struct FsPatchStats;

// The address the loader must reserve: the XBE is linked there and every one of its absolute
// addresses assumes it. mmap_min_addr defaults to 65536, exactly this value, so mapping here is legal.
constexpr uint32_t XB_IMAGE_BASE = 0x10000;

class XbeImage {
public:
    ~XbeImage() { unmap(); }
    XbeImage() = default;
    XbeImage(const XbeImage &) = delete;
    XbeImage &operator=(const XbeImage &) = delete;

    // Map the image at its own base and copy the headers. Sections are not copied yet.
    bool map(const XbePosix &x, std::string &err);

    // Copy every section to its virtual address, zero-filling the uninitialized tail. This is what
    // the XDK does for preloaded sections at image load and for the rest on XeLoadSection; doing it
    // for all of them at once is what makes the test able to execute code straight afterwards.
    bool copy_sections(std::string &err);

    // Rewrite the image's fs-relative accesses so they reach this thread's TEB through gs.
    // Returns the number of instructions rewritten; *out receives the breakdown.
    int patch_fs(FsPatchStats *out);

    uint8_t *at(uint32_t va) const { return va >= mapped_base && va < mapped_base + mapped_size ? (uint8_t *)(uintptr_t)va : nullptr; }
    bool valid() const { return mapped; }
    uint32_t base() const { return mapped_base; }
    uint32_t span() const { return mapped_size; }

    void unmap();

    // Stop tracking the mapping without munmap. The loader can leave while the game's threads are still
    // running -- a probe times out inside the game's loop -- and unmapping the image under a live thread
    // is what produced the "unhandled exception" at the end of every real-image run. The process exits
    // immediately after, and the kernel reclaims the whole address space then, so the pages do not need
    // to be given back by hand.
    void abandon();

private:
    bool mapped = false;
    uint32_t mapped_base = 0, mapped_size = 0;
    void *map_addr = nullptr;
    std::vector<XbeSectionInfo> sections;
    const std::vector<uint8_t> *file_data = nullptr; // borrowed from the XbePosix passed to map()
};

// The 64 MB block at 0x80000000 that stands in for the Xbox's contiguous physical memory, which the
// game reaches through MmAllocateContiguousMemoryEx and MmGetPhysicalAddress (k_memory.cpp).
// physical = virtual - CONTIGUOUS_BASE, which only works if the block really lands there.
bool map_contiguous_block(std::string &err);
void unmap_contiguous_block();
uint32_t contiguous_base();
uint32_t contiguous_size();