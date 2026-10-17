#include "platform/posix/boot.h"

#include "kernel/k_memory_posix.h"
#include "kernel/k_file_posix.h"
#include "kernel/k_misc_posix.h"
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

struct BootThread {
    uint32_t entry = 0;
    uint32_t stack_top = 0;
};

// The game owns this thread from here on. It is entered exactly as the Windows port enters it: at the
// XBE's entry point, with nothing of ours on the stack, because the game's own prologue has to find
// its TEB fields and install its SEH frame against the stack it makes for itself.
void *boot_thread(void *arg)
{
    BootThread *b = (BootThread *)arg;
#if defined(__GLIBC__)
    pthread_setname_np(pthread_self(), "boot");
#endif
    uint32_t marker = 0;
    b->stack_top = (uint32_t)(uintptr_t)&marker;
    ((void (*)(void))(uintptr_t)b->entry)();
    return nullptr; // game entry points do not return; if this one did, so be it
}

std::atomic<int> g_exit_code{-1}; // negative = no exit requested
const char *g_exit_why = "unspecified";

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

bool boot_prepare(const std::string &path, XbePosix &x, XbeImage &img, BootReport &rep, std::string &err)
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
        kfile_init(game_dir.empty() ? std::string(".") : game_dir, boot_default_hdd_dir());
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

    BootThread boot;
    boot.entry = entry;

    // The image header says how much stack the game wants. Give it at least that, and never less than a
    // megabyte: the game's deep expressions are its own business, but a thread that runs out mid-frame
    // is indistinguishable from a SEH bug.
    size_t stack = stack_commit > (1u << 20) ? stack_commit : (size_t)(1u << 20);
    stack += 64 * 1024; // for our own frames between here and the game's entry

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);

    pthread_t thread;
    int rc = pthread_create(&thread, &attr, boot_thread, &boot);
    pthread_attr_destroy(&attr);
    if (rc != 0) return BootOutcome::FailedToStart;

    if (timeout_ms) {
        // Poll for completion rather than join, so a game that never returns does not hold the process.
        // pthread_tryjoin_np is the glibc call that answers "has it finished yet" without blocking; the
        // loop is the portable shape around it, one 10 ms slice at a time.
        const uint32_t slice_ms = 10;
        for (uint32_t waited = 0; waited < timeout_ms; waited += slice_ms) {
            void *ret = nullptr;
            if (pthread_tryjoin_np(thread, &ret) == 0) {
                if (stack_top) *stack_top = boot.stack_top;
                logc("boot: the entry point returned after %u ms", waited);
                return BootOutcome::Returned;
            }
            // A kernel call asked to end the run. The thread that asked is parked and will not finish, so
            // it is detached rather than joined, the same as a game that never returns.
            if (boot_exit_requested()) {
                pthread_detach(thread);
                if (stack_top) *stack_top = boot.stack_top;
                logc("boot: the run ended on request (%s)", boot_exit_reason());
                return BootOutcome::Exited;
            }
            struct timespec ts = {0, (long)slice_ms * 1000 * 1000};
            nanosleep(&ts, nullptr);
        }
        pthread_detach(thread);
        if (stack_top) *stack_top = boot.stack_top;
        return BootOutcome::TimedOut;
    }

    // Waiting forever still has to stop if the game asks to leave.
    if (boot_exit_requested()) {
        pthread_detach(thread);
        if (stack_top) *stack_top = boot.stack_top;
        logc("boot: the run ended on request (%s)", boot_exit_reason());
        return BootOutcome::Exited;
    }
    pthread_join(thread, nullptr);
    if (stack_top) *stack_top = boot.stack_top;
    return BootOutcome::Returned;
}