// window.cpp, for a machine with no SDL2 development package.
//
// The loader is one program with no optional parts on Windows, so the obvious way to handle a missing
// window library is to require it and stop. That is the wrong trade here: SDL2 is needed by one mode of one
// program, while the other twenty-nine test suites and the whole kernel layer have nothing to do with
// windows, and a machine that cannot run a graphics test is still a machine whose kernel tests are worth
// running. So this file stands in for window.cpp when CMake cannot find SDL2, and every entry point says
// what is missing rather than pretending the window exists.
//
// It is deliberately the same header and the same names. A caller cannot tell which of the two it was
// linked against, so no caller grows a second opinion about whether a window is possible.
#include "platform/posix/window.h"

#include "platform/posix/log.h"

namespace platform {
namespace {

const char *kWhy = "this build has no SDL2: install the i386 development package with "
                  "`sudo apt install libsdl2-dev:i386` and configure again. The x86-64 package does not "
                  "help -- this loader is a 32-bit process.";

} // namespace

bool window_open(uint32_t width, uint32_t height, const char *title)
{
    (void)width;
    (void)height;
    (void)title;
    logf("[platform] window_open: %s\n", kWhy);
    return false;
}

bool window_ready() { return false; }

void window_close() {}

const char *window_video_driver() { return ""; }
const char *window_last_error() { return kWhy; }

bool window_pump_events() { return false; }
uint32_t window_width() { return 0; }
uint32_t window_height() { return 0; }
bool window_size_changed() { return false; }
bool window_begin_frame() { return false; }
bool window_end_frame() { return false; }
uint32_t window_frames() { return 0; }

} // namespace platform