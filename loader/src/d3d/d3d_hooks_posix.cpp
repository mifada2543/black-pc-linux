// Hooks for the XBE's library sections -- see d3d_hooks_posix.h for the shape and for why it is this shape.
#include "d3d/d3d_hooks_posix.h"

#include "d3d/thunk_posix.h"

#include "platform/posix/boot.h"
#include "platform/posix/log.h"
#include "platform/posix/seh.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

namespace xd3d {
namespace {

// The generated tables, straight from the section lists of the known build. They are already C++
// initialisers of the same shape as LibFunc, so including them is not a parse step that could go wrong.
const LibFunc kD3D[] = {
#include "d3d_functions.inc"
};
const LibFunc kDSound[] = {
#include "dsound_functions.inc"
};
const LibFunc kXpp[] = {
#include "xpp_functions.inc"
};

// A row of the installed set, with the section it came from. The index of a row is what the thunks carry,
// so this vector's order is the thunks' order and must not change once they are made.
struct Row {
    LibFunc fn;
    const char *section;
};

std::vector<Row> g_rows;
uint8_t *g_thunks;
size_t g_thunks_used;
uint32_t g_arena_base;
uint32_t g_image_size;
uint8_t *g_image_code;
const size_t kThunkBytes = 16; // push imm32 + call rel32 + add esp,4 + ret
const size_t kThunkPage = 0x40000;

// The arena goes *next to the image*, not wherever mmap hands out.
//
// Every patch is a rel32 jump, and a rel32 reaches a little under 2 GB. Asking for any address gets
// whatever the kernel feels like, which on this machine is regularly 2.4 GB above the image -- and then
// every jump silently wraps: the target computed from the truncated displacement lands in the middle of
// somewhere, or inside the image again, and the run carries on executing filler bytes until it faults.
// That is exactly the failure this had, and it looked like a bug in the patched function because the
// patched function was not what went wrong. Near the image is also where the Windows port's VirtualAlloc
// lands, by the same arithmetic.
uint8_t *arena_near(uint32_t image_size, uint8_t *code, std::string &err)
{
    // Anchored on the mapping, never on the image base the caller claims. A caller may legitimately pass a
    // base its mapping does not start at -- the tests do exactly that, to keep the arithmetic honest -- and
    // anchoring on the claim then put the arena *inside* the image. Nothing crashes at that point: the
    // patches are written, they point into the arena's own pages, and the first call through one executes
    // the wrong code with the game's arguments. It surfaced as a fault inside a filler byte, which is a
    // very long way from the cause.
    const uintptr_t anchor = (uintptr_t)code + ((image_size + 0xFFF) & ~(uint32_t)0xFFF);
    for (int attempt = 0; attempt < 8; attempt++) {
        const uintptr_t at = (anchor + 0x10000 + (uintptr_t)attempt * 0x200000) & ~(uintptr_t)0xFFF;
        void *p = mmap((void *)at, kThunkPage, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) continue;
        // Belt and braces: an arena inside the image is refused even if the kernel let it through.
        if ((uintptr_t)p < (uintptr_t)code + image_size && (uintptr_t)p + kThunkPage > (uintptr_t)code) {
            munmap(p, kThunkPage);
            continue;
        }
        g_arena_base = (uint32_t)(uintptr_t)p;
        return (uint8_t *)p;
    }
    err = "no free page for the thunk arena near the image (tried eight addresses above it)";
    return nullptr;
}

// A rel32 jump can only reach so far; say so by name instead of writing a jump that wraps.
bool within_reach(const uint8_t *from, const void *to, int32_t *rel_out)
{
    const int64_t delta = (int64_t)(uintptr_t)to - (int64_t)(uintptr_t)(from + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    *rel_out = (int32_t)delta;
    return true;
}

LibHit g_last_hit;
uint32_t g_hits;
bool g_stop_on_unimplemented = true;


void *code_alloc(size_t n, uint32_t image_size, uint8_t *code, std::string &err)
{
    if (!g_thunks) {
        g_thunks = arena_near(image_size, code, err);
        if (!g_thunks) return nullptr;
        g_image_size = image_size;
        g_image_code = code;
    }
    if (g_thunks_used + n > kThunkPage) return nullptr;
    uint8_t *p = g_thunks + g_thunks_used;
    g_thunks_used += (n + 15) & ~(size_t)15;
    return p;
}

bool write_jmp(uint8_t *from, const void *to)
{
    int32_t rel = 0;
    if (!within_reach(from, to, &rel)) return false;
    from[0] = 0xE9;
    memcpy(from + 1, &rel, 4);
    return true;
}

// The shared tail every thunk calls. The thunk pushes the row index and then *calls* this, so the frame is
// an ordinary one -- [esp] = return address into the thunk, [esp+4] = the index, [esp+8...] = the call's
// arguments -- and the tail ends with a plain `ret`. An earlier version had the thunk *jump* here, which put
// the index underneath the return address and made the way back out need a hand-written `mov`/`lea`/`jmp`
// sequence; that sequence read the wrong word and jumped into the call's first argument, which the fixture
// fills with 0x22222222 -- an address inside the synthetic image, so the run continued by accident instead
// of failing. The shape below is the one ktable_posix.cpp's stub_common already uses.
extern "C" __attribute__((stdcall)) void lib_unimplemented_tail();

__asm__(".text\n"
        ".globl lib_unimplemented_tail\n"
        ".type lib_unimplemented_tail, @function\n"
        "lib_unimplemented_tail:\n"
        "    pushl %ebp\n"
        "    movl %esp, %ebp\n"
        "    pushl %edx\n"            // LTCG callers pass arguments in registers, and the report needs them
        "    pushl %ecx\n"
        "    pushl %eax\n"
        "    leal 12(%ebp), %eax\n"   // &arguments: past the saved ebp, the return address and the index
        "    pushl %eax\n"
        "    pushl 8(%ebp)\n"         // the index
        "    call blackrt_lib_unimplemented_posix\n"
        "    movl %ebp, %esp\n"
        "    popl %ebp\n"
        "    ret\n"
        ".size lib_unimplemented_tail, .-lib_unimplemented_tail\n");

} // namespace

// ---- the trace ----
//
// A traced thunk calls blackrt_hle_trace before its implementation runs, which is the only place in
// the whole path where a called D3D entry point can be named. Bounded: a game left running for an
// hour makes millions of calls, and a report that grows without limit is a leak wearing a report's
// clothes. The overflow is counted rather than hidden, because "the trace stopped" and "the trace was
// complete" are different facts.
constexpr size_t kTraceMax = 8192;
std::vector<uint32_t> g_trace;
bool g_trace_overflow = false;

extern "C" void blackrt_hle_trace(uint32_t addr)
{
    if (g_trace.size() < kTraceMax) g_trace.push_back(addr);
    else g_trace_overflow = true;
}

const char *hooks_trace_name(uint32_t addr)
{
    for (const Row &r : g_rows)
        if (r.fn.addr == addr) return r.fn.name;
    return nullptr;
}

const char *hooks_trace_section(uint32_t addr)
{
    for (const Row &r : g_rows)
        if (r.fn.addr == addr) return r.section;
    return nullptr;
}

void hooks_trace_reset()
{
    g_trace.clear();
    g_trace_overflow = false;
}

size_t hooks_trace_count() { return g_trace.size(); }
uint32_t hooks_trace_at(size_t i) { return i < g_trace.size() ? g_trace[i] : 0; }
bool hooks_trace_overflowed() { return g_trace_overflow; }

extern "C" __attribute__((stdcall)) void blackrt_lib_unimplemented_posix(uint32_t index, const uint32_t *frame,
                                                                       uint32_t eax, uint32_t ecx, uint32_t edx)
{
    if (index < g_rows.size()) {
        const Row &row = g_rows[index];
        g_last_hit.index = index;
        g_last_hit.addr = row.fn.addr;
        g_last_hit.name = row.fn.name;
        g_last_hit.section = row.section;
        g_last_hit.caller = frame[0];
        g_last_hit.eax = eax;
        g_last_hit.ecx = ecx;
        g_last_hit.edx = edx;
        for (int i = 0; i < 6; i++) g_last_hit.args[i] = frame[1 + i];
        g_hits++;

        logf("==== UNIMPLEMENTED %s: %s (0x%08x)", row.section, row.fn.name, row.fn.addr);
        logf("    called from %08x", frame[0]);
        logf("    eax=%08x ecx=%08x edx=%08x  stack args: %08x %08x %08x %08x %08x %08x", eax, ecx, edx,
             frame[1], frame[2], frame[3], frame[4], frame[5], frame[6]);
    }

    if (!g_stop_on_unimplemented) return; // tests: hand control back so the caller can check what it saw

    // Ending the run is the honest answer. A return value invented here would be read as a real D3D result
    // and the game would carry on drawing with a device that does not exist; HalReturnToFirmware and the
    // --strict kernel stub park for exactly the same reason.
    char why[128];
    snprintf(why, sizeof why, "unimplemented library function %s", index < g_rows.size() ? g_rows[index].fn.name : "?");
    boot_request_exit(1, why);
    for (;;) sleep(3600);
}

namespace {

// The probe the signal bridge calls for a 0xCC that hooks_install wrote. False for any other address, so a
// trap raised anywhere else still travels the normal exception path and is reported as a breakpoint.
bool breakpoint_probe(uint32_t eip, uint32_t eax, uint32_t ecx, uint32_t edx, const uint32_t *frame)
{
    for (size_t i = 0; i < g_rows.size(); i++) {
        if (g_rows[i].fn.addr != eip) continue;
        blackrt_lib_unimplemented_posix((uint32_t)i, frame, eax, ecx, edx);
        return true;
    }
    return false;
}

bool install_section(const LibFunc *funcs, size_t count, const Hle *hle, size_t hle_count, const char *section,
                     uint32_t image_base, uint32_t image_size, uint8_t *code, HookReport *report, std::string &err,
                     bool trace)
{
    // Sorted by address, because "the room this function has" is read off the next one: a jump must not
    // reach into the function that follows. The rows are copied rather than pointed at: they are 16 bytes
    // each and there are 819 of them, so a copy costs nothing next to getting the element type wrong.
    std::vector<LibFunc> sorted(funcs, funcs + count);
    std::sort(sorted.begin(), sorted.end(),
              [](const LibFunc &a, const LibFunc &b) { return a.addr < b.addr; });

    HookReport local;
    for (size_t i = 0; i < sorted.size(); i++) {
        const LibFunc &f = sorted[i];
        local.functions++;

        // The addresses come from one known build. If they do not fall inside *this* image then this is not
        // that build, and writing a jump anyway would patch whatever the loader happens to have mapped at
        // that address -- in the tests, nothing at all, and the first function segfaults the process.
        if (!code || f.addr < image_base || f.addr - image_base + f.size > image_size) {
            char where[32];
            snprintf(where, sizeof where, "0x%08x", f.addr);
            err = std::string(f.name) + " at " + where + " is outside this image (0x" + std::to_string(image_base) +
                 "+0x" + std::to_string(image_size) + ") -- these tables are for another build";
            return false;
        }
        if (f.size == 0) {
            err = std::string(f.name) + " has size 0 in the section table";
            return false;
        }
        uint8_t *fn = code + (f.addr - image_base);
        const uint32_t room = (i + 1 < sorted.size() && sorted[i + 1].addr - f.addr < f.size)
                                  ? sorted[i + 1].addr - f.addr
                                  : f.size;

        const Hle *h = nullptr;
        for (size_t k = 0; k < hle_count; k++)
            if (hle[k].addr == f.addr) h = &hle[k];
        if (h && !h->impl) {
            local.skipped++; // the replacement says to leave the original code in place
            continue;
        }

        if (room < 5) {
            if (h) {
                // An HLE replacement has to be *called*: there is nowhere to send a five-byte jump to, and
                // trapping would lose the arguments. Refuse rather than pretend.
                err = std::string(f.name) + " is " + std::to_string(room) + " bytes, too small for a jump";
                return false;
            }
            // Four functions of the real build are this short. One 0xCC plus a probe that recognises it:
            // the same trade the Windows port makes with a vectored exception handler, for the same reason.
            fn[0] = 0xCC;
            local.trapped++;
            g_rows.push_back(Row{f, section});
            continue;
        }

        if (h && h->impl) {
            // The positive case, and the one that was missing: a row with a replacement has to be *called*,
            // not reported and stopped. The thunk is built in the arena beside the image, so the entry needs
            // only the same five bytes a jump always needs -- `room >= 5` above is the whole requirement.
            std::string thunk_err;
            void *t = make_hle_thunk(*h, &thunk_err, trace);
            if (!t) {
                err = thunk_err.empty() ? (std::string(f.name) + " has no thunk") : thunk_err;
                return false;
            }
            if (!write_jmp(fn, t)) {
                err = std::string(f.name) + " is too far from the thunk arena for a five-byte jump";
                return false;
            }
            local.patched++;
            local.hle++;
            g_rows.push_back(Row{f, section});
            continue;
        }

        const uint32_t index = (uint32_t)g_rows.size();
        uint8_t *target = (uint8_t *)code_alloc(kThunkBytes, image_size, code, err);
        if (!target) {
            if (err.empty()) err = "the thunk arena is full";
            return false;
        }
        target[0] = 0x68; // push imm32: the row this thunk stands for
        memcpy(target + 1, &index, 4);
        target[5] = 0xE8; // call the shared tail, so its frame is an ordinary stdcall one
        int32_t rel = 0;
        if (!within_reach(target + 5, (const void *)&lib_unimplemented_tail, &rel)) {
            err = "the shared thunk tail is out of rel32 reach of the arena";
            return false;
        }
        memcpy(target + 6, &rel, 4);
        target[10] = 0x83; // add esp, 4: the index goes, and the game's return address is on top again
        target[11] = 0xC4;
        target[12] = 0x04;
        target[13] = 0xC3; // ret
        if (!write_jmp(fn, target)) {
            err = std::string(f.name) + " is too far from the thunk arena for a five-byte jump";
            return false;
        }
        local.patched++;
        g_rows.push_back(Row{f, section});
    }

    if (report) *report = local;
    return true;
}

} // namespace

const LibFunc *d3d_section_functions(size_t *count)
{
    if (count) *count = sizeof kD3D / sizeof kD3D[0];
    return kD3D;
}

const LibFunc *dsound_section_functions(size_t *count)
{
    if (count) *count = sizeof kDSound / sizeof kDSound[0];
    return kDSound;
}

const LibFunc *xpp_section_functions(size_t *count)
{
    if (count) *count = sizeof kXpp / sizeof kXpp[0];
    return kXpp;
}

const LibHit &hooks_last_hit()
{
    return g_last_hit;
}

uint32_t hooks_hit_count()
{
    return g_hits;
}

void hooks_set_stop_on_unimplemented(bool on)
{
    g_stop_on_unimplemented = on;
}

uint32_t hooks_arena_base()
{
    return g_arena_base;
}

void *hooks_alloc_thunk(size_t bytes, std::string &err)
{
    void *p = code_alloc(bytes, g_image_size, g_image_code, err);
    return p;
}

bool hooks_prepare_arena(uint32_t image_size, uint8_t *code, std::string &err)
{
    return code_alloc(16, image_size, code, err) != nullptr;
}

bool hooks_install(const LibFunc *funcs, size_t count, const Hle *hle, size_t hle_count, uint32_t image_base,
                   uint32_t image_size, uint8_t *code, HookReport *report, std::string &err)
{
    return install_section(funcs, count, hle, hle_count, "", image_base, image_size, code, report, err, false);
}

// Find the image's own section of this name. The guard compares against this and not against the image's
// total size, because "inside the image" is too weak: the tables describe three specific sections, and a
// different build moves them.
const SectionSpan *find_span(const SectionSpan *sections, size_t count, const char *name)
{
    for (size_t i = 0; i < count; i++)
        if (sections[i].name && strcmp(sections[i].name, name) == 0) return &sections[i];
    return nullptr;
}

// Every row of a table has to sit inside the section it describes. One row outside means the table is for
// another build, and a refusal by name is the honest answer: patching anyway writes jumps into whatever
// happens to be mapped there, and the first of them is a crash with nothing to explain it.
bool rows_fit(const SectionSpan *span, const LibFunc *funcs, size_t count, const char *which, std::string &err)
{
    for (size_t i = 0; i < count; i++) {
        const LibFunc &f = funcs[i];
        const uint32_t rel = f.addr - span->vaddr;
        if (f.addr < span->vaddr || rel + f.size > span->vsize) {
            char where[32];
            snprintf(where, sizeof where, "0x%08x", f.addr);
            err = std::string(which) + ": " + f.name + " at " + where + " is outside this image's " +
                 span->name + " section (0x" + std::to_string(span->vaddr) + "+0x" + std::to_string(span->vsize) +
                 ") -- these tables are for another build";
            return false;
        }
    }
    return true;
}

bool hooks_install_all_impl(uint32_t image_base, uint32_t image_size, uint8_t *code,
                            const SectionSpan *sections, size_t section_count, const Hle *hle, size_t hle_count,
                            HookReport *report, bool trace, std::string &err)
{
    struct Table {
        const LibFunc *funcs;
        size_t count;
        const char *name;
    };
    const Table tables[] = {
        {kD3D, sizeof kD3D / sizeof kD3D[0], "D3D"},
        {kDSound, sizeof kDSound / sizeof kDSound[0], "DSOUND"},
        {kXpp, sizeof kXpp / sizeof kXpp[0], "XPP"},
    };

    g_rows.clear();
    g_thunks_used = 0;
    g_thunks = nullptr; // re-aimed at this image: a second image may be nowhere near the first one's arena
    g_image_size = 0;
    g_image_code = nullptr;
    hooks_trace_reset();

    HookReport total;
    for (size_t t = 0; t < 3; t++) {
        // The guard. A section the image does not have at all is a refusal too: the table would otherwise be
        // patched into whatever now occupies that address range.
        if (sections && section_count) {
            const SectionSpan *span = find_span(sections, section_count, tables[t].name);
            if (!span) {
                err = std::string("this image has no ") + tables[t].name +
                     " section, so its function table has nothing to install into";
                return false;
            }
            if (!rows_fit(span, tables[t].funcs, tables[t].count, tables[t].name, err)) return false;
        }

        HookReport one;
        if (!install_section(tables[t].funcs, tables[t].count, hle, hle_count, tables[t].name, image_base,
                             image_size, code, &one, err, trace))
            return false;
        total.functions += one.functions;
        total.patched += one.patched;
        total.trapped += one.trapped;
        total.skipped += one.skipped;
        total.hle += one.hle;
    }

    seh_set_breakpoint_probe(&breakpoint_probe);
    if (report) *report = total;
    logf("hooks: %zu functions in the library sections: %zu patched (%zu with an HLE replacement), %zu trapped "
         "(too small to jump), %zu left native",
         total.functions, total.patched, total.hle, total.trapped, total.skipped);
    return true;
}

bool hooks_install_all(uint32_t image_base, uint32_t image_size, uint8_t *code, HookReport *report,
                       std::string &err)
{
    return hooks_install_all_impl(image_base, image_size, code, nullptr, 0, nullptr, 0, report, false, err);
}

bool hooks_install_all_guarded(uint32_t image_base, uint32_t image_size, uint8_t *code,
                               const SectionSpan *sections, size_t section_count, HookReport *report,
                               std::string &err)
{
    return hooks_install_all_impl(image_base, image_size, code, sections, section_count, nullptr, 0, report, false,
                                  err);
}

bool hooks_install_all_with_hle(uint32_t image_base, uint32_t image_size, uint8_t *code,
                                const SectionSpan *sections, size_t section_count, const Hle *hle, size_t hle_count,
                                HookReport *report, bool trace, std::string &err)
{
    return hooks_install_all_impl(image_base, image_size, code, sections, section_count, hle, hle_count, report,
                                  trace, err);
}

} // namespace xd3d