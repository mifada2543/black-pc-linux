#include "platform/posix/xbe.h"

#include <cstring>
#include <ctime>

namespace {

// The entry point and the thunk table address are XORed with a per-build-kind key. Picking the key
// that decodes the entry point into the image identifies the build kind without trusting a field.
constexpr uint32_t ENTRY_XOR[3] = {0xA8FC57AB, 0x94859D4B, 0x40B5C16E}; // retail, debug, sega
constexpr uint32_t THUNK_XOR[3] = {0x5B6D40B6, 0xEFB1F152, 0x2290059D};
const char *const KIND_NAMES[3] = {"retail", "debug", "sega"};

uint32_t rd32(const std::vector<uint8_t> &d, size_t off)
{
    uint32_t v = 0;
    if (off + 4 <= d.size()) memcpy(&v, &d[off], 4);
    return v;
}

uint16_t rd16(const std::vector<uint8_t> &d, size_t off)
{
    uint16_t v = 0;
    if (off + 2 <= d.size()) memcpy(&v, &d[off], 2);
    return v;
}

} // namespace

bool XbePosix::load(const std::string &path, std::string &err)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        err = "cannot seek " + path;
        return false;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        err = "cannot size " + path;
        return false;
    }
    rewind(f);
    std::vector<uint8_t> buf((size_t)n);
    size_t got = n > 0 ? fread(buf.data(), 1, (size_t)n, f) : 0;
    fclose(f);
    if (got != (size_t)n) {
        err = "short read on " + path;
        return false;
    }
    return load_from_memory(std::move(buf), err);
}

bool XbePosix::load_from_memory(std::vector<uint8_t> bytes, std::string &err)
{
    data = std::move(bytes);
    if (data.size() < 0x180 || memcmp(data.data(), "XBEH", 4) != 0) {
        err = "not an XBE (missing XBEH magic)";
        return false;
    }

    base = rd32(data, 0x104);
    size_of_headers = rd32(data, 0x108);
    size_of_image = rd32(data, 0x10C);
    timedate = rd32(data, 0x114);
    cert_addr = rd32(data, 0x118);
    num_sections = rd32(data, 0x11C);
    sect_addr = rd32(data, 0x120);
    init_flags = rd32(data, 0x124);
    uint32_t raw_entry = rd32(data, 0x128);
    tls_addr = rd32(data, 0x12C);
    stack_commit = rd32(data, 0x130);
    heap_reserve = rd32(data, 0x134);
    heap_commit = rd32(data, 0x138);
    pe_base = rd32(data, 0x13C);
    pe_size = rd32(data, 0x140);
    debug_path_addr = rd32(data, 0x14C);
    uint32_t raw_thunk = rd32(data, 0x158);
    libver_count = rd32(data, 0x160);
    libver_addr = rd32(data, 0x164);

    if (size_of_headers < 0x180 || base == 0 || size_of_image == 0) {
        err = "implausible image header";
        return false;
    }

    // Entry point and thunk address are XORed with a build-kind key: pick the key that lands the
    // decoded entry point inside the image.
    kind.clear();
    for (int k = 0; k < 3; k++) {
        uint32_t e = raw_entry ^ ENTRY_XOR[k];
        if (e >= base && e < base + size_of_image) {
            kind = KIND_NAMES[k];
            entry = e;
            uint32_t t = raw_thunk ^ THUNK_XOR[k];
            thunk_addr = t;
            break;
        }
    }
    if (kind.empty()) {
        err = "entry point does not decode into the image with any known key";
        return false;
    }

    sections.clear();
    if (sect_addr < base || (sect_addr - base) + (size_t)num_sections * sizeof(XbeSectionHeader) > data.size()) {
        err = "section table out of range";
        return false;
    }
    for (uint32_t i = 0; i < num_sections; i++) {
        size_t o = (sect_addr - base) + (size_t)i * sizeof(XbeSectionHeader);
        XbeSectionHeader sh;
        memcpy(&sh, &data[o], sizeof sh);
        XbeSectionInfo s;
        s.index = i;
        s.flags = sh.flags;
        s.vaddr = sh.virtual_addr;
        s.vsize = sh.virtual_size;
        s.raw_addr = sh.raw_addr;
        s.raw_size = sh.raw_size;
        s.header_va = sect_addr + i * (uint32_t)sizeof(XbeSectionHeader);
        if (sh.name_addr >= base) {
            size_t no = sh.name_addr - base;
            while (no < data.size() && data[no] && s.name.size() < 64) s.name += (char)data[no++];
        }
        // A section whose raw data falls outside the file is corrupt; refuse rather than map garbage.
        if (s.raw_size && (size_t)s.raw_addr + s.raw_size > data.size()) {
            err = "section " + std::to_string(i) + " raw data outside the file";
            return false;
        }
        sections.push_back(s);
    }

    if (cert_addr >= base && (size_t)(cert_addr - base) + 0xB0 <= data.size()) {
        size_t co = cert_addr - base;
        cert_timedate = rd32(data, co + 4);
        title_id = rd32(data, co + 8);
        // The title is UTF-16LE in the file; UTF-8 is what the rest of the port uses, and the loader
        // logs it. A lone surrogate or a stray non-character becomes '?' rather than invalid UTF-8.
        std::string t;
        for (int i = 0; i < 40; i++) {
            uint16_t c = rd16(data, co + 0x0C + i * 2);
            if (!c) break;
            if (c < 0x80) {
                t += (char)c;
            } else if (c < 0x800) {
                t += (char)(0xC0 | (c >> 6));
                t += (char)(0x80 | (c & 0x3F));
            } else {
                t += '?';
            }
        }
        title = t;
        allowed_media = rd32(data, co + 0x9C);
        region = rd32(data, co + 0xA0);
        ratings = rd32(data, co + 0xA4);
        disk_number = rd32(data, co + 0xA8);
        version = rd32(data, co + 0xAC);
    }

    has_tls = false;
    if (tls_addr) {
        if (const uint8_t *p = at_va(tls_addr, sizeof(XbeTlsInfo))) {
            memcpy(&tls, p, sizeof tls);
            has_tls = true;
        }
    }

    libraries.clear();
    if (libver_count && libver_count < 256) {
        if (const uint8_t *p = at_va(libver_addr, libver_count * 16)) {
            for (uint32_t i = 0; i < libver_count; i++) {
                XbeLibraryInfo l;
                const uint8_t *q = p + (size_t)i * 16;
                size_t n = 0;
                while (n < 8 && q[n]) n++;
                l.name.assign((const char *)q, n);
                memcpy(&l.major, q + 8, 2);
                memcpy(&l.minor, q + 10, 2);
                memcpy(&l.build, q + 12, 2);
                memcpy(&l.flags, q + 14, 2);
                libraries.push_back(l);
            }
        }
    }
    return true;
}

