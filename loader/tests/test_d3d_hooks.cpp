// The library-section hook installer, against the real section tables.
//
// Nothing here needs the game. What is checked is the part that is easy to get quietly wrong:
//
//   - every function in the D3D/DSOUND/XPP tables of the known build is covered, exactly once;
//   - a patched entry is a five-byte jump that stays inside that function's own bytes (four functions of
//     that build are shorter than a jump, and they trap instead);
//   - a call through a patched entry reaches the shared tail with the function's own name, the caller's
//     return address, the register arguments and the stack arguments intact;
//   - a call through a *trapped* entry is recognised by the signal bridge and reported the same way.
//
// The image is synthetic: the tables hold absolute addresses, so the region is mapped where the tables
// say it is (0x200000, well clear of the loader's own mappings and of the 64 MB block at 0x80000000) and
// filled with a filler byte, so "was this byte ours?" is answerable.
#include "d3d/d3d_hooks_posix.h"
#include "d3d/thunk_posix.h"
#include "platform/posix/seh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

static int g_failures, g_checks;

static std::string to_hex(uint32_t v)
{
    char buf[16];
    snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

static void check(bool cond, const std::string &what)
{
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s\n", what.c_str());
        g_failures++;
    }
    fflush(stdout);
}

static void check_eq_u32(uint64_t got, uint64_t want, const std::string &what)
{
    char buf[192];
    snprintf(buf, sizeof buf, "%s (got %llu, want %llu)", what.c_str(), (unsigned long long)got,
             (unsigned long long)want);
    check(got == want, buf);
}

// A caller written in assembly because the point is what the caller's stack and registers look like when
// the thunk is entered: three pushed arguments, and eax/ecx/edx holding the values an LTCG caller would
// pass in registers. It cleans up its own arguments afterwards, which is what a cdecl caller would do --
// the loader's thunks never return in production, so this only has to hold for the test's sake.
//
// No callee-saved register is used here on purpose. An earlier version kept the entry address in %ebx
// across the call with push/pop, and the assembler deleted the `pop %ebx` as dead: %ebx really is dead
// after the frame teardown, but the *stack* it was sitting on is not, so the `ret` after it popped a word
// from the wrong place and the test jumped into whatever happened to be there. The symptom was a fault
// inside the synthetic image's filler byte -- three steps and one function away from the cause.
extern "C" __attribute__((cdecl)) uint32_t call_hook_entry(void *entry, uint32_t expect_arg1);
__asm__(".text\n"
        ".globl call_hook_entry\n"
        ".type call_hook_entry, @function\n"
        "call_hook_entry:\n"
        "    pushl %ebp\n"
        "    movl %esp, %ebp\n"
        "    subl $4, %esp\n"             // a scratch slot inside the frame: no register to restore
        "    movl 8(%ebp), %eax\n"
        "    movl %eax, -4(%ebp)\n"
        "    pushl $0x34343434\n"
        "    pushl $0x33333333\n"
        "    pushl $0x32323232\n"
        "    movl $0xaaaaaaaa, %eax\n"
        "    movl $0xbbbbbbbb, %ecx\n"
        "    movl $0xcccccccc, %edx\n"
        "    call *-4(%ebp)\n"
        "    addl $12, %esp\n"            // the thunk returned without dropping them
        "    movl %ebp, %esp\n"
        "    popl %ebp\n"
        "    ret\n"
        ".size call_hook_entry, .-call_hook_entry\n");

// Everything the three sections cover, as one list, with the room each entry really has. The tables are
// already sorted by address inside each section; the room calculation is repeated here on purpose -- the
// test's answer and the installer's answer must agree, and if they stop agreeing it says so.
struct Row {
    uint32_t addr;
    uint32_t size;
    uint32_t room;
    const char *name;
    const char *section;
};

