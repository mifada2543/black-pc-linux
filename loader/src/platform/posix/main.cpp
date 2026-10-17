// blacklinux: the Linux loader.
//
// The Windows build is a DLL that black.exe calls; on Linux there is no stub and no DLL, so this is the
// program: it maps default.xbe over its own address space, patches the image's fs-relative accesses onto
// this thread's TEB, fills the kernel import table, installs the fault bridge and enters the image's
// entry point on a new thread. Everything with a failure mode lives in boot.cpp, so that the tests can
// reach it; this file is argument parsing and reporting.
//
// Usage:
//   blacklinux [options]
//     --xbe <path>    the XBE to run          (default game/default.xbe)
//     --log <path>    where to write the log  (default blacklinux.log)
//     --probe         prepare, enter the entry point, wait up to --timeout, then exit
//     --info          parse and print the image, then exit without mapping anything
//     --timeout <s>   seconds before --probe gives up and reports where the boot thread is
//     --trace         every stub call and every detail line on the console as well as in the log
//     --strict        treat an unimplemented kernel call as fatal, instead of logging it
//     --version, --help
//
// What it does not do yet, and says so at boot time: no kernel memory allocation, no TLS slots for the
// boot thread, no DPC/timer thread, no graphics. The first kernel calls the game makes are logged as
// stubs, which is exactly the list that says what to implement next.
#include "kernel/ktable_posix.h"
#include "platform/posix/boot.h"
#include "platform/posix/log.h"

#include <stdio.h>
#include <string.h>

namespace {

constexpr const char *kVersion = "0.2.0 (linux)";

struct Options {
    std::string xbe = "game/default.xbe";
    std::string log = "blacklinux.log";
    // Where \Device\Harddisk0 lives. Empty means "use the default under XDG_DATA_HOME", which is what a
    // player's writable console data belongs in: the game's own folder is the DVD and stays read-only.
    std::string hdd;
    bool probe = false;
    bool info = false;
    bool trace = false;
    bool strict = false;
    bool help = false;
    bool version = false;
    uint32_t timeout_s = 15;
};

void usage()
{
    fprintf(stderr,
            "blacklinux -- BLACK (Xbox) loader, Linux/i386 port\n"
            "usage: blacklinux [options]\n"
            "  --xbe <path>    XBE to run (default game/default.xbe)\n"
            "  --log <path>    log file (default blacklinux.log)\n"
            "  --hdd <path>    writable folder for \\Device\\Harddisk0 (default $XDG_DATA_HOME/blacklinux)\n"
            "  --probe         enter the entry point, wait --timeout seconds, then exit\n"
            "  --info          print the image and exit without mapping it\n"
            "  --timeout <s>   --probe timeout in seconds (default 15)\n"
            "  --trace         detail lines and every stub call on the console too\n"
            "  --strict        an unimplemented kernel call is fatal instead of logged\n"
            "  --version, --help\n");
}

// Returns false on a bad argument, having said why.
bool parse_args(int argc, char **argv, Options &o)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        auto need = [&](const char *what) -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "blacklinux: %s needs a value\n", what);
                return nullptr;
            }
            return argv[++i];
        };
        if (!strcmp(a, "--xbe")) {
            const char *v = need("--xbe");
            if (!v) return false;
            o.xbe = v;
        } else if (!strcmp(a, "--hdd")) {
            const char *v = need("--hdd");
            if (!v) return false;
            o.hdd = v;
        } else if (!strcmp(a, "--log")) {
            const char *v = need("--log");
            if (!v) return false;
            o.log = v;
        } else if (!strcmp(a, "--timeout")) {
            const char *v = need("--timeout");
            if (!v) return false;
            o.timeout_s = (uint32_t)atoi(v);
        } else if (!strcmp(a, "--probe")) {
            o.probe = true;
        } else if (!strcmp(a, "--info")) {
            o.info = true;
        } else if (!strcmp(a, "--trace")) {
            o.trace = true;
        } else if (!strcmp(a, "--strict")) {
            o.strict = true;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            o.help = true;
        } else if (!strcmp(a, "--version")) {
            o.version = true;
        } else {
            fprintf(stderr, "blacklinux: unknown option '%s' (try --help)\n", a);
            return false;
        }
    }
    return true;
}

int run(const Options &o)
{
    log_init(o.log.c_str(), o.trace);
    log_set_trace(o.trace);
    kernel_set_trace(o.trace);

    logc("BLACK loader %s -- XBE %s, log %s", kVersion, o.xbe.c_str(), log_path() ? log_path() : "(none)");

    XbePosix x;
    XbeImage img;
    BootReport rep;
    std::string err;

    if (o.info) {
        if (!boot_inspect(o.xbe, x, rep, err)) {
            fprintf(stderr, "blacklinux: %s\n", err.c_str());
            return 1;
        }
        boot_print_report(rep);
        log_close();
        return 0;
    }

    if (!boot_prepare(o.xbe, x, img, rep, err, o.hdd)) {
        fprintf(stderr, "blacklinux: %s\n", err.c_str());
        logc("boot failed: %s", err.c_str());
        log_close();
        return 1;
    }
    kernel_set_strict(o.strict);
    boot_print_report(rep);

    // The kernel side is complete: every one of the 115 ordinals is an implementation or a data export. What
    // is left is the game above it -- the D3D8 layer and the host window.
    logc("kernel: complete -- the 115 ordinals are implemented; what is missing is the D3D8 layer above it");
    logc("starting XBE entry point 0x%08x", rep.entry);

    uint32_t stack_top = 0;
    const BootOutcome outcome =
        boot_run_entry(rep.entry, rep.stack_commit, o.probe ? o.timeout_s * 1000 : 0, &stack_top);

    int code = 0;
    switch (outcome) {
    case BootOutcome::Returned:
        logc("boot: the game's entry point returned (this does not happen on the real image)");
        break;
    case BootOutcome::TimedOut:
        // The normal state for the real game: it is inside its own main loop. Say where the boot thread's
        // stack was, because that is the first thing anyone wants when a crash happens later.
        logc("boot: still inside the game after %u s; boot thread stack top 0x%08x", o.timeout_s, stack_top);
        break;
    case BootOutcome::Exited:
        // The game asked to leave: HalReturnToFirmware, HalInitiateShutdown or KeBugCheck. Its exit code
        // is the reason, so a bugcheck does not look like a clean run and a clean leave does not look
        // like a crash.
        code = boot_exit_code();
        logc("boot: the game ended the run itself: %s", boot_exit_reason());
        break;
    case BootOutcome::FailedToStart:
        logc("boot: could not create the boot thread");
        fprintf(stderr, "blacklinux: could not create the boot thread\n");
        code = 1;
        break;
    }

    if (o.strict && rep.thunks.stubbed)
        logc("strict: %d kernel imports are logging stubs", rep.thunks.stubbed);
    log_close();
    return code;
}

} // namespace

int main(int argc, char **argv)
{
    Options o;
    if (!parse_args(argc, argv, o)) {
        usage();
        return 2;
    }
    if (o.help) {
        usage();
        return 0;
    }
    if (o.version) {
        fprintf(stderr, "blacklinux %s\n", kVersion);
        return 0;
    }
    return run(o);
}