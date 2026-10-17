#include "platform/posix/image.h"

#include "platform/posix/fs_patch.h"

#include <sys/mman.h>

#include <cstring>

namespace {

// Page-align up.
uint32_t page_up(uint32_t v) { return (v + 0xFFF) & ~0xFFFu; }

void *map_fixed(uint32_t addr, uint32_t len, int prot)
{
    void *p = mmap((void *)(uintptr_t)addr, len, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

} // namespace

bool XbeImage::map(const XbePosix &x, std::string &err)
{
    if (mapped) {
        err = "already mapped";
        return false;
    }
    if (x.base != XB_IMAGE_BASE) {
        // The whole loader assumes 0x10000: the fs patches, the library data addresses in fe/, and the
        // absolute game addresses all sit inside it. An XBE linked elsewhere would need a relocation
        // pass that does not exist yet.
        err = "XBE base is " + std::to_string(x.base) + ", expected " + std::to_string(XB_IMAGE_BASE);
        return false;
    }

    uint32_t span = page_up(x.size_of_image);
    if (!map_fixed(x.base, span, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        err = "cannot map " + std::to_string(span) + " bytes at 0x" + std::to_string(x.base) +
              " (check vm.mmap_min_addr is at most 65536)";
        return false;
    }
    map_addr = (void *)(uintptr_t)x.base;
    mapped_base = x.base;
    mapped_size = span;
    mapped = true;
    sections = x.sections;
    file_data = &x.data;

    // The headers, exactly as the XDK does: the image header is mapped at the base, so the section
    // table and everything it points at live at their image virtual addresses.
    uint32_t hdr = x.size_of_headers < x.data.size() ? x.size_of_headers : (uint32_t)x.data.size();
    if (hdr) memcpy((void *)(uintptr_t)x.base, x.data.data(), hdr);
    return true;
}

bool XbeImage::copy_sections(std::string &err)
{
    if (!mapped) {
        err = "not mapped";
        return false;
    }
    for (const XbeSectionInfo &s : sections) {
        if (s.vaddr < mapped_base || (uint64_t)s.vaddr + s.vsize > (uint64_t)mapped_base + mapped_size) {
            err = "section " + s.name + " at 0x" + std::to_string(s.vaddr) + " does not fit the image";
            return false;
        }
        uint32_t vsize = s.vsize ? s.vsize : s.raw_size;
        if (s.raw_size) {
            size_t o = (size_t)s.raw_addr;
            if (o + s.raw_size > file_data->size()) {
                err = "section " + s.name + " raw data is outside the file";
                return false;
            }
            memcpy((void *)(uintptr_t)s.vaddr, file_data->data() + o, s.raw_size);
        }
        // BSS: the initialized image is shorter than the section, and the rest must read as zero.
        if (vsize > s.raw_size) memset((void *)(uintptr_t)(s.vaddr + s.raw_size), 0, vsize - s.raw_size);
        // The XbeSectionHeader itself needs no work: it lives in the header region, which map() copied
        // wholesale, so it is already at header_va with the real name and digest.
    }
    return true;
}

int XbeImage::patch_fs(FsPatchStats *out)
{
    if (!mapped) return 0;
    FsPatchStats total;
    for (const XbeSectionInfo &s : sections) {
        if (!(s.flags & XBE_SEC_EXECUTABLE)) continue;
        if (s.name == ".rdata" || s.name == ".data" || s.name == ".XTLID" || s.name.rfind("$$", 0) == 0) continue;
        if (!s.raw_size) continue;
        FsPatchStats st = patch_fs_to_gs((uint8_t *)(uintptr_t)s.vaddr, s.raw_size);
        total.patched += st.patched;
        total.unknown += st.unknown;
        total.no_disp += st.no_disp;
        total.prefixes += st.prefixes;
    }
    if (out) *out = total;
    return (int)total.patched;
}

void XbeImage::unmap()
{
    if (!mapped) return;
    munmap(map_addr, mapped_size);
    mapped = false;
    map_addr = nullptr;
    mapped_base = mapped_size = 0;
    sections.clear();
    file_data = nullptr;
}

// ---------------------------------------------------------------------------
// contiguous physical memory
// ---------------------------------------------------------------------------

namespace {
void *g_contig;
uint32_t g_contig_size;
constexpr uint32_t CONTIGUOUS_BASE = 0x80000000;
constexpr uint32_t CONTIGUOUS_SIZE = 64 << 20;
} // namespace

uint32_t contiguous_base() { return CONTIGUOUS_BASE; }
uint32_t contiguous_size() { return CONTIGUOUS_SIZE; }

bool map_contiguous_block(std::string &err)
{
    if (g_contig) return true;
    // RWX: the game writes generated code and data here. W^X policies can refuse this, which is a
    // deployment note rather than a bug (see .agents/plan.md, risk 7).
    g_contig = map_fixed(CONTIGUOUS_BASE, CONTIGUOUS_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (!g_contig) {
        err = "cannot map the 64 MB contiguous block at 0x80000000 (check the address is free and that "
              "a W^X policy is not blocking PROT_WRITE|PROT_EXEC)";
        return false;
    }
    g_contig_size = CONTIGUOUS_SIZE;
    return true;
}

void unmap_contiguous_block()
{
    if (!g_contig) return;
    munmap(g_contig, g_contig_size);
    g_contig = nullptr;
    g_contig_size = 0;
}