// Hooks for the XBE's library sections (D3D, DSOUND, XPP), Linux side.
//
// The port of src/d3d/dispatch.cpp. The shape is the same on purpose -- the Windows file is the invoice,
// and the reasons in its comments are the reason this one exists in this shape:
//
//   - Every function in the replaced sections is patched at its *entry*, not through a vtable or import
//     table, because the game calls them by address inside its own image and there is no other seam.
//   - A function with an HLE replacement jumps to a generated thunk; everything else jumps to a thunk that
//     names the function, logs the call, and ends the run. Leaving library code in place means NV2A
//     commands reaching a GPU that does not exist here.
//   - Four functions in the real build are shorter than the five bytes a jmp needs. They get a single 0xCC
//     instead, and the signal bridge recognises the trap (see seh_set_breakpoint_probe). Windows does this
//     with a vectored exception handler; both exist for the same reason.
//
// What is *not* here yet: the parameter-forwarding thunk for HLE replacements (the Windows `make_thunk`,
// which decodes a "s"/"ecx"/"edx" parameter spec). That arrives with the device layer, which is the first
// thing that needs it.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace xd3d {

// One row of the generated section tables (src/*_functions.inc): where a function is and how big it is.
struct LibFunc {
    uint32_t addr;
    uint32_t size;
    const char *name;
};

struct HookReport {
    size_t functions = 0; // functions in the table
    size_t patched = 0;   // entry patched with a jump to a thunk
    size_t trapped = 0;   // too small to patch: a 0xCC plus a trap handler instead
    size_t skipped = 0;   // not patched at all (an HLE entry that says "leave the original")
    size_t hle = 0;       // patched with an HLE replacement rather than with a name-and-stop thunk
};

// The three section tables, built for one specific XBE (its SHA-256 is in the .inc header).
const LibFunc *d3d_section_functions(size_t *count);
const LibFunc *dsound_section_functions(size_t *count);
const LibFunc *xpp_section_functions(size_t *count);

// Patch every entry of `funcs`. `image_base` is the VA the image is linked at and `code` is the first byte
// of the mapping, so the entry of `f` is code + (f.addr - image_base). Every row is checked against the
// mapped range and against its own size first: a jump written past the end of a function would overwrite
// the next one's entry, which is a crash with no explanation in sight.
//
// An HLE replacement is a row of `hle`; a row with `impl == nullptr` means "leave the original code in
// place", which is only safe for functions that read nothing but state this port keeps valid.
struct Hle {
    uint32_t addr;
    const char *name;
    void *impl; // nullptr = run the original
    const char *params;
    bool returns;
};

bool hooks_install(const LibFunc *funcs, size_t count, const Hle *hle, size_t hle_count, uint32_t image_base,
                   uint32_t image_size, uint8_t *code, HookReport *report, std::string &err);

// Where one library section lives in *this* image. The function tables are per-build, so a table may only be
// installed into a section of the same name, and only if every row of it falls inside that section's range.
// The guard is structural rather than cryptographic on purpose: the tables carry the build's SHA-256 in a
// comment, and nothing here could check a hash it cannot compute -- the image is not on this machine. What
// it can check is the thing that would actually go wrong: writing a jump into a range that is not the
// section the table describes.
struct SectionSpan {
    const char *name;
    uint32_t vaddr = 0;
    uint32_t vsize = 0;
};

// `sections` may be null, which means "no guard": the installer then checks only that each row lies inside
// the image, exactly as before. Passing the spans is what turns a silent mis-patch into a refusal by name.
bool hooks_install_all_guarded(uint32_t image_base, uint32_t image_size, uint8_t *code,
                               const SectionSpan *sections, size_t section_count, HookReport *report,
                               std::string &err);

// The three tables above, all of them, in the order they are hooked, with no HLE replacements installed.
bool hooks_install_all(uint32_t image_base, uint32_t image_size, uint8_t *code, HookReport *report,
                       std::string &err);

// Install all three sections *and* an HLE replacement table, optionally building traced thunks. This is the
// call the boot path makes; `hooks_install_all` above is the bare one the tests use.
bool hooks_install_all_with_hle(uint32_t image_base, uint32_t image_size, uint8_t *code,
                                const SectionSpan *sections, size_t section_count, const Hle *hle, size_t hle_count,
                                HookReport *report, bool trace, std::string &err);

// Where the generated thunks live, and it is a fact worth exposing: the arena is mapped next to the image
// because every patch is a rel32 jump, and a jump that cannot reach is a silent corruption rather than an
// error.
uint32_t hooks_arena_base();

// Room for a generated thunk, from that same arena. The HLE thunk builder (thunk_posix.h) allocates here
// rather than making an arena of its own: two arenas would mean two places where reach has to be checked,
// and the one nobody remembered to check is the one that breaks.
void *hooks_alloc_thunk(size_t bytes, std::string &err);

// Map the arena without installing anything, for a caller that only wants to build thunks (the tests, and
// a boot sequence that builds HLE thunks before it patches any entry). Installing hooks maps it too.
bool hooks_prepare_arena(uint32_t image_size, uint8_t *code, std::string &err);

// What the loader wants to know about a call it stopped on.
struct LibHit {
    uint32_t index;      // row in the table it was installed from
    uint32_t addr;       // the function's own VA
    const char *name;    // its name from the table
    const char *section; // "D3D", "DSOUND" or "XPP"
    uint32_t caller;     // return address
    uint32_t eax, ecx, edx;
    uint32_t args[6];    // the first six stack words, as the Windows report shows them
};
const LibHit &hooks_last_hit();
uint32_t hooks_hit_count();

// The first call to a library function ends the run, because a return value invented by the loader would
// be worse than a stopped run. Tests turn it off so they can call a thunk and see what it does.
void hooks_set_stop_on_unimplemented(bool on);

// ---- the call trace ----
//
// A traced thunk reports its row's address before the replacement runs, which is the only place a called
// D3D entry point can be named -- and naming it is the whole point of `--probe`: "the game got past
// CreateDevice and stopped at the next named D3D function" is only a useful answer if the functions before
// it were recorded too. Addresses, not indices: whoever builds a thunk may not know its row index in the
// combined table, but the address is in the row itself.
void hooks_trace_reset();
size_t hooks_trace_count();
uint32_t hooks_trace_at(size_t i);
const char *hooks_trace_name(uint32_t addr);   // null when the address is not a row of this build's tables
const char *hooks_trace_section(uint32_t addr);
bool hooks_trace_overflowed(); // true once the trace hit its bound; "truncated", not "complete"

} // namespace xd3d