static std::vector<Row> collect()
{
    struct Section {
        const xd3d::LibFunc *funcs;
        size_t count;
        const char *name;
    };
    size_t n3 = 0, ns = 0, nx = 0;
    const Section sections[] = {
        {xd3d::d3d_section_functions(&n3), n3, "D3D"},
        {xd3d::dsound_section_functions(&ns), ns, "DSOUND"},
        {xd3d::xpp_section_functions(&nx), nx, "XPP"},
    };

    std::vector<Row> all;
    for (size_t s = 0; s < 3; s++) {
        std::vector<xd3d::LibFunc> sorted(sections[s].funcs, sections[s].funcs + sections[s].count);
        std::sort(sorted.begin(), sorted.end(),
                  [](const xd3d::LibFunc &a, const xd3d::LibFunc &b) { return a.addr < b.addr; });
        for (size_t i = 0; i < sorted.size(); i++) {
            const uint32_t addr = sorted[i].addr;
            const uint32_t room = (i + 1 < sorted.size() && sorted[i + 1].addr - addr < sorted[i].size)
                                      ? sorted[i + 1].addr - addr
                                      : sorted[i].size;
            all.push_back(Row{addr, sorted[i].size, room, sorted[i].name, sections[s].name});
        }
    }
    return all;
}

static const uint32_t kImageBase = 0x00200000;

// A replacement the HLE tests can name in a table. It is never called -- these tests check what the
// installer does with a row, not what the row does -- so its body only has to be a real address.
static void hle_impl(uint32_t arg) { (void)arg; }
static const uint8_t kFiller = 0x90;

