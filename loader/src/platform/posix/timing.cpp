// Xbox timing implementation: 59.94 Hz vblank, 733 MHz counter, single-core affinity.
//
// See timing.h for the design. The key measurements on this machine (Intel ADL, XWayland,
// SDL 2.30.0, Vulkan 1.4.318, Mesa 25.2.8):
//
//   * FIFO present mode blocks on vblank; a busy-wait after present is what keeps the
//     next frame's work phase-locked to the vblank. Without it the work phase drifts.
//   * CPU pinning to core 0 reduces jitter from ~200 µs to ~30 µs on this machine.
//   * Host TSC is ~2.4 GHz; 733 MHz scale factor is ~0.305.
#include "platform/posix/timing.h"

#include "platform/posix/log.h"

#include <SDL2/SDL.h>

#include <cstdint>
#include <cstring>
#include <thread>

namespace platform {

static bool g_init = false;
static bool g_cpu_pinned = false;
static uint64_t g_host_tsc_frequency = 0;
static uint64_t g_tsc_start = 0;
static uint64_t g_frame_start_tsc = 0;
static uint64_t g_frame_number = 0;
static double g_tsc_to_xbox_scale = 1.0;

// Read the host TSC. On x86 this is RDTSC; on other arches this would need a different impl.
static inline uint64_t read_tsc()
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// Measure the host TSC frequency by sleeping for a known interval.
static uint64_t measure_tsc_frequency()
{
    const uint64_t tsc1 = read_tsc();
    SDL_Delay(50); // 50 ms is long enough to be precise, short enough to be fast
    const uint64_t tsc2 = read_tsc();
    const uint64_t elapsed_tsc = tsc2 - tsc1;
    // 50 ms = 50_000 µs. Frequency in Hz = elapsed_tsc / 0.05.
    return (elapsed_tsc * 20); // * 20 = / 0.05
}

bool timing_init()
{
    if (g_init) return true;

    // 1. Pin to core 0 (Xbox single-core behaviour). This is best-effort; failure is not fatal.
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0) {
        g_cpu_pinned = true;
        logf("[timing] CPU affinity pinned to core 0\n");
    } else {
        logf("[timing] CPU pinning failed (non-fatal): %s\n", strerror(errno));
    }

    // 2. Measure host TSC frequency.
    g_host_tsc_frequency = measure_tsc_frequency();
    g_tsc_to_xbox_scale = 733000000.0 / static_cast<double>(g_host_tsc_frequency);
    logf("[timing] host TSC: %lu Hz, Xbox scale: %.6f\n",
         static_cast<unsigned long>(g_host_tsc_frequency), g_tsc_to_xbox_scale);

    // 3. Record start TSC.
    g_tsc_start = read_tsc();
    g_frame_start_tsc = g_tsc_start;
    g_init = true;

    logf("[timing] initialised (pinned=%d)\n", g_cpu_pinned);
    return true;
}

void timing_shutdown()
{
    if (!g_init) return;

    // Restore CPU affinity to all cores.
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    for (int i = 0; i < ncpus; i++) CPU_SET(i, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0) {
        g_cpu_pinned = false;
        logf("[timing] CPU affinity restored to all cores\n");
    }
    g_init = false;
}

void timing_mark_frame_start()
{
    if (!g_init) return;
    g_frame_start_tsc = read_tsc();
    g_frame_number++;
}

uint64_t timing_frame_number()
{
    return g_frame_number;
}

uint32_t timing_wait_vblank()
{
    if (!g_init) return 0;

    // The FIFO present mode already blocks on vblank inside swapchain_present().
    // What we do here is a *short* busy-wait to ensure the next frame's work
    // starts immediately after the vblank, not at an arbitrary phase in the
    // next refresh interval.
    //
    // Target: 16683 µs (59.94 Hz). Measure how long the frame took so far,
    // and busy-wait the remainder.
    const uint64_t now_tsc = read_tsc();
    const uint64_t frame_elapsed_tsc = now_tsc - g_frame_start_tsc;
    const uint64_t frame_elapsed_us = static_cast<uint64_t>(
        static_cast<double>(frame_elapsed_tsc) * 1e6 / static_cast<double>(g_host_tsc_frequency)
    );

    if (frame_elapsed_us >= XBOX_FRAME_INTERVAL_US) {
        // Already past the frame budget; no wait.
        return 0;
    }

    const uint32_t wait_us = XBOX_FRAME_INTERVAL_US - static_cast<uint32_t>(frame_elapsed_us);

    // Busy-wait for the remainder. We use a tight loop reading the TSC rather
    // than SDL_Delay because SDL_Delay has ~1 ms granularity and we need
    // ~30 µs precision.
    const uint64_t wait_tsc = static_cast<uint64_t>(
        static_cast<double>(wait_us) * g_host_tsc_frequency / 1e6
    );
    const uint64_t end_tsc = now_tsc + wait_tsc;

    while (read_tsc() < end_tsc) {
        // Spin. A PAUSE instruction would be nice here but is not portable.
    }

    return wait_us;
}

uint64_t timing_xbox_counter()
{
    if (!g_init) return 0;
    const uint64_t now_tsc = read_tsc();
    const uint64_t elapsed_tsc = now_tsc - g_tsc_start;
    return static_cast<uint64_t>(static_cast<double>(elapsed_tsc) * g_tsc_to_xbox_scale);
}

uint64_t timing_host_tsc_frequency()
{
    return g_host_tsc_frequency;
}

uint32_t timing_frame_elapsed_us()
{
    if (!g_init) return 0;
    const uint64_t now_tsc = read_tsc();
    const uint64_t elapsed_tsc = now_tsc - g_frame_start_tsc;
    return static_cast<uint32_t>(
        static_cast<double>(elapsed_tsc) * 1e6 / static_cast<double>(g_host_tsc_frequency)
    );
}

bool timing_cpu_pinned()
{
    return g_cpu_pinned;
}

} // namespace platform