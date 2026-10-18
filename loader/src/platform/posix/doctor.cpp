// `--doctor`: one page that says what this machine can and cannot do, for a bug report.
//
// The point is not to be pretty. It is that a report of "it does not work" says nothing, and every question
// that report then has to ask -- is the process really 32-bit, which GPU did it pick and why not the other
// one, is the image present -- is a question this program can answer by itself. So the answer belongs in the
// program rather than in the reply.
//
// The last section is the one that matters most and the one that is hardest to get from anywhere else: what
// is *not* built yet, and what each missing piece blocks. A reader who sees that the FVF constants are
// unreadable because the image is absent understands the situation in a way no error message conveys.
#include "platform/posix/doctor.h"

#include "gfx/gfx_vulkan.h"
#include "platform/posix/log.h"
#include "platform/posix/xbe.h"

#include <stdio.h>
#include <string.h>

#include <string>

namespace xd3d {
namespace {

const char *device_type_name(int t)
{
    // VkPhysicalDeviceType as ints, so this file needs no Vulkan headers. The values are the enum's, and
    // they have been since Vulkan 1.0; what the enum *is* comes from gfx's header, which does include it.
    switch (t) {
    case 0: return "other";
    case 1: return "integrated";
    case 2: return "discrete";
    case 3: return "virtual";
    case 4: return "cpu";
    default: return "?";
    }
}

const char *version_string(uint32_t v)
{
    static char buf[32];
    snprintf(buf, sizeof buf, "%u.%u.%u", (v >> 22) & 0x7F, (v >> 12) & 0x3FF, v & 0xFFF);
    return buf;
}

bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

void section(const char *title)
{
    printf("\n== %s ", title);
    for (int i = 0; i < (int)(60 - strlen(title)); i++) putchar('-');
    putchar('\n');
}

} // namespace

int run_doctor(const char *xbe_path, const char *hdd_path)
{
    printf("BLACK loader -- doctor\n");
    printf("Everything below is measured, not assumed. Please paste the whole of it into a bug report;\n");
    printf("a report that starts at \"it does not work\" has to be answered with questions this avoids.\n");

    // ---- the process ----
    section("process");
    // The one fact that decides everything else. A 64-bit loader is a different program with different
    // bugs, and the port is i386 by definition: every thunk, every stack frame, every address in the game's
    // own headers assumes a 32-bit world.
    printf("  pointer size      %zu bytes (%s)\n", sizeof(void *),
           sizeof(void *) == 4 ? "32-bit, as required" : "NOT 32-BIT -- the port is defined as i386");
    printf("  long size         %zu bytes\n", sizeof(long));
    {
        char host[256] = "?";
        if (FILE *f = fopen("/proc/sys/kernel/hostname", "r")) {
            if (fgets(host, sizeof host, f)) {
                host[strcspn(host, "\r\n")] = 0;
            }
            fclose(f);
        }
        printf("  host              %s\n", host);
    }
    {
        char rel[256] = "?";
        if (FILE *f = fopen("/etc/os-release", "r")) {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                if (!strncmp(line, "PRETTY_NAME=", 12)) {
                    snprintf(rel, sizeof rel, "%s", line + 12);
                    rel[strcspn(rel, "\r\n")] = 0;
                    break;
                }
            }
            fclose(f);
        }
        printf("  distribution      %s\n", rel);
    }

    // ---- the image ----
    section("game image");
    printf("  path              %s\n", xbe_path ? xbe_path : "(none given)");
    if (!xbe_path || !file_exists(xbe_path)) {
        // Said plainly, because "the game does not start" and "the game is not there" look identical from
        // the outside, and this port cannot run one word of the game without it.
        printf("  status            ABSENT -- nothing else in this report depends on it, and nothing the\n");
        printf("                    loader does depends on it yet either. Track B needs it.\n");
    } else {
        XbePosix xbe;
        std::string err;
        if (!xbe.load(xbe_path, err)) {
            printf("  status            PRESENT but unreadable: %s\n", err.c_str());
        } else {
            printf("  status            present\n");
            printf("  title             %s\n", xbe.title.c_str());
            printf("  kind              %s\n", xbe.kind.c_str());
            printf("  image base        0x%08x, size 0x%x\n", xbe.base, xbe.size_of_image);
            printf("  sections          %u\n", xbe.num_sections);
            // The facts below are the ones the per-build tables were written against. If any of them differs
            // from what the tables assume, the tables are wrong and no amount of debugging the loader will
            // say so.
            printf("  entry point       0x%08x\n", xbe.entry);
        }
    }
    if (hdd_path && hdd_path[0])
        printf("  writable hdd      %s (%s)\n", hdd_path, file_exists(hdd_path) ? "present" : "not created yet");
    else
        printf("  writable hdd      (none given; the loader will use XDG_DATA_HOME)\n");

