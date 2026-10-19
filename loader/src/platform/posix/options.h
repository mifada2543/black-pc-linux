// Command line options accessors for platform layer.
#pragma once

#include <cstdint>
#include <string>

struct Options {
    std::string xbe = "game/default.xbe";
    std::string log = "blacklinux.log";
    std::string hdd;
    bool doctor = false;
    bool probe = false;
    bool window = false;
    bool info = false;
    bool trace = false;
    bool strict = false;
    bool help = false;
    bool version = false;
    uint32_t timeout_s = 15;
    uint32_t frames = 60;
    uint32_t window_width = 0;
    uint32_t window_height = 0;
    float mouse_sens = 1.0f;
    bool mouse_invert = false;
    bool reverb = true;        // the I3DL2 send (mix bin 11) is mixed; --no-reverb turns it off
    float reverb_gain = 1.0f;  // overall level of that send (--reverb-gain)
};

// Set the global options instance (called from main after parsing)
void options_set_instance(Options *o);

// Mouse look settings (from command line options)
float get_mouse_sens();
bool get_mouse_invert();

// Reverb settings (from command line options)
bool get_reverb();
float get_reverb_gain();
