#include "platform/posix/boot.h"

#include "kernel/k_memory_posix.h"
#include "kernel/k_file_posix.h"
#include "kernel/k_misc_posix.h"
#include "kernel/k_thread_posix.h"
#include "platform/posix/fs_patch.h"
#include "platform/posix/log.h"
#include "platform/posix/seh.h"

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <atomic>

namespace {

void collect(XbePosix &x, BootReport &rep)
{
    rep.kind = x.kind;
    rep.title = x.title;
    rep.title_id = x.title_id;
    rep.base = x.base;
    rep.size_of_headers = x.size_of_headers;
    rep.size_of_image = x.size_of_image;
    rep.entry = x.entry;
    rep.thunk_addr = x.thunk_addr;
    rep.pe_base = x.pe_base;
    rep.stack_commit = x.stack_commit;
    rep.heap_reserve = x.heap_reserve;
    rep.heap_commit = x.heap_commit;
    rep.timedate = x.timedate;
    rep.debug_path = x.debug_path();
    rep.has_tls = x.has_tls;
    rep.tls = x.tls;

    rep.sections.clear();
    for (const XbeSectionInfo &s : x.sections) {
        BootReport::Section out;
        out.name = s.name;
        out.vaddr = s.vaddr;
        out.vsize = s.vsize;
        out.raw_size = s.raw_size;
        out.flags = s.flags;
        out.executable = (s.flags & XBE_SEC_EXECUTABLE) != 0;
        out.writable = (s.flags & XBE_SEC_WRITABLE) != 0;
        out.preloaded = (s.flags & XBE_SEC_PRELOAD) != 0;
        rep.sections.push_back(out);
    }
    rep.ordinals = x.kernel_imports();
}

const char *section_flags(const BootReport::Section &s)
{
    static thread_local char buf[8];
    buf[0] = s.executable ? 'x' : '-';
    buf[1] = s.writable ? 'w' : '-';
    buf[2] = s.preloaded ? 'p' : '-';
    buf[3] = 0;
    return buf;
}

// The one exit request the loader honours, whatever asked for it. HalReturnToFirmware and KeBugCheck both
// set it and then park their thread, so whoever gets here first is the reason the run ended.
std::atomic<int> g_exit_code{-1}; // negative = no exit requested
const char *g_exit_why = "unspecified";

// The thread boot_run_entry is watching, so that an exit request can mark it finished. Without this the
// loader reads "did it finish?" and "was an exit requested?" as two separate reads, and a game that
// bugchecks and returns in the same instant can be reported as a clean return with its bugcheck code
// dropped. The record outlives boot_run_entry -- threads are never freed -- so this pointer stays valid.
std::atomic<XboxThread *> g_watched{nullptr};

} // namespace

void boot_request_exit(int code, const char *why)
{
    // Only the first request is logged: HalReturnToFirmware and KeBugCheck both park their thread, so a
    // second call can only come from somewhere that did not notice.
    int expected = -1;
    if (g_exit_code.compare_exchange_strong(expected, code)) {
        g_exit_why = why ? why : "unspecified";
        logc("exit requested: %s (code %d)", g_exit_why, code);
    }
    // The requester is parked and will not finish on its own, so the watched thread is marked finished here.
    // The flag above is already set by the time this is visible, which is what closes the window between
    // "the run ended" and "the thread stopped": whoever wakes up reads both together.
    kthread_mark_finished(g_watched.load());
}

bool boot_exit_requested() { return g_exit_code.load() >= 0; }
int boot_exit_code() { return g_exit_code.load(); }
const char *boot_exit_reason() { return g_exit_why; }

std::string boot_default_hdd_dir()
{
    const char *xdg = getenv("XDG_DATA_HOME");
    std::string base = xdg && xdg[0] ? std::string(xdg) : std::string();
    if (base.empty()) {
        const char *home = getenv("HOME");
        base = home && home[0] ? std::string(home) + "/.local/share" : std::string(".");
    }
    return base + "/blacklinux";
}

