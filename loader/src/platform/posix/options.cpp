// Command line options accessors for platform layer.
// This file is part of the blackcore library so the input layer can access option values.
#include "platform/posix/options.h"

#include <cstdint>

// Global options instance (set after parsing)
static Options *g_options = nullptr;

void options_set_instance(Options *o)
{
    g_options = o;
}

float get_mouse_sens()
{
    return g_options ? g_options->mouse_sens : 1.0f;
}

bool get_mouse_invert()
{
    return g_options ? g_options->mouse_invert : false;
}
