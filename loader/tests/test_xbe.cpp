// XBE parsing and image mapping, driven by a synthetic XBE we build here.
//
// This is the closest thing to the Phase 1 gate that can run without the user's disc: the fixture is a
// real XBE image (correct header offsets, section table, certificate, TLS directory, library versions,
// kernel thunk table), and the code section contains x86 instructions that really read fs:[disp].
//
// The last test is the point of the whole exercise. It maps the image at 0x10000, rewrites its
// fs-relative accesses to gs:[K+disp], and then *calls* the patched code. If the loader hands control
// to game code, that code has to be able to reach the TEB, and that only works if every step of the
// chain is right.
#include "platform/posix/fs_patch.h"
#include "platform/posix/image.h"
#include "platform/posix/teb.h"
#include "platform/posix/xbe.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

static int g_failures, g_checks;
static void check(bool cond, const std::string &what)
{
    g_checks++;
    if (cond) { printf("  ok   %s\n", what.c_str()); }
    else { g_failures++; printf("  FAIL %s\n", what.c_str()); }
}

namespace {

constexpr uint32_t BASE = 0x10000;
constexpr uint32_t IMAGE_SIZE = 0x40000;
constexpr uint32_t HEADERS = 0x1000;
constexpr uint32_t ENTRY_XOR_RETAIL = 0xA8FC57AB;
constexpr uint32_t THUNK_XOR_RETAIL = 0x5B6D40B6;

// Where things live in the synthetic image.
// The certificate, TLS directory, library versions and debug path all live outside the image header
// (which ends around 0x164). In a real XBE the certificate has its own address too; putting it at
// BASE+0x100 would overwrite size_of_image.
constexpr uint32_t CERT_VA = BASE + 0x800;
constexpr uint32_t TLS_VA = BASE + 0x900;
constexpr uint32_t SECT_VA = BASE + 0x180;
constexpr uint32_t NAMES_VA = BASE + 0x300;
constexpr uint32_t LIBVER_VA = BASE + 0x960;
constexpr uint32_t DEBUGPATH_VA = BASE + 0xA00;
constexpr uint32_t TEXT_VA = BASE + 0x10000;
constexpr uint32_t RDATA_VA = BASE + 0x20000;
constexpr uint32_t DATA_VA = BASE + 0x30000;

constexpr uint32_t TEXT_RAW = 0x1000;
constexpr uint32_t RDATA_RAW = 0x2000;
constexpr uint32_t DATA_RAW = 0x3000;
constexpr uint32_t DATA_RAW_SIZE = 0x2000; // raw image; the BSS tail runs to vsize 0x8000
// The kernel thunk table has to live inside a section, or at_va() cannot reach it.
constexpr uint32_t THUNK_IN_DATA = DATA_VA + 0x1000;

// Offsets of the three test functions inside .text.
constexpr uint32_t FN_READ_PRCB = 0x00;   // mov eax, fs:[0x20] ; ret
constexpr uint32_t FN_READ_IRQL = 0x08;   // mov eax, fs:[0x24] ; ret
constexpr uint32_t FN_SET_IRQL = 0x10;    // mov dword fs:[0x24], 0x77 ; mov eax, fs:[0x24] ; ret
constexpr uint32_t FN_READ_DPC = 0x28;    // mov eax, fs:[0x58] ; ret
constexpr uint32_t FN_PUSH_SEH = 0x30;    // the SEH prologue shape (see build_text)
constexpr uint32_t FN_NESTED = 0x50;      // mov eax, fs:[fs:[0x20]] ; ret

// Writes panic rather than scribble past the buffer: a field aimed at an address outside the fixture
// is a bug in the test, and finding it by segfault later costs a lot more than checking here.
static void put32(std::vector<uint8_t> &d, size_t off, uint32_t v)
{
    if (off + 4 > d.size()) { fprintf(stderr, "FATAL: put32 at 0x%zx past the 0x%zx-byte fixture\n", off, d.size()); abort(); }
    memcpy(&d[off], &v, 4);
}
static void put16(std::vector<uint8_t> &d, size_t off, uint16_t v)
{
    if (off + 2 > d.size()) { fprintf(stderr, "FATAL: put16 at 0x%zx past the 0x%zx-byte fixture\n", off, d.size()); abort(); }
    memcpy(&d[off], &v, 2);
}
static void put_bytes(std::vector<uint8_t> &d, size_t off, const void *p, size_t n)
{
    if (off + n > d.size()) { fprintf(stderr, "FATAL: put_bytes at 0x%zx (%zu) past the 0x%zx-byte fixture\n", off, n, d.size()); abort(); }
    memcpy(&d[off], p, n);
}

// The encodings, as GNU as emits them (checked with objdump -d -M intel).
void build_text(std::vector<uint8_t> &t)
{
    t.assign(0x80, 0);
    // mov eax, dword ptr fs:[0x20] ; ret
    uint8_t a[] = {0x64, 0xA1, 0x20, 0, 0, 0, 0xC3};
    put_bytes(t, FN_READ_PRCB, a, sizeof a);
    // mov eax, dword ptr fs:[0x24] ; ret
    uint8_t b[] = {0x64, 0xA1, 0x24, 0, 0, 0, 0xC3};
    put_bytes(t, FN_READ_IRQL, b, sizeof b);
    // mov dword ptr fs:[0x24], 0x77 ; mov eax, dword ptr fs:[0x24] ; ret
    // modrm is 0x05 (mod=00, rm=101 = direct disp32). 0x25 would mean an SIB byte follows, which
    // would eat the first displacement byte -- this is why every fixture here comes from objdump.
    uint8_t c[] = {0x64, 0xC7, 0x05, 0x24, 0, 0, 0, 0x77, 0, 0, 0, 0x64, 0xA1, 0x24, 0, 0, 0, 0xC3};
    put_bytes(t, FN_SET_IRQL, c, sizeof c);
    // mov eax, dword ptr fs:[0x58] ; ret
    uint8_t d[] = {0x64, 0xA1, 0x58, 0, 0, 0, 0xC3};
    put_bytes(t, FN_READ_DPC, d, sizeof d);
    // The MSVC SEH prologue shape: push a cookie, store esp into fs:[0], read the head back, then pop
    // the cookie so the caller can still return cleanly. A bare push followed by ret would jump to the
    // cookie, which is a fixture bug rather than a loader one. Encodings from objdump.
    uint8_t e[] = {0x68, 0x34, 0x12, 0, 0, // push 0x1234
                   0x64, 0x89, 0x25, 0, 0, 0, 0, // mov dword fs:[0], esp
                   0x64, 0xA1, 0, 0, 0, 0,       // mov eax, fs:[0]
                   0x83, 0xC4, 0x04,             // add esp, 4
                   0xC3};                        // ret
    put_bytes(t, FN_PUSH_SEH, e, sizeof e);
    // mov eax, dword ptr fs:[fs:[0x20]] ; ret  (doubly prefixed)
    uint8_t f[] = {0x64, 0x64, 0xA1, 0x20, 0, 0, 0, 0xC3};
    put_bytes(t, FN_NESTED, f, sizeof f);
}

void add_section(std::vector<uint8_t> &d, size_t idx, uint32_t flags, uint32_t vaddr, uint32_t vsize, uint32_t raw_addr,
                 uint32_t raw_size, const char *name, uint32_t names_off)
{
    size_t o = (SECT_VA - BASE) + idx * 56;
    XbeSectionHeader sh{};
    sh.flags = flags;
    sh.virtual_addr = vaddr;
    sh.virtual_size = vsize;
    sh.raw_addr = raw_addr;
    sh.raw_size = raw_size;
    sh.name_addr = NAMES_VA + names_off;
    sh.reference_count = 1;
    put_bytes(d, o, &sh, sizeof sh);
    size_t no = (NAMES_VA + names_off) - BASE;
    put_bytes(d, no, name, strlen(name) + 1);
}

std::vector<uint8_t> build_xbe()
{
    std::vector<uint8_t> d(0x5000, 0);
    put_bytes(d, 0, "XBEH", 4);

    put32(d, 0x104, BASE);
    put32(d, 0x108, HEADERS);
    put32(d, 0x10C, IMAGE_SIZE);
    put32(d, 0x114, 1033924800u);      // a plausible timestamp
    put32(d, 0x118, CERT_VA);
    put32(d, 0x11C, 3);                // three sections
    put32(d, 0x120, SECT_VA);
    put32(d, 0x124, 0);
    put32(d, 0x128, (TEXT_VA + 0x60) ^ ENTRY_XOR_RETAIL);
    put32(d, 0x12C, TLS_VA);
    put32(d, 0x130, 0x2000);
    put32(d, 0x134, 0x10000);
    put32(d, 0x138, 0x10000);
    put32(d, 0x13C, BASE);
    put32(d, 0x140, IMAGE_SIZE);
    put32(d, 0x14C, DEBUGPATH_VA);    // debug path
    put32(d, 0x158, THUNK_IN_DATA ^ THUNK_XOR_RETAIL);
    put32(d, 0x160, 2);                // two libraries
    put32(d, 0x164, LIBVER_VA);

    // Certificate: a UTF-16 title, to prove the UTF-8 conversion.
    put32(d, (CERT_VA - BASE) + 4, 1033924800u);
    put32(d, (CERT_VA - BASE) + 8, 0x43484350u); // title id
    const char16_t title[] = u"BLACK TEST";
    for (int i = 0; title[i]; i++) put16(d, (CERT_VA - BASE) + 0x0C + i * 2, (uint16_t)title[i]);
    put32(d, (CERT_VA - BASE) + 0x9C, 0x03);     // allowed media
    put32(d, (CERT_VA - BASE) + 0xA0, 0x01);     // region
    put32(d, (CERT_VA - BASE) + 0xA4, 0xFF);     // ratings

    // TLS directory: 6 dwords.
    put32(d, (TLS_VA - BASE) + 0, TEXT_VA + 0x70);         // data start
    put32(d, (TLS_VA - BASE) + 4, TEXT_VA + 0x78);         // data end
    put32(d, (TLS_VA - BASE) + 8, DATA_VA + 0x10);         // index address
    put32(d, (TLS_VA - BASE) + 12, DATA_VA + 0x20);        // callbacks address
    put32(d, (TLS_VA - BASE) + 16, 0x1000);                // zero fill
    put32(d, (TLS_VA - BASE) + 20, 0x100000);              // characteristics

    put_bytes(d, DEBUGPATH_VA - BASE, "unit:\\x\\default.xbe", 22);

    // Sections. .text is executable and carries the fs-relative code; .rdata is executable but is
    // data, and must be skipped by the patcher; .data is writable with a BSS tail.
    add_section(d, 0, XBE_SEC_EXECUTABLE | XBE_SEC_PRELOAD, TEXT_VA, 0x1000, TEXT_RAW, 0x1000, ".text", 0);
    add_section(d, 1, XBE_SEC_EXECUTABLE, RDATA_VA, 0x1000, RDATA_RAW, 0x1000, ".rdata", 8);
    add_section(d, 2, XBE_SEC_WRITABLE | XBE_SEC_PRELOAD, DATA_VA, 0x8000, DATA_RAW, DATA_RAW_SIZE, ".data", 0x10);

    std::vector<uint8_t> text;
    build_text(text);
    put_bytes(d, TEXT_RAW, text.data(), text.size());

    // .rdata deliberately contains an fs-relative byte pattern that must NOT be patched.
    uint8_t bait[] = {0x64, 0xA1, 0x20, 0, 0, 0};
    put_bytes(d, RDATA_RAW, bait, sizeof bait);

    // Kernel thunk table: ordinals, then a zero terminator.
    const uint32_t ords[] = {6, 14, 51, 72, 82, 95, 99, 116, 155, 0};
    for (size_t i = 0; i < sizeof ords / 4; i++)
        put32(d, DATA_RAW + 0x1000 + i * 4, ords[i]);

    // Library versions: 16 bytes each.
    const char *libs[2] = {"xboxkrnl.exe", "xbox_d3d8l"};
    for (int i = 0; i < 2; i++) {
        size_t o = (LIBVER_VA - BASE) + (size_t)i * 16;
        put_bytes(d, o, libs[i], 8);
        put16(d, o + 8, (uint16_t)(1 + i));
        put16(d, o + 10, 0);
        put16(d, o + 12, 5592);
        put16(d, o + 14, 0);
    }
    return d;
}

} // namespace

