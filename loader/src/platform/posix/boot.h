// The Phase 1 boot sequence, in the order the Windows loader does it, for the parts that exist so far.
//
// main.cpp is a thin argument parser around this file, which is deliberate: the sequence is the part
// with all the failure modes, so it has to be reachable from a test. test_boot.cpp runs it against a
// synthetic image; the real default.xbe runs the same code from the command line.
//
// What the Windows loader does here, and what this port has so far:
//
//   settings_load          not yet -- black.ini comes with the settings port
//   xbe.load               done (xbe.cpp)
//   kmem_init              done (k_memory_posix.cpp; the contiguous block was already mapped by image.cpp)
//   xthread_init_slots     NOT YET -- TLS slots for the boot thread
//   image_map              done (image.cpp)
//   patch_fs_accesses      done (fs_patch.cpp, via XbeImage::patch_fs)
//   kernel_resolve_thunks  done (ktable_posix.cpp)
//   kfile_init             done (k_file_posix.cpp over xfile.cpp): the mount table is built from the
//                          folder the XBE came from plus the writable hard-disk folder
//   install_crash_handlers done (seh.cpp: seh_install_signal_bridge)
//   install_probes         not needed -- those patch Windows-only call sites
//   install_d3d_hooks      NOT YET (Vulkan phase)
//   kdpc_init              NOT YET
//   kthread_create         done here: a real thread that enters the image's entry point
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "kernel/ktable_posix.h"
#include "platform/posix/image.h"
#include "platform/posix/xbe.h"

// Everything worth printing about an image, gathered once so the report and the tests see the same
// numbers rather than two formats of them.
struct BootReport {
    // From the header.
    std::string path;
    std::string kind; // retail / debug / sega: which XOR key decoded the entry point
    std::string title;
    uint32_t title_id = 0;
    uint32_t base = 0, size_of_headers = 0, size_of_image = 0;
    uint32_t entry = 0, thunk_addr = 0, pe_base = 0;
    uint32_t stack_commit = 0, heap_reserve = 0, heap_commit = 0;
    uint32_t timedate = 0;
    std::string debug_path;

    bool has_tls = false;
    XbeTlsInfo tls{};

    struct Section {
        std::string name;
        uint32_t vaddr = 0, vsize = 0, raw_size = 0, flags = 0;
        bool executable = false, writable = false, preloaded = false;
    };
    std::vector<Section> sections;

    std::vector<uint32_t> ordinals; // kernel imports in thunk-table order

    // What the loader did about it.
    int fs_patched = 0;
    int fs_unknown = 0;
    int fs_no_disp = 0;
    KernelThunkStats thunks;

    // What the memory layer ended up with.
    uint32_t mem_contiguous_base = 0;
    uint32_t mem_contiguous_free = 0;
    uint32_t mem_virtual_regions = 0;
};

// Parse only. Fills `rep` and prints the header/section/import summary through logc. Safe to call on
// any file: it never maps anything, so it is the right thing for --info.
bool boot_inspect(const std::string &path, XbePosix &x, BootReport &rep, std::string &err);

// Parse, map, copy the sections, patch fs->gs, and fill the kernel import table. `img` stays mapped so
// the caller can execute from it. Prints each step through logc.
//
// `hdd` is the writable folder for \Device\Harddisk0; an empty string means boot_default_hdd_dir(). It
// is a parameter rather than a global because the option was parsed and then ignored, which is the kind
// of bug that only shows when someone actually passes the flag.
bool boot_prepare(const std::string &path, XbePosix &x, XbeImage &img, BootReport &rep, std::string &err,
                  const std::string &hdd = std::string());

// Where \Device\Harddisk0 should live when the command line did not say. $XDG_DATA_HOME/blacklinux, or
// ~/.local/share/blacklinux; created if it is not there. The game folder is the DVD and stays read-only,
// which is why the writable console data goes somewhere else.
std::string boot_default_hdd_dir();

// Enter the image's entry point on a real thread and wait. The entry point is game code that may never
// return, so this returns when the thread returns, when `timeout_ms` expires (0 = wait forever), or when
// a kernel call asked to end the run. `stack_top` is the boot thread's stack pointer at entry, which is
// what a crashed game frame points back into.
enum class BootOutcome { Returned, TimedOut, Exited, FailedToStart };
BootOutcome boot_run_entry(uint32_t entry, uint32_t stack_commit, uint32_t timeout_ms, uint32_t *stack_top);

// End the run from inside a kernel call. On the console HalReturnToFirmware and KeBugCheck end the
// session; here they set this request and park the calling thread, and boot_run_entry notices it and
// returns Exited. `why` must be a string literal: it is stored by pointer and printed later.
//
// The codes are the ones the exit path already uses elsewhere in the port: 0 normal, 1 a fatal error the
// game reported itself, 4 a bugcheck.
void boot_request_exit(int code, const char *why);
bool boot_exit_requested();
int boot_exit_code();
const char *boot_exit_reason();

// The stdout dump --info and --probe share. Written to stderr so it interleaves with the log lines in a
// terminal and stays next to them in a redirect.
void boot_print_report(const BootReport &rep);