const uint8_t *XbePosix::at_va(uint32_t va, uint32_t len) const
{
    if (va >= base && (uint64_t)va + len <= (uint64_t)base + size_of_headers && (va - base) + len <= data.size())
        return &data[va - base];
    for (const auto &s : sections) {
        if (va >= s.vaddr && (uint64_t)va + len <= (uint64_t)s.vaddr + s.raw_size) {
            size_t o = (size_t)s.raw_addr + (va - s.vaddr);
            if (o + len <= data.size()) return &data[o];
        }
    }
    return nullptr;
}

uint32_t XbePosix::u32_at_va(uint32_t va) const
{
    uint32_t v = 0;
    if (const uint8_t *p = at_va(va, 4)) memcpy(&v, p, 4);
    return v;
}

std::string XbePosix::debug_path() const
{
    std::string s;
    if (!debug_path_addr || debug_path_addr < base) return s;
    size_t o = debug_path_addr - base;
    while (o < data.size() && o < size_of_headers && data[o] && s.size() < 260) s += (char)data[o++];
    return s;
}

std::vector<uint32_t> XbePosix::kernel_imports() const
{
    std::vector<uint32_t> ords;
    // The thunk table is a run of 32-bit pointers terminated by a zero entry. Bound the walk so a
    // truncated table cannot spin: the image is at most a few tens of MB.
    const uint32_t max_entries = (size_of_image / 4) + 1;
    for (uint32_t va = thunk_addr; ords.size() < max_entries; va += 4) {
        const uint8_t *p = at_va(va, 4);
        if (!p) break;
        uint32_t v;
        memcpy(&v, p, 4);
        if (!v) break;
        ords.push_back(v & 0x7FFFFFFF); // the top bit is a flag, not part of the ordinal
    }
    return ords;
}

const XbeSectionInfo *XbePosix::section_containing(uint32_t va) const
{
    for (const auto &s : sections) {
        uint32_t span = s.vsize > s.raw_size ? s.vsize : s.raw_size;
        if (va >= s.vaddr && va < s.vaddr + span) return &s;
    }
    return nullptr;
}

const XbeSectionInfo *XbePosix::section_named(const std::string &name) const
{
    for (const auto &s : sections)
        if (s.name == name) return &s;
    return nullptr;
}

bool XbePosix::is_code_section(const XbeSectionInfo &s) const
{
    if (!(s.flags & XBE_SEC_EXECUTABLE)) return false;
    // Same exclusions as xthread.cpp's is_code_section: .rdata is executable but holds data, and
    // $$* sections are the kernel's private scratch.
    if (s.name == ".rdata" || s.name == ".data" || s.name == ".XTLID" || s.name.rfind("$$", 0) == 0) return false;
    return true;
}