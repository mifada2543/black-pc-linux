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
//     --window        open a window and present frames into it, then exit (no XBE needed)
//     --info          parse and print the image, then exit without mapping anything
//     --timeout <s>   seconds before --probe gives up and reports where the boot thread is
//     --trace         every stub call and every detail line on the console as well as in the log
//     --strict        treat an unimplemented kernel call as fatal, instead of logging it
//     --version, --help
//
// What it does not do yet, and says so at boot time: no kernel memory allocation, no TLS slots for the
// boot thread, no DPC/timer thread, no graphics. The first kernel calls the game makes are logged as
// stubs, which is exactly the list that says what to implement next.
#include "d3d/device_posix.h"
#include "kernel/ktable_posix.h"
#include "platform/posix/boot.h"
#include "platform/posix/doctor.h"

#include "platform/posix/log.h"
#include "platform/posix/window.h"
#include "platform/posix/timing.h"
#include "platform/posix/main.h"
#include "platform/posix/options.h"
#include "gfx/gfx_vulkan.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

// Global options instance (set after parsing)
Options *g_options = nullptr;

namespace {

constexpr const char *kVersion = "0.2.0 (linux)";

const Options &get_options() { return *g_options; }
float get_mouse_sens() { return g_options ? g_options->mouse_sens : 1.0f; }
bool get_mouse_invert() { return g_options ? g_options->mouse_invert : false; }

void usage()
{
    fprintf(stderr,
            "blacklinux -- BLACK (Xbox) loader, Linux/i386 port\n"
            "usage: blacklinux [options]\n"
            "  --xbe <path>    XBE to run (default game/default.xbe)\n"
            "  --log <path>    log file (default blacklinux.log)\n"
            "  --hdd <path>    writable folder for \\Device\\Harddisk0 (default $XDG_DATA_HOME/blacklinux)\n"
            "  --doctor        report what this machine can and cannot do, then exit\n"
            "  --probe         enter the entry point, wait --timeout seconds, then report how far it got\n"
            "                  (set BLACKLINUX_TRACE=1 to record which D3D entries it called)\n"
            "  --window        open a window, present --frames frames into it, then exit\n"
            "  --frames <n>    frames to present in --window mode (default 60)\n"
            "  --size <WxH>    window size for --window (default: the back buffer the game asked for)\n"
            "  --info          print the image and exit without mapping it\n"
            "  --timeout <s>   --probe timeout in seconds (default 15)\n"
            "  --trace         detail lines and every stub call on the console too\n"
            "  --strict        an unimplemented kernel call is fatal instead of logged\n"
            "  --version, --help\n");
}

// "1920x1080". Returns false on anything else, having said why. Written out rather than with sscanf
// because sscanf("1920x1080", "%ux%u") also accepts "1920xyz1080", and a size that came out as 1920 and 0
// is a window of nothing.
bool parse_size(const char *text, uint32_t &w, uint32_t &h)
{
    const char *x = strchr(text, 'x');
    if (!x || x == text) {
        fprintf(stderr, "blacklinux: --size wants WxH, e.g. 1280x720 (got '%s')\n", text);
        return false;
    }
    w = (uint32_t)atoi(text);
    h = (uint32_t)atoi(x + 1);
    if (w < 1 || h < 1) {
        fprintf(stderr, "blacklinux: --size wants two numbers above zero (got '%s')\n", text);
        return false;
    }
    return true;
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
        } else if (!strcmp(a, "--doctor")) {
            o.doctor = true;
        } else if (!strcmp(a, "--probe")) {
            o.probe = true;
        } else if (!strcmp(a, "--window")) {
            o.window = true;
        } else if (!strcmp(a, "--frames")) {
            const char *v = need("--frames");
            if (!v) return false;
            o.frames = (uint32_t)atoi(v);
        } else if (!strcmp(a, "--size")) {
            const char *v = need("--size");
            if (!v) return false;
            if (!parse_size(v, o.window_width, o.window_height)) return false;
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

// --window: open a window, present frames into it, exit. This is the whole window path with nothing drawn
// in it but a colour that changes, and it is here because the game image is not on this machine yet: the
// present path is finished long before the D3D8 layer is, and "finished" has to mean more than "compiles".
//
// The size comes from the device state -- the same display_width/display_height that GetDisplayMode
// reports to the game, which is what the game then sizes its back buffer from. Using it here means the
// window is the size the game will draw into rather than a size picked for this mode, and --size overrides
// it for looking at something else.
int run_window(const Options &o)
{
    const uint32_t want_w = o.window_width ? o.window_width : xd3d::state().display_width;
    const uint32_t want_h = o.window_height ? o.window_height : xd3d::state().display_height;
    logc("window: asked for %ux%u, %u frame(s)", want_w, want_h, o.frames);

    if (!platform::window_open(want_w, want_h, "BLACK")) {
        // A refusal here is not a crash and not a bug report: it is a machine with no display, no Vulkan
        // device, or no SDL2, and the sentence names which. Exit 1 because --window was asked for.
        logc("window: %s", platform::window_last_error());
        fprintf(stderr, "blacklinux: %s\n", platform::window_last_error());
        return 1;
    }
    // The surface creator was registered in window_open, now initialise gfx (which creates the surface)
    logc("window: calling gfx::init()...");
    if (!gfx::init()) {
        logc("window: gfx::init failed: %s", gfx::last_error());
        logc("window: gfx::init failed: %s", gfx::last_error());
        platform::window_close();
        return 1;
    }
    logc("window: gfx::init() succeeded");
    // swapchain info will be logged after first frame begins

    // Initialise timing: Xbox 59.94 Hz vblank, 733 MHz counter, CPU pinning.
    if (!platform::timing_init()) {
        logc("window: timing init failed (non-fatal, continuing)");
    } else {
        logc("window: timing initialised (pinned=%d, host TSC %llu Hz)",
             platform::timing_cpu_pinned(), platform::timing_host_tsc_frequency());
    }

    uint32_t presented = 0, skipped = 0;
    for (uint32_t frame = 0; frame < o.frames; frame++) {
        platform::timing_mark_frame_start();
        if (!platform::window_begin_frame()) {
            // A frame with no drawable is a minimised window or a surface that went away, not a failure:
            // it is counted and the loop carries on, which is what the real game loop does.
            skipped++;
            logf("[window] frame %u skipped: %s\n", frame, platform::window_last_error());
            SDL_Delay(16);
            continue;
        }
        if (presented == 0) {
            logc("window: swapchain %ux%u, %s, %u image(s)",
                 gfx::swapchain_width(), gfx::swapchain_height(),
                 gfx::swapchain_present_mode_name(), gfx::swapchain_image_count());
        }
        // A colour that changes every frame, so a person watching the window can see it is alive and can
        // see it is *this* program presenting. Not a gradient and not a pattern: a pattern would need a
        // draw, and a draw on 32-bit llvmpipe produces no fragments (measured), which would make the
        // window look broken for a reason that has nothing to do with the present path.
        const float t = o.frames > 1 ? (float)frame / (float)(o.frames - 1) : 1.0f;
        const float colour[4] = {t, 0.25f, 1.0f - t, 1.0f};
        gfx::clear_color(colour);
        if (platform::window_end_frame()) presented++;
        else {
            skipped++;
            logf("[window] frame %u not presented: %s\n", frame, platform::window_last_error());
        }
        // Wait for vblank: Xbox 59.94 Hz pacing. This replaces the crude SDL_Delay(16).
        const uint32_t waited = platform::timing_wait_vblank();
        (void)waited;
    }

    logc("window: %u frame(s) presented, %u skipped", presented, skipped);
    platform::timing_shutdown();
    platform::window_close();
    return presented ? 0 : 1;
}

int run(const Options &o)
{
    log_init(o.log.c_str(), o.trace);
    log_set_trace(o.trace);
    kernel_set_trace(o.trace);

    // --doctor runs before anything else, and deliberately. A machine with no usable Vulkan device still has
    // to be able to say so, and a report that dies at the first missing thing cannot: it would report the
    // first failure and never the second, which is exactly what a reader needs to see.
    if (o.doctor) return xd3d::run_doctor(o.xbe.c_str(), o.hdd.c_str());

    // --window next, and for the same reason: it is the one mode that needs no image at all, so it has to be
    // runnable on a machine that has none.
    if (o.window) return run_window(o);

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

    // --probe is a report as well as a run: after the timeout it says, by name, how far the game got. It
    // is printed whether or not the run ended in a stop, because "it got all the way through D3D init and
    // then sat in its own loop" and "it died on the fourth call" are both answers someone needs.
    if (o.probe) boot_print_probe_report(rep);

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
    options_set_instance(&o);
    return run(o);
}