static void test_parser()
{
    printf("\nXBE parser\n");
    XbePosix x;
    std::string err;
    bool ok = x.load_from_memory(build_xbe(), err);
    check(ok, ok ? std::string("parsed the synthetic XBE") : ("parsed the synthetic XBE -- " + err));
    if (!ok) return; // every check below dereferences what the parser did not fill in

    check(x.base == BASE && x.size_of_headers == HEADERS && x.size_of_image == IMAGE_SIZE, "image header fields");
    check(x.kind == "retail", "the entry point decoded with the retail XOR key");
    check(x.entry == TEXT_VA + 0x60, "entry point address");
    check(x.num_sections == 3, "section count");
    check(x.title_id == 0x43484350, "certificate title id");
    check(x.title == "BLACK TEST", "the UTF-16 title came back as UTF-8");
    check(x.allowed_media == 3 && x.region == 1 && x.ratings == 0xFF, "certificate media/region/ratings");
    check(x.debug_path() == "unit:\\x\\default.xbe", "debug path");
    check(x.has_tls && x.tls.zero_fill == 0x1000 && x.tls.characteristics == 0x100000, "TLS directory");
    check(x.libraries.size() == 2 && x.libraries[0].name == "xboxkrnl" && x.libraries[1].name == "xbox_d3d" &&
              x.libraries[1].major == 2 && x.libraries[1].build == 5592,
          "library version table");
    check(x.section_named(".text") != nullptr && x.section_named(".rdata") != nullptr, "sections found by name");
    check(x.section_containing(TEXT_VA + 4) != nullptr && x.section_containing(TEXT_VA + 4)->name == ".text",
          "section_containing resolves an address in .text");
    check(x.section_containing(DATA_VA + 0x4000) != nullptr, "section_containing covers the BSS tail");

    std::vector<uint32_t> imports = x.kernel_imports();
    check(imports.size() == 9, "kernel import table read to its zero terminator (9 entries)");
    check(!imports.empty() && imports[0] == 6 && imports[8] == 155, "ordinal values in thunk order");

    check(x.is_code_section(*x.section_named(".text")), ".text is code");
    check(!x.is_code_section(*x.section_named(".rdata")), ".rdata is executable but excluded from the fs scan");

    // A truncated or wrong file must be refused, not half-parsed.
    XbePosix bad;
    std::vector<uint8_t> junk(0x400, 0xCC);
    check(!bad.load_from_memory(junk, err) && !err.empty(), "a non-XBE file is refused with a reason");
    std::vector<uint8_t> noentry = build_xbe();
    put32(noentry, 0x128, 0x12345678);
    XbePosix bad2;
    check(!bad2.load_from_memory(noentry, err), "an entry point that decodes outside the image is refused");
}

