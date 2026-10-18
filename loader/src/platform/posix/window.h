// The host window: SDL2 makes the window, Vulkan presents into it.
//
// What lives here and what lives in gfx
// ------------------------------------
// SDL2 is a window system, not a graphics API, and everything about it that matters to this layer -- the
// X11 or Wayland extension Vulkan needs, the call that turns a drawable into a VkSurfaceKHR, the size the
// drawable actually ended up -- is asked of SDL2 rather than assumed. So this file never names a Vulkan
// type, and gfx/gfx_vulkan.cpp never includes an SDL header. The two meet at exactly two points, both in
// the other direction: the extensions are handed to gfx before gfx::init() exists, and the surface is made
// through a plain function pointer gfx calls back through.
//
// The order of the three steps below is Vulkan's order, not a preference
// ---------------------------------------------------------------
// SDL_Init and SDL_CreateWindow come first because SDL_Vulkan_GetInstanceExtensions names the extension
// that reaches this machine's window system, and an instance cannot be created with an extension it was
// not told about. gfx::init() comes second, and it makes the surface *inside* itself: the queue family that
// can present has to be chosen when the logical device is created, and answering that question needs a
// surface to ask about. swapchain_create() comes last, when the device exists and the only thing left is
// the surface's geometry. Getting this order wrong produces a loader with a window it cannot present to,
// which is why it is written down rather than discovered.
//
// Two things measured on this machine (2026-10-19, XWayland, SDL 2.30.0, Vulkan 1.4.318) that shaped it:
//
//   - SDL2 *does* name VK_KHR_surface in its own instance extension list, so the list is deduplicated
//     rather than appended to blindly (a name twice in ppEnabledExtensionNames is invalid usage).
//   - The window's size is a request and the drawable is the truth: a window asked for 900x600 reports a
//     drawable of 796x966 here, because the window manager had its own idea. So the size the swapchain is
//     built at comes from the surface (see gfx::swapchain_create), and window_width()/window_height()
//     report the drawable, not what was asked for.
#pragma once

#include <stdint.h>

namespace gfx {
struct Image;
}

namespace platform {

// Open the window and everything behind it: SDL, the Vulkan instance, the surface and the swapchain.
// `width` and `height` are a request in pixels for the window; the drawable ends up whatever the window
// system makes of it. False means nothing was opened and window_last_error() says which of the four steps
// failed and why -- a machine with no display, a machine with no Vulkan, and a machine whose surface the
// window system refused all look identical from the outside otherwise.
bool window_open(uint32_t width, uint32_t height, const char *title);
bool window_ready();
void window_close();

// What the window system is actually driving: SDL_GetCurrentVideoDriver() ("x11", "wayland", ...). Empty
// before window_open. Reported rather than assumed, because the instance extension list comes from it and
// "VK_KHR_xlib_surface" against "VK_KHR_wayland_surface" is the whole difference.
const char *window_video_driver();

// SDL's own answer to whatever it last refused. Empty when it has not refused anything.
const char *window_last_error();

// Drain SDL's event queue. False means the user closed the window or the platform asked the application
// to quit, which is the loop's reason to end. Everything else (a resize, a key, focus) is recorded rather
// than reported: a frame loop asks about what it cares about.
bool window_pump_events();

// The drawable's size in pixels, as SDL reports it. Not the size that was asked for, and not necessarily
// the size the swapchain is: the swapchain belongs to the surface and is asked for itself.
uint32_t window_width();
uint32_t window_height();

// Whether the drawable changed size since the last frame. Read once per frame by window_begin_frame(), so
// a caller that asks twice gets the same answer twice.
bool window_size_changed();

// One frame, up to the drawable. Rebuilds the swapchain first if the window changed shape or the driver
// said the old one was stale, takes the next drawable, makes it gfx's render target and opens a frame on
// it. False means this frame has nothing to draw into -- a minimised window, a surface that went away --
// and nothing was recorded, so a caller can skip the frame rather than draw into the last image.
bool window_begin_frame();

// The drawable this frame is drawing into, as the gfx Image it was made of, or null when there is no
// frame in progress.
gfx::Image *window_target();

// Submit and present. The frame is submitted by gfx::swapchain_present, which is also what waits; a caller
// does not end the frame itself. False means the frame was not presented and gfx::last_error() says why.
// A stale swapchain afterwards is a request to rebuild on the next frame, not a failure.
bool window_end_frame();

// Frames presented since window_open. The counter the --window mode stops on, and the number a test
// checks rather than "it ran for a while".
uint32_t window_frames();

} // namespace platform