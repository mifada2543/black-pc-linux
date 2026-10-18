// Mouse look implementation: SDL2 mouse relative mode -> yaw/pitch degrees per frame.
#include "input/mouselook_posix.h"
#include "d3d/device_posix.h"
#include "platform/posix/log.h"
#include <cmath>

namespace {
    // Internal state (internal linkage)
    float g_yaw_deg = 0.0f;
    float g_pitch_deg = 0.0f;
    // The frame of the last player update, 0 before the first. No player update stamps it here -- see
    // mouselook_set_frame() for why, and for what the value stands in for meanwhile.
    uint32_t g_play_frame = 0;
    bool g_installed = false;
}

// Public API (external linkage)
void install_mouselook_patches() { g_installed = true; logf("[input] mouselook installed (SDL2 relative mode)"); }
bool mouselook_installed() { return g_installed; }
uint32_t mouselook_play_frame() { return g_play_frame; }
void mouselook_set_frame(float yaw_deg, float pitch_deg)
{
    g_yaw_deg = yaw_deg;
    g_pitch_deg = pitch_deg;

    // g_play_frame is stamped here, and it is a proxy: the pad poll stands in for the player update. The
    // authoritative stamp is the player's view update (on Windows the installed patch writes it from
    // ml_sway, which menus, pauses and cutscenes do not run), and that hook is not built in this port, so
    // nothing wrote this value at all: mouselook_play_frame() answered 0 forever and in_play() in
    // input/bindings_posix.cpp was false from the first frame on. The pad poll is the only per-frame event
    // this port has, so it records xd3d::frame_count() instead. in_play() then means "the pad was polled
    // during the current frame", which is also true in menus, pauses and cutscenes, so this does not yet
    // tell play from them and apply() still takes the menu branch while playing. Two limits are left open
    // rather than guessed away: the frame compared against is xd3d::frame_count(), and
    // xd3d::frame_count_increment() has no caller yet, so that counter is still 0 and in_play() stays
    // false; and when the player-update hook is built, this stamp has to move into it and lose this note.
    g_play_frame = xd3d::frame_count();
}