void boot_print_report(const BootReport &rep)
{
    fprintf(stderr, "xbe: %s\n", rep.path.c_str());
    fprintf(stderr, "  title        '%s' id %08x, %s build, timestamp %u\n", rep.title.c_str(), rep.title_id,
            rep.kind.c_str(), rep.timedate);
    fprintf(stderr, "  base         0x%08x, headers 0x%x, image 0x%x\n", rep.base, rep.size_of_headers,
            rep.size_of_image);
    fprintf(stderr, "  entry        0x%08x, thunk table 0x%08x\n", rep.entry, rep.thunk_addr);
    fprintf(stderr, "  debug path   %s\n", rep.debug_path.empty() ? "(none)" : rep.debug_path.c_str());
    fprintf(stderr, "  stack/heap   commit 0x%x / reserve 0x%x / commit 0x%x\n", rep.stack_commit, rep.heap_reserve,
            rep.heap_commit);
    if (rep.has_tls)
        fprintf(stderr, "  tls          data 0x%08x..0x%08x, index 0x%08x, callbacks 0x%08x, zerofill 0x%x\n",
                rep.tls.data_start, rep.tls.data_end, rep.tls.index_addr, rep.tls.callbacks_addr, rep.tls.zero_fill);
    else
        fprintf(stderr, "  tls          (none)\n");
    fprintf(stderr, "  sections     %zu\n", rep.sections.size());
    for (const BootReport::Section &s : rep.sections)
        fprintf(stderr, "    %-10s vaddr 0x%08x vsize 0x%-8x raw 0x%-8x %s\n", s.name.c_str(), s.vaddr, s.vsize,
                s.raw_size, section_flags(s));
    fprintf(stderr, "  kernel imports %zu\n", rep.ordinals.size());
    if (!rep.ordinals.empty()) {
        fprintf(stderr, "   ");
        for (size_t i = 0; i < rep.ordinals.size() && i < 24; i++)
            fprintf(stderr, " %u(%s)", rep.ordinals[i], kernel_export_name(rep.ordinals[i]));
        if (rep.ordinals.size() > 24) fprintf(stderr, " ...");
        fprintf(stderr, "\n");
    }
    if (rep.fs_patched)
        fprintf(stderr, "  fs patch     %d instructions rewritten to gs (K + displacement)\n", rep.fs_patched);
    if (rep.fs_unknown) fprintf(stderr, "  fs patch     %d unrecognised forms left alone\n", rep.fs_unknown);
    if (rep.thunks.imports)
        fprintf(stderr, "  imports      %d resolved: %d implemented, %d data, %d logging stubs\n",
                rep.thunks.imports, rep.thunks.implemented, rep.thunks.data_exports, rep.thunks.stubbed);
}

bool boot_inspect(const std::string &path, XbePosix &x, BootReport &rep, std::string &err)
{
    rep.path = path;
    if (!x.load(path, err)) return false;
    collect(x, rep);
    return true;
}

bool boot_prepare(const std::string &path, XbePosix &x, XbeImage &img, BootReport &rep, std::string &err,
                  const std::string &hdd)
{
    if (!boot_inspect(path, x, rep, err)) return false;

    // Kernel memory first: the game's first instructions allocate, and the contiguous block is what
    // MmGetPhysicalAddress's arithmetic is defined against.
    if (!kmem_init()) {
        err = "cannot map the contiguous memory block (see the log)";
        return false;
    }

    if (!img.map(x, err)) return false;
    if (!img.copy_sections(err)) {
        img.unmap();
        return false;
    }

    FsPatchStats fs{};
    rep.fs_patched = img.patch_fs(&fs);
    rep.fs_unknown = fs.unknown;
    rep.fs_no_disp = fs.no_disp;

    if (!kernel_resolve_thunks(x, img, err)) {
        img.unmap();
        return false;
    }
    rep.thunks = kernel_thunk_stats();

    // Kernel memory before anything can allocate: the game allocates in its first instructions, and the
    // contiguous block is also what MmGetPhysicalAddress's arithmetic is defined against.
    uint32_t contig_free = 0, contig_used = 0, contig_reserved = 0;
    kmem_contiguous_stats(&contig_free, &contig_used, &contig_reserved);
    rep.mem_contiguous_base = contiguous_base();
    rep.mem_contiguous_free = contig_free;
    rep.mem_virtual_regions = kmem_virtual_regions();
    const uint32_t free_mb = contig_free >> 20;

    // The filesystem comes up before the game's first file call. The DVD is the folder this XBE came
    // from; the hard disk is a writable folder of its own, because a mounted DVD is read-only and the
    // game writes saves.
    {
        const size_t slash = path.find_last_of('/');
        const std::string game_dir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
        const std::string hdd_dir = hdd.empty() ? boot_default_hdd_dir() : hdd;
        kfile_init(game_dir.empty() ? std::string(".") : game_dir, hdd_dir);
    }

    // The section loader is told which image is running, so XeLoadSection can recognise the section
    // header the game hands it and copy the section where it belongs.
    kmisc_set_image(&img, &x);

    // The bridge is installed before anything can fault: from here on a fault becomes an exception the
    // game's own handlers get to see, instead of a process that dies where it stands.
    seh_install_signal_bridge();

    logc("memory: contiguous RAM at 0x%08x, %u MB, %u MB free after the reserved ends", contiguous_base(),
         contiguous_size() >> 20, free_mb);
    logc("xbe: '%s' %s build, base 0x%08x size 0x%x, entry 0x%08x, %zu sections, %zu kernel imports", rep.title.c_str(),
         rep.kind.c_str(), rep.base, rep.size_of_image, rep.entry, rep.sections.size(), rep.ordinals.size());
    logc("fs: %d instructions rewritten to gs, %d unrecognised", rep.fs_patched, rep.fs_unknown);
    logc("kernel: %d imports resolved (%d implemented, %d data exports, %d logging stubs)", rep.thunks.imports,
         rep.thunks.implemented, rep.thunks.data_exports, rep.thunks.stubbed);
    // The full stub list is a paragraph; on the console it drowns everything, so it goes to the log.
    if (rep.thunks.stubbed) logf("kernel: logging stubs for: %s", rep.thunks.stub_names.c_str());
    return true;
}

