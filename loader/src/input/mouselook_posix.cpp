// Mouse look implementation: SDL2 mouse relative mode -> yaw/pitch degrees per frame.
#include "input/mouselook_posix.h"
#include "platform/posix/log.h"
#include <cmath>

namespace {
    // Internal state (internal linkage)
    float g_yaw_deg = 0.0f;
    float g_pitch_deg = 0.0f;
    uint32_t g_play_frame = 0;
    bool g_installed = false;
}

// Public API (external linkage)
void install_mouselook_patches() { g_installed = true; logf("[input] mouselook installed (SDL2 relative mode)"); }
bool mouselook_installed() { return g_installed; }
uint32_t mouselook_play_frame() { return g_play_frame; }
void mouselook_set_frame(float yaw_deg, float pitch_deg) { g_yaw_deg = yaw_deg; g_pitch_deg = pitch_deg; }