int main()
{
    printf("BLACK loader -- library section hooks\n");

    const std::vector<Row> rows = collect();

    // --- the tables themselves -----------------------------------------------------------------------
    printf("\nthe section tables\n");
    check(rows.size() == 819, "the three sections hold 819 functions (" + std::to_string(rows.size()) + ")");
    {
        std::vector<uint32_t> addrs;
        bool sized = true;
        for (const Row &r : rows) {
            if (r.size == 0) sized = false;
            addrs.push_back(r.addr);
        }
        std::sort(addrs.begin(), addrs.end());
        bool unique = true;
        for (size_t i = 1; i < addrs.size(); i++)
            if (addrs[i] == addrs[i - 1]) unique = false;
        check(unique, "no address appears twice across the three sections");
        check(sized, "every function has a non-zero size");

        // Eight of the 819 entries are shorter than a five-byte jump: the table has one 3-byte function in
        // D3D and seven 4-byte ones across DSOUND and XPP. The installer's room is min(size, distance to the
        // next entry), and here the size is what binds -- but the neighbour half of that rule is still the
        // reason it is written that way, because a jump written past a function's end lands in the next
        // entry and breaks two functions with nothing in the log to point at either.
        size_t short_by_size = 0, short_by_neighbour = 0;
        for (const Row &r : rows) {
            if (r.size < 5) short_by_size++;
            else if (r.room < 5) short_by_neighbour++;
        }
        check_eq_u32(short_by_size, 8, "eight functions are shorter than a jump by their own size");
        check_eq_u32(short_by_neighbour, 0, "and none is shortened only by the next entry starting too close");
        check_eq_u32(short_by_size + short_by_neighbour, 8, "so eight entries trap instead of jumping");
    }

    // --- installing ----------------------------------------------------------------------------------
    printf("\ninstalling into a synthetic image\n");

    uint32_t lowest = 0xFFFFFFFF, highest = 0;
    for (const Row &r : rows) {
        lowest = std::min(lowest, r.addr);
        highest = std::max(highest, r.addr + r.size);
    }
    // mmap wants a page-aligned address and the first function is not one.
    const uint32_t region_base = lowest & ~0xFFFu;
    const size_t span = (((size_t)(highest - region_base)) + 0xFFF) & ~(size_t)0xFFF;
    void *region = mmap((void *)(uintptr_t)region_base, span, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (region == MAP_FAILED) {
        check(false, "the synthetic image region is mapped");
        printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    check((uintptr_t)region == region_base, "the synthetic image is mapped where the tables say it is");
    memset(region, kFiller, span);

    // The signal bridge is what turns the trap bytes below into reports, exactly as it does in the program.
    seh_install_signal_bridge();

    uint8_t *code = (uint8_t *)(uintptr_t)kImageBase;
    xd3d::HookReport report;
    std::string err;
    xd3d::hooks_set_stop_on_unimplemented(false); // so a thunk can return and be inspected
    // The image size is measured from the *image base*, not from where the mapping starts. The two differ on
    // purpose here: the mapping begins at the lowest function's page while the base is a page below it, which
    // is the case that put the thunk arena inside the image when the installer anchored on the base alone.
    const bool installed = xd3d::hooks_install_all(kImageBase, highest - kImageBase, code, &report, err);
    check(installed, "every entry of the three sections was hooked" + (installed ? std::string() : ": " + err));
    if (!installed) {
        printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    check_eq_u32(report.functions, rows.size(), "the installer saw every function");
    check_eq_u32(report.patched + report.trapped, rows.size(), "each one is either patched or trapped");
    check_eq_u32(report.trapped, 8, "and the eight short ones trapped instead of jumping");
    check_eq_u32(report.patched, rows.size() - 8, "so the rest were patched");

    // --- what the patches look like -------------------------------------------------------------------
    printf("\nwhat was written\n");
    {
        const size_t expected_jumps = rows.size() - 8;
        size_t jumps = 0, traps = 0, spills = 0, untouched = 0, bad_tail = 0;
        uint32_t first_tail = 0;
        bool first_tail_set = false;
        for (const Row &r : rows) {
            const uint8_t *fn = code + (r.addr - kImageBase);
            if (r.room < 5) {
                if (fn[0] == 0xCC) traps++;
                continue;
            }
            if (fn[0] != 0xE9) {
                untouched++;
                continue;
            }
            jumps++;
            // Nothing past this function's own bytes may have moved. A jump five bytes long that ran into
            // the next function's entry would leave both entries broken, and the game would fault there.
            for (uint32_t i = 5; i < r.room; i++)
                if (fn[i] != kFiller) spills++;
            int32_t rel;
            memcpy(&rel, fn + 1, 4);
            const uint8_t *thunk = fn + 5 + rel;
            // The thunk's exact shape: push the row index, call the one shared tail, drop the index, return
            // to the game. The `call` rather than a `jmp` is deliberate -- see lib_unimplemented_tail.
            if (thunk[0] != 0x68) bad_tail++;
            if (thunk[5] != 0xE8) bad_tail++;
            if (thunk[10] != 0x83 || thunk[11] != 0xC4 || thunk[12] != 0x04 || thunk[13] != 0xC3) bad_tail++;
            int32_t tail_rel;
            memcpy(&tail_rel, thunk + 6, 4);
            const uint32_t tail = (uint32_t)(uintptr_t)(thunk + 10 + tail_rel);
            if (!first_tail_set) {
                first_tail = tail;
                first_tail_set = true;
            } else if (tail != first_tail) {
                bad_tail++; // every thunk must go to the one shared tail
            }
        }
        check_eq_u32(jumps, expected_jumps, "every patchable entry starts with a jump");
        check_eq_u32(traps, 8, "every entry too short for a jump holds a single breakpoint byte");
        check_eq_u32(spills, 0, "no patch reached past the end of its own function");
        check_eq_u32(untouched, 0, "no entry was left alone");
        check_eq_u32(bad_tail, 0,
                     "every thunk pushes its index, calls the one shared tail, and returns to the game");

        // The reason the installer maps its arena next to the image: every patch is a rel32 jump, and a
        // jump that cannot reach wraps silently and lands in the middle of somewhere. This is the check
        // that catches it, and it is here because the failure it prevents looks like a bug in the patched
        // function rather than in the patching.
        const uint32_t arena = xd3d::hooks_arena_base();
        size_t out_of_reach = 0;
        for (const Row &r : rows) {
            if (r.room < 5) continue;
            const int64_t delta = (int64_t)(arena + 0x100000) - (int64_t)r.addr;
            if (delta < INT32_MIN || delta > INT32_MAX) out_of_reach++;
        }
        check(out_of_reach == 0, "the thunk arena is within rel32 reach of every patched entry (" +
                                     std::to_string(out_of_reach) + " out of reach, arena at " + to_hex(arena) +
                                     ")");
    }

    // --- a call through a patched entry ----------------------------------------------------------------
    printf("\na call from \"game code\" through a patched entry\n");
    {
        const Row *victim = nullptr;
        for (const Row &r : rows)
            if (r.room >= 5 && std::string(r.section) == "D3D" && strncmp(r.name, "FUN_", 4) != 0)
                victim = &r; // a named D3D function, not one of the unnamed ones
        check(victim != nullptr, "a named function with room for a jump was found");
        if (victim) {
            void *entry = code + (victim->addr - kImageBase);
            uint8_t *saved = new uint8_t[victim->room];
            memcpy(saved, entry, victim->room);

            const uint32_t before = xd3d::hooks_hit_count();
            call_hook_entry(entry, 0x32323232);

            const xd3d::LibHit &hit = xd3d::hooks_last_hit();
            check_eq_u32(xd3d::hooks_hit_count(), before + 1, "the call was recorded once");
            check(std::string(hit.name) == victim->name,
                  "and it named the function the game called (" + std::string(hit.name) + ")");
            check(hit.addr == victim->addr, "with the address from the section table");
            check(std::string(hit.section) == victim->section,
                  "and the section it belongs to (" + std::string(hit.section) + ")");
            check_eq_u32(hit.eax, 0xAAAAAAAA, "eax reached the tail, as an LTCG caller would pass it");
            check_eq_u32(hit.ecx, 0xBBBBBBBB, "and ecx");
            check_eq_u32(hit.edx, 0xCCCCCCCC, "and edx");
            check_eq_u32(hit.args[0], 0x32323232, "and the first stack argument");
            check_eq_u32(hit.args[1], 0x33333333, "and the second");
            check_eq_u32(hit.args[2], 0x34343434, "and the third");
            // The caller is wherever the test binary happens to load -- it is PIE, and on this machine that
            // is the 0x5xxxxxxx range -- so the check is "a code address well above the synthetic image",
            // not a hardcoded one.
            check(hit.caller > 0x00400000u && hit.caller < 0x80000000u,
                  "the caller is inside this test binary (" + to_hex(hit.caller) + ")");

            memcpy(entry, saved, victim->room); // put it back: the next check wants a clean entry
            delete[] saved;
        }
    }

    // --- a call into a trapped entry --------------------------------------------------------------------
    printf("\na call into an entry too small to jump\n");
    {
        const Row *tiny = nullptr;
        for (const Row &r : rows)
            if (r.room < 5) tiny = &r;
        check(tiny != nullptr, "one of the short functions was found");
        if (tiny) {
            // Called as a plain call: the 0xCC raises SIGTRAP and the bridge's probe recognises it. That is
            // the whole reason the probe exists -- on Windows a vectored exception handler does the same.
            uint8_t *entry = code + (tiny->addr - kImageBase);
            // The bridge resumes *after* the 0xCC, so the byte there decides where the call goes next. In
            // the real image that is the rest of the original function; here a `ret` makes it come home.
            entry[1] = 0xC3;
            const uint32_t before = xd3d::hooks_hit_count();
            call_hook_entry(entry, 0);

            const xd3d::LibHit &hit = xd3d::hooks_last_hit();
            check_eq_u32(xd3d::hooks_hit_count(), before + 1, "the breakpoint was recognised and reported");
            check(hit.addr == tiny->addr, "as the function whose entry holds it (" + std::string(hit.name) + ")");
            check(std::string(hit.section) == tiny->section, "with its section");
        }
    }

    // --- an HLE replacement is actually installed ---------------------------------------------------------
    printf("\nan HLE replacement reaches the entry\n");
    {
        // The gap this closes: a row with a real implementation used to fall through to the name-and-stop
        // thunk, so a game stopped at its first D3D call no matter how much of the console had been ported.
        // The existing tests covered only the two refusals, so nothing noticed.
        // A region of its own, mapped here. The thunk lives in the arena beside whatever image was installed
        // first, and a five-byte jump from a stack buffer cannot reach anything -- the refusal further down
        // is the honest outcome of getting that wrong, which is why it is one of the two refusal tests.
        const size_t slot = 0x200;
        uint8_t *own = (uint8_t *)mmap((void *)(uintptr_t)kImageBase, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC,
                                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        check(own != MAP_FAILED, "a region for the HLE entry is mapped");
        if (own == MAP_FAILED) return 1;
        memset(own, kFiller, 0x4000);
        const xd3d::LibFunc fns[] = {{kImageBase + slot, 32, "HLE_ONE"}};
        const xd3d::Hle hle[] = {{kImageBase + slot, "HLE_ONE", (void *)&hle_impl, "s", true}};
        xd3d::HookReport one{};
        std::string err_h;
        const bool ok = xd3d::hooks_install(fns, 1, hle, 1, kImageBase, 0x4000, own, &one, err_h);
        check(ok, "a row with an implementation installs: " + err_h);
        check_eq_u32(one.patched, 1, "and counts as patched");
        check_eq_u32(one.hle, 1, "and as an HLE replacement, not as a name-and-stop thunk");
        check(own[slot] == 0xE9, "and the entry really was jumped over");

        // A row marked native is the opposite: no jump, no thunk, original code still in place.
        memset(own, kFiller, 0x4000);
        const xd3d::Hle native_hle[] = {{kImageBase + slot, "NATIVE", nullptr, "", true}};
        xd3d::HookReport two{};
        std::string err_n;
        xd3d::hooks_install(fns, 1, native_hle, 1, kImageBase, 0x4000, own, &two, err_n);
        check_eq_u32(two.hle, 0, "a row marked native is not counted as an HLE replacement");
        check_eq_u32(two.skipped, 1, "it is counted as skipped");
        check(own[slot] == kFiller, "and its entry is untouched");
        munmap(own, 0x4000);
    }

    // --- the per-build guard ---------------------------------------------------------------------------
    printf("\nthe guard refuses another build\n");
    {
        // The tables are per-build, and the guard is structural: every row has to land inside the image's
        // own section of the same name. A section the image does not have is a refusal, not a shrug --
        // patching anyway writes jumps into whatever now occupies that range.
        uint8_t page[0x1000];
        memset(page, kFiller, sizeof page);
        const xd3d::SectionSpan other[] = {{"SOMETHING_ELSE", kImageBase, 0x1000}};
        xd3d::HookReport rep{};
        std::string err_m;
        const bool ok_missing = xd3d::hooks_install_all_guarded(kImageBase, 0x1000, page, other, 1, &rep, err_m);
        check(!ok_missing && err_m.find("no D3D section") != std::string::npos,
              "an image with no D3D section is refused by name: " + err_m);

        // And the section that is there but too small for the table it holds: the same class of mistake,
        // caught the same way, with the address in the message so it can be traced back to a row.
        const xd3d::SectionSpan tiny[] = {{"D3D", kImageBase, 0x200}, {"DSOUND", kImageBase + 0x400, 0x200},
                                          {"XPP", kImageBase + 0x800, 0x200}};
        std::string err_t;
        xd3d::HookReport rep2{};
        const bool ok_tiny = xd3d::hooks_install_all_guarded(kImageBase, 0x4000, (uint8_t *)(uintptr_t)kImageBase,
                                                             tiny, 3, &rep2, err_t);
        check(!ok_tiny && err_t.find("another build") != std::string::npos,
              "a section too small for its table is refused, and says the tables are for another build: " + err_t);
    }

    // --- the trace ---------------------------------------------------------------------------------------
    printf("\nthe trace records what the game called\n");
    {
        // Side one: the emitter. A traced thunk is eight bytes longer, and the difference is a fixed block
        // rather than a variable one -- that is what keeps the size check and the emitted bytes in step.
        const xd3d::Hle row = {0x00200000, "TRACED", (void *)&hle_impl, "s", true};
        const size_t plain = xd3d::hle_thunk_size(row, false);
        const size_t traced = xd3d::hle_thunk_size(row, true);
        check(traced == plain + 8, "a traced thunk is eight bytes longer, and exactly eight");
        check_eq_u32(xd3d::hle_thunk_size(row, false), plain, "the untraced size does not depend on the flag twice");

        // Side two: the recorder. This is the function the thunk calls, so it is what decides whether
        // "the game got this far" is an answer the report can print.
        xd3d::hooks_trace_reset();
        check_eq_u32(xd3d::hooks_trace_count(), 0, "a fresh trace is empty");
        check(!xd3d::hooks_trace_overflowed(), "and not marked truncated");

        xd3d::blackrt_hle_trace(0x0020AAAAu);
        xd3d::blackrt_hle_trace(0x0020BBBBu);
        check_eq_u32(xd3d::hooks_trace_count(), 2, "two calls are two records");
        if (xd3d::hooks_trace_count() == 2) {
            check_eq_u32(xd3d::hooks_trace_at(0), 0x0020AAAAu, "recorded in order, first call first");
            check_eq_u32(xd3d::hooks_trace_at(1), 0x0020BBBBu, "and then the second");
            check(xd3d::hooks_trace_at(2) == 0, "reading past the end answers zero, not garbage");
        }

        xd3d::hooks_trace_reset();
        check_eq_u32(xd3d::hooks_trace_count(), 0, "the trace can be cleared between runs");
    }

    // --- the two refusal cases -------------------------------------------------------------------------
    printf("\nwhat the installer refuses\n");
    {
        // An HLE replacement cannot be delivered by a trap: the arguments would be lost, so it is an error
        // rather than a silent downgrade. The Windows port refuses in exactly the same situation.
        const xd3d::LibFunc tiny[] = {{0x00200000, 3, "TOO_SMALL"}};
        std::vector<uint8_t> page(0x1000, kFiller);
        uint8_t *where = (uint8_t *)page.data();
        const xd3d::Hle hle[] = {{0x00200000, "TOO_SMALL", (void *)&where, "ss", true}};
        xd3d::HookReport one;
        std::string err2;
        const bool ok = xd3d::hooks_install(tiny, 1, hle, 1, kImageBase, 0x1000, where, &one, err2);
        check(!ok && err2.find("too small") != std::string::npos,
              "an HLE replacement on a 3-byte function is refused by name: " + err2);
    }
    {
        // impl == nullptr means "leave the original code in place", and the entry must not be touched.
        uint8_t page[0x1000];
        memset(page, kFiller, sizeof page);
        const xd3d::LibFunc fns[] = {{kImageBase + 0x100, 32, "NATIVE"}};
        const xd3d::Hle hle[] = {{kImageBase + 0x100, "NATIVE", nullptr, "", true}};
        xd3d::HookReport one;
        std::string err3;
        const bool ok = xd3d::hooks_install(fns, 1, hle, 1, kImageBase, 0x1000, page, &one, err3);
        check(ok && one.skipped == 1 && page[0x100] == kFiller,
              "a function marked native is left exactly as it was");
    }
    {
        // An address outside the image is a table/base mismatch, not something to skip quietly: skipping
        // would leave the game running the original code in a function that is supposed to be ours.
        const xd3d::LibFunc below[] = {{kImageBase - 4, 32, "BEFORE_IMAGE"}};
        std::vector<uint8_t> page(0x1000, kFiller);
        xd3d::HookReport one;
        std::string err4;
        const bool ok = xd3d::hooks_install(below, 1, nullptr, 0, kImageBase, 0x1000, page.data(), &one, err4);
        check(!ok && err4.find("outside this image") != std::string::npos,
              "a function below the image base is refused: " + err4);

        // The case that matters in the program: an image that is not the build these tables came from. The
        // refusal has to say so, because writing a jump at those addresses would patch whatever the loader
        // has mapped there and fail later with no explanation at all.
        const xd3d::LibFunc beyond[] = {{kImageBase + 0x2000, 32, "PAST_THE_END"}};
        std::string err5;
        const bool ok2 = xd3d::hooks_install(beyond, 1, nullptr, 0, kImageBase, 0x1000, page.data(), &one, err5);
        check(!ok2 && err5.find("another build") != std::string::npos,
              "a function past the end of a smaller image is refused: " + err5);
    }

    xd3d::hooks_set_stop_on_unimplemented(true);
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}