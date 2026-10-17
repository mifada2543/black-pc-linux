// XBE parser for Linux.
//
// The field layout and the two XOR keys are the XDK's, not this project's; they come from
// tools/xbeinfo.py and are what loader/src/xbe.cpp already reads. This is the POSIX half: same
// offsets, no Windows file APIs, and no <windows.h>, so it can be built and tested on its own.
//
// The image is left as a plain std::vector. Mapping it into the address space the game expects is
// image.h's job, because that has to happen at 0x10000 and is a different concern from parsing.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

// Section header as it lies in the file (56 bytes).
#pragma pack(push, 1)
struct XbeSectionHeader {
    uint32_t flags;
    uint32_t virtual_addr;
    uint32_t virtual_size;
    uint32_t raw_addr;
    uint32_t raw_size;
    uint32_t name_addr;
    uint32_t reference_count;
    uint32_t head_shared_ref_addr;
    uint32_t tail_shared_ref_addr;
    uint8_t digest[20];
};
#pragma pack(pop)
static_assert(sizeof(XbeSectionHeader) == 56, "XBE section header is 56 bytes");

enum : uint32_t {
    XBE_SEC_WRITABLE = 0x01,
    XBE_SEC_PRELOAD = 0x02,
    XBE_SEC_EXECUTABLE = 0x04,
    XBE_SEC_INSERTED_FILE = 0x08,
    XBE_SEC_HEAD_PAGE_RO = 0x10,
    XBE_SEC_TAIL_PAGE_RO = 0x20,
};

struct XbeSectionInfo {
    uint32_t index = 0;
    std::string name;
    uint32_t flags = 0, vaddr = 0, vsize = 0, raw_addr = 0, raw_size = 0;
    uint32_t header_va = 0; // where this section's XbeSectionHeader lands in the mapped image
};

struct XbeTlsInfo {
    uint32_t data_start = 0, data_end = 0, index_addr = 0, callbacks_addr = 0, zero_fill = 0, characteristics = 0;
};

struct XbeLibraryInfo {
    std::string name;
    uint16_t major = 0, minor = 0, build = 0, flags = 0;
};

class XbePosix {
public:
    bool load(const std::string &path, std::string &err);

    // Parse an image already in memory. Used by the tests, which build a synthetic XBE.
    bool load_from_memory(std::vector<uint8_t> bytes, std::string &err);

    std::vector<uint8_t> data;
    std::string kind; // retail / debug / sega: which XOR key decoded the entry point

    uint32_t base = 0, size_of_headers = 0, size_of_image = 0, timedate = 0, cert_addr = 0;
    uint32_t num_sections = 0, sect_addr = 0, init_flags = 0, entry = 0, tls_addr = 0;
    uint32_t stack_commit = 0, heap_reserve = 0, heap_commit = 0, pe_base = 0, pe_size = 0;
    uint32_t debug_path_addr = 0, thunk_addr = 0, libver_count = 0, libver_addr = 0;

    // Certificate.
    uint32_t title_id = 0, cert_timedate = 0, allowed_media = 0, region = 0, ratings = 0, disk_number = 0, version = 0;
    std::string title; // UTF-8; the field is UTF-16 in the file

    std::vector<XbeSectionInfo> sections;
    bool has_tls = false;
    XbeTlsInfo tls{};
    std::vector<XbeLibraryInfo> libraries;

    // Pointer into the file for an image virtual address range, or nullptr.
    const uint8_t *at_va(uint32_t va, uint32_t len) const;
    uint32_t u32_at_va(uint32_t va) const;
    std::string debug_path() const;
    // Kernel import ordinals, in thunk-table order, stopping at the first zero entry.
    std::vector<uint32_t> kernel_imports() const;
    const XbeSectionInfo *section_containing(uint32_t va) const;
    const XbeSectionInfo *section_named(const std::string &name) const;
    // True when the section's code should be scanned for fs-relative accesses.
    bool is_code_section(const XbeSectionInfo &s) const;
};