BootOutcome boot_run_entry(uint32_t entry, uint32_t stack_commit, uint32_t timeout_ms, uint32_t *stack_top)
{
    if (stack_top) *stack_top = 0;

    // The image header says how much stack the game wants. Give it at least that, and never less than a
    // megabyte: the game's deep expressions are its own business, but a thread that runs out mid-frame
    // is indistinguishable from a SEH bug.
    uint32_t stack = stack_commit > (1u << 20) ? stack_commit : (uint32_t)(1u << 20);
    stack += 64 * 1024; // for our own frames between here and the game's entry

    // The boot thread goes through the thread layer rather than pthread directly, so that it has a KTHREAD
    // and a TLS block before its first instruction -- the game's prologue reads fs:[0x20], fs:[0x24] and
    // fs:[0x28] as its first act, and the entry point is entered the Xbox's way: cdecl, no arguments.
    uint32_t handle = 0, tid = 0;
    if (kthread_create(&handle, &tid, stack, 0, entry, 0, 0, false, "boot", true) != KT_SUCCESS)
        return BootOutcome::FailedToStart;
    XboxThread *thread = kthread_from_handle(handle);
    if (!thread) return BootOutcome::FailedToStart;
    g_watched.store(thread);

    // An exit request always wins over "the entry point returned", whichever the loader happens to notice
    // first. The two are observed in the same loop and a request can arrive while the loop is asleep
    // waiting for the thread -- so it is checked at both points, not just one. Otherwise a game that
    // bugchecks on its way out of the entry point is reported as a clean return and its bugcheck code is
    // lost, which is precisely the confusion the exit codes exist to prevent.
    auto end_on_request = [&]() {
        if (!boot_exit_requested()) return false;
        thread->detached = true; // the thread that asked is parked, and must not be joined
        if (stack_top) *stack_top = thread->stack_top;
        logc("boot: the run ended on request (%s)", boot_exit_reason());
        return true;
    };

    if (timeout_ms) {
        // One 10 ms slice at a time rather than a single wait, so the loop can watch for an exit request
        // between slices; a game that never returns must not hold the process past the timeout.
        const uint32_t slice_ms = 10;
        for (uint32_t waited = 0; waited < timeout_ms; waited += slice_ms) {
            if (end_on_request()) return BootOutcome::Exited;
            if (kthread_wait_finished(thread, slice_ms)) {
                if (end_on_request()) return BootOutcome::Exited;
                if (stack_top) *stack_top = thread->stack_top;
                logc("boot: the entry point returned after %u ms", waited);
                return BootOutcome::Returned;
            }
        }
        thread->detached = true;
        if (stack_top) *stack_top = thread->stack_top;
        return BootOutcome::TimedOut;
    }

    // Waiting forever still has to stop if the game asks to leave, and for the same reason as above.
    if (end_on_request()) return BootOutcome::Exited;
    while (!kthread_wait_finished(thread, 10)) {
        if (end_on_request()) return BootOutcome::Exited;
    }
    if (end_on_request()) return BootOutcome::Exited;
    if (stack_top) *stack_top = thread->stack_top;
    return BootOutcome::Returned;
}