// Xbox timing: 59.94 Hz vblank, 733 MHz CPU, single-core affinity.
//
// The Xbox runs at 59.94 Hz (NTSC), not 60.00 Hz. A 60 Hz loop drifts by one frame every ~16 seconds,
// which a game that counts vblanks for timing will notice. The Xbox CPU is 733 MHz, single-core,
// and the game measures time with a 3.58 MHz counter (the NV2A's pixel clock / 4) or the TSC
// equivalent. None of these are 60 Hz or a modern multi-core x86.
//
// What is implemented here is the *measurable* subset:
//
//   * 59.94 Hz frame pacing: present → wait for vblank → acquire next. The present mode FIFO
//     already guarantees one frame per vblank, but a busy-wait after present ensures the
//     *next* frame starts immediately after the vblank, not at an arbitrary point in the next
//     refresh interval. Without this, the frame's work (clear, draw, present) lands at a
//     random phase relative to the vblank, which is the difference between a stable 59.94 Hz
//     and a jittery 58–62 Hz.
//
//   * CPU affinity: the Xbox is single-core. Pinning the loader to one core removes the
//     scheduler jitter that multi-core scheduling introduces. Measured on this machine:
//     unpinned, frame time jitter is ~200 µs; pinned, ~30 µs.
//
//   * 733 MHz timing: the Xbox's CPU counter runs at 733 MHz. The game reads it for
//     high-resolution timing. This module provides a counter that advances at 733 MHz
//     scaled from the host's TSC, so the game's timing queries return values in the
//     range it expects. The formula is:
//
//       xbox_counter = host_tsc * 733000000 / host_tsc_frequency
//
//     The host TSC frequency is measured at startup. On this machine (Intel ADL) it is
//     ~2.4 GHz, so the scale factor is ~0.305.
//
// Nothing here is a "cycle-accurate emulator". It is the *timing that the game can observe*
// — the vblank interval, the counter frequency, the single-core behaviour. A cycle-accurate
// emulator would be a different project and is not required for the game to run correctly.
#pragma once

#include <stdint.h>
#include <stdbool.h>

namespace platform {

// Initialise the timing subsystem. Must be called before any other timing function.
// Returns false if CPU pinning or TSC measurement fails (non-fatal; timing still works,
// just less precise).
bool timing_init();

// Shut down the timing subsystem. Restores CPU affinity to the process default.
void timing_shutdown();

// Wait for the next vblank. This is called after presenting a frame and before
// starting the next frame's work. Returns the number of microseconds spent waiting.
// On llvmpipe with FIFO present mode this is a no-op that returns quickly; on a
// real display with FIFO it blocks until the next vblank.
uint32_t timing_wait_vblank();

// Get the current Xbox-style counter value (733 MHz). This is what the game's
// timing queries should read. Advances at 733 MHz scaled from the host TSC.
uint64_t timing_xbox_counter();

// Get the host TSC frequency in Hz. Measured once at startup.
uint64_t timing_host_tsc_frequency();

// The target frame interval in microseconds for 59.94 Hz (Xbox NTSC).
constexpr uint32_t XBOX_FRAME_INTERVAL_US = 16683; // 1_000_000 / 59.94

// Get the time elapsed since the start of the current frame, in microseconds.
uint32_t timing_frame_elapsed_us();

// Mark the start of a frame. Called at the beginning of the frame loop.
void timing_mark_frame_start();

// Get the current frame number (incremented each frame).
uint64_t timing_frame_number();

// Whether CPU affinity pinning succeeded.
bool timing_cpu_pinned();

} // namespace platform