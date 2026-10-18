// Mouse look: the mouse rotates the player directly (patches in the game's player update).
// Linux SDL2 version.
#pragma once

#include <cstdint>

void install_mouselook_patches();
bool mouselook_installed();
// The frame of the last player update, 0 before the first. On Windows that is exactly what it says: the
// installed patch stamps it from the player's view update, which menus, pauses and cutscenes do not run,
// and bindings::in_play() reads it to tell play from menus.
//
// On this port it is a PROXY and must not be read as the real thing. The player-update hook is not built,
// so mouselook_set_frame() stamps the pad poll instead -- see its comment for the full reasoning. That
// happens in menus too, so the value does NOT distinguish play from a menu yet, and bindings::in_play()
// additionally compares against xd3d::frame_count(), which nothing currently increments.
//
// When the player-update hook is built, the stamp moves into it and this note goes away with it.
uint32_t mouselook_play_frame();
// The mouse angle for the next player update, in degrees (yaw + = left, pitch + = the game's pitch rate
// direction). Called once per pad poll; replaces what the previous frame did not use.
void mouselook_set_frame(float yaw_deg, float pitch_deg);