static void test_map_and_execute()
{
    printf("\nmap, patch, execute\n");
    XbePosix x;
    std::string err;
    if (!x.load_from_memory(build_xbe(), err)) { check(false, "fixture parsed: " + err); return; }

    XbeImage img;
    check(img.map(x, err), "mapped the image at 0x10000" + (err.empty() ? "" : (": " + err)));
    if (!img.valid()) return;
    check(img.at(0x1234) == nullptr && img.at(TEXT_VA) != nullptr, "the mapped range is bounded to the image");

    check(img.copy_sections(err), "copied the sections" + (err.empty() ? "" : (": " + err)));
    check(memcmp(img.at(TEXT_VA), "XBEH", 0) == 0 && img.at(TEXT_VA)[0] == 0x64,
          ".text landed at its virtual address with its first fs opcode intact");
    check(img.at(RDATA_RAW == 0 ? 0 : RDATA_VA)[0] == 0x64, ".rdata was copied too");
    // .data: raw_size 0x100 but vsize 0x8000, so the tail must be zero.
    bool bss_zero = true;
    for (uint32_t i = DATA_VA + DATA_RAW_SIZE; i < DATA_VA + 0x8000; i += 0x40)
        if (img.at(i)[0]) bss_zero = false;
    check(bss_zero, "the BSS tail of .data reads as zeros");

    // Give this thread's TEB known values, then patch and call.
    teb()->prcb = 0x11223344;
    teb()->irql = 0x55;
    teb()->dpc_active = 1;

    FsPatchStats st{};
    int patched = img.patch_fs(&st);
    check(patched >= 7, "the patcher rewrote the image's fs-relative accesses");
    check(st.unknown == 0, "no fs offset in the fixture was left unhandled");
    check(img.at(RDATA_VA)[0] == 0x64, ".rdata was not touched, even though it is executable");

    // Now call the mapped code. Before the patch these read fs:[0x20] and would fault, because the
    // Linux %fs base is 0. After it they read gs:[K+0x20], which is this thread's TEB.
    auto call = [&](uint32_t off) -> uint32_t {
        auto fn = (uint32_t(*)(void))((uintptr_t)(TEXT_VA + off));
        return fn();
    };
    check(call(FN_READ_PRCB) == 0x11223344, "patched code reads KPCR.Prcb out of this thread's block");
    check(call(FN_READ_IRQL) == 0x55, "patched code reads KPCR.Irql");
    check(call(FN_READ_DPC) == 1, "patched code reads KPRCB.DpcRoutineActive");

    uint32_t r1 = call(FN_SET_IRQL);
    check(r1 == 0x77 && teb()->irql == 0x77, "patched code writes KPCR.Irql and reads it back");


    // The MSVC SEH prologue shape: push <cookie>; mov fs:[0], esp. After patching it must land in
    // our own chain head. The stub returns the head it installed, not the cookie.
    uint32_t seh_head_before = teb()->seh;
    auto push_seh = (uint32_t(*)(void))((uintptr_t)(TEXT_VA + FN_PUSH_SEH));
    uint32_t installed = push_seh();
    check(installed != 0 && installed == teb()->seh, "the SEH prologue stub installed a record and read the head back");
    check(installed != seh_head_before, "and the head changed from what it was");
    teb()->seh = seh_head_before;

    check(call(FN_NESTED) == 0x11223344, "a doubly-prefixed fs access is patched and still reads the TEB");

    // And the unpatched behaviour, to prove the patch is what made it work: a fresh thread would
    // fault on a raw fs access. Assert the prefix is gone rather than triggering it.
    check(img.at(TEXT_VA + FN_READ_PRCB)[0] == 0x65, "the fs prefix byte is now gs in the mapped image");
    // K depends on where glibc puts this thread's block, so the bytes are compared against the value the
    // patcher baked rather than against numbers typed in here.
    uint32_t baked;
    memcpy(&baked, img.at(TEXT_VA + FN_READ_PRCB) + 2, 4);
    check(baked == teb_patch_offset(0x20), "the displacement in the mapped image is the patched K+offset");

    img.unmap();
    check(!img.valid(), "unmapped cleanly");
}

static void test_contiguous()
{
    printf("\ncontiguous physical memory\n");
    std::string err;
    bool ok = map_contiguous_block(err);
    check(ok, "mapped the 64 MB block at 0x80000000" + (ok ? "" : (": " + err)));
    if (!ok) return;
    check(contiguous_base() == 0x80000000 && contiguous_size() == (64u << 20), "base and size");
    auto *p = (volatile uint32_t *)(uintptr_t)contiguous_base();
    p[0] = 0x5A5A5A5Au;
    check(p[0] == 0x5A5A5A5Au, "it is really at that address, and writable");
    check(p[16] == 0, "and it is zeroed, like MmAllocateContiguousMemory would return");
    unmap_contiguous_block();
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("BLACK loader -- XBE parser and image mapping tests\n");
    test_parser();
    test_map_and_execute();
    test_contiguous();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    unmap_contiguous_block();
    return g_failures ? 1 : 0;
}