    // ---- the graphics device ----
    section("vulkan");
    printf("  initialising, which probes every device with a submit...\n");
    fflush(stdout);
    const bool ok = gfx::init();
    if (!ok) {
        printf("  status            NO USABLE DEVICE: %s\n", gfx::last_error());
        printf("\n  Nothing that draws can run without one. See tools/check-env.sh, which says which\n");
        printf("  package is missing.\n");
        return 1;
    }

    const uint32_t n = gfx::adapter_count();
    printf("  devices found     %u\n", n);
    for (uint32_t i = 0; i < n; i++) {
        const gfx::AdapterInfo &a = gfx::adapter_at(i);
        const std::string why = gfx::vulkan_skip_reason(i);
        printf("    [%u] %s\n", i, a.name.c_str());
        printf("        type %s, API %s, driver 0x%x, ids %04x:%04x%s\n", device_type_name(a.device_type),
               version_string(a.api_version), a.driver_version, a.vendor_id, a.device_id,
               i == (uint32_t)gfx::adapter_index() ? "   <- chosen" : "");
        if (!why.empty()) printf("        SKIPPED: %s\n", why.c_str());
    }
    const gfx::AdapterInfo &chosen = gfx::adapter();
    printf("  chosen            %s (%s)\n", chosen.name.c_str(), device_type_name(chosen.device_type));
    printf("  dynamic rendering %s\n", gfx::has_device_extension("VK_KHR_dynamic_rendering") ? "yes"
                                                                                                  : "no -- the loader draws with it and will refuse");

    // ---- what is not built yet ----
    section("not built yet");
    // Each line is a thing a bug report would otherwise be blamed on. "The game draws nothing" is not a bug
    // in the loader while the draw path stops at choosing a pipeline, and the only honest way to say that is
    // to say it in the program.
    printf("  These are unfinished on purpose or blocked, not broken. Each blocks the next line of work.\n\n");
    if (!xbe_path || !file_exists(xbe_path))
        printf("  - FVF constants, D3DMATERIAL9/D3DLIGHT9/D3DRECT layouts: unreadable without the image.\n"
               "    They are NOT transcribed from memory, because a wrong value here produces a pipeline\n"
               "    that accepts everything and never says so. This is why no draw reaches the GPU yet.\n");
    else
        printf("  - FVF constants and the D3D9-era struct layouts: the image is present but nothing reads\n"
               "    them out of it yet.\n");
    printf("  - 3 of 50 device HLE rows need a Vulkan device at call time (CreateDevice, Clear, Swap).\n");
    printf("  - 4 of 50 device HLE rows need the struct layouts above.\n");
    printf("  - A0, the gate that says a real GPU can carry this port: open. See NOTES.md for the\n");
    printf("    measurement, which is a crash inside the 32-bit Intel driver at vkQueueSubmit and which\n");
    printf("    reproduces in a ten-line program, so it is not this port.\n");
    printf("  - A9, the audio decoder: waiting on a licence decision (WMA and XMV are separately\n");
    printf("    licensed; nothing here decodes either until that is settled).\n");

    // ---- how to reproduce ----
    section("for a bug report");
    printf("  blacklinux --doctor                 this page\n");
    printf("  blacklinux --probe --timeout 30     how far the game got, by name\n");
    printf("  BLACKLINUX_TRACE=1 blacklinux --probe   which D3D entries it called\n");
    printf("  blacklinux --probe --strict         an unimplemented kernel call is fatal\n");
    printf("  tools/check-env.sh                  what is installed and what to install\n");

    gfx::shutdown();
    return 0;
}

} // namespace xd3d