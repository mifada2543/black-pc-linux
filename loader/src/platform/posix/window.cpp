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
#include "platform/posix/window.h"

#include "gfx/gfx_vulkan.h"
#include "platform/posix/log.h"
#include "platform/posix/timing.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

namespace platform {

static bool g_sdl = false;
static SDL_Window *g_window = nullptr;
static char g_last_error[256] = "";
static bool g_size_changed = false;
static uint32_t g_frames = 0;
// The swapchain image the current frame is drawing into. swapchain_acquire chooses it, and the present has
// to be the same one: passing a constant 0 presented image 0 while the acquire semaphore belonged to
// another image, which is a frame the GPU never finished with.
static uint32_t g_current_image = 0;
static bool g_has_focus = false;
static bool g_mouse_captured = false;
static int g_mouse_wheel = 0;

void window_log_err(const char *fmt, const char *detail)
{
    snprintf(g_last_error, sizeof g_last_error, "%s: %s", fmt, detail ? detail : "(no detail)");
    logf("[platform] %s: %s", fmt, detail ? detail : "");
}

// Called by gfx::init() to create the Vulkan surface from our SDL window.
static VkResult make_surface(void *user, VkInstance inst, VkSurfaceKHR *out)
{
    (void)user;
    if (!g_window) return VK_ERROR_SURFACE_LOST_KHR;
    if (!SDL_Vulkan_CreateSurface(g_window, inst, out)) {
        const char *e = SDL_GetError();
        logf("[platform] SDL_Vulkan_CreateSurface failed: %s\n", e);
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    return VK_SUCCESS;
}

// --- public API ---

bool window_open(uint32_t width, uint32_t height, const char *title)
{
    memset(g_last_error, 0, sizeof g_last_error);
    g_frames = 0;
    g_size_changed = false;

    if (!g_sdl) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            window_log_err("SDL_Init", SDL_GetError());
            return false;
        }
        g_sdl = true;
    }

    const char *video = SDL_GetCurrentVideoDriver();
    if (!video) video = "unknown";
    logf("[platform] video driver: %s\n", video);

    SDL_Window *win = SDL_CreateWindow(
        title ? title : "BLACK",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        (int)width, (int)height,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!win) {
        window_log_err("SDL_CreateWindow", SDL_GetError());
        return false;
    }
    g_window = win;

    // SDL_Vulkan_GetInstanceExtensions names the extension that reaches this machine's window system.
    // An instance created without that extension cannot make a surface for this window system, and the
    // list is *deduplicated* rather than appended to: a name twice in ppEnabledExtensionNames is invalid
    // usage, and this layer saw it happen on the first try.
    unsigned int ext_count = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(g_window, &ext_count, nullptr)) {
        window_log_err("SDL_Vulkan_GetInstanceExtensions (count)", SDL_GetError());
        return false;
    }
    std::vector<const char *> ext(ext_count);
    if (!SDL_Vulkan_GetInstanceExtensions(g_window, &ext_count, ext.data())) {
        window_log_err("SDL_Vulkan_GetInstanceExtensions (list)", SDL_GetError());
        return false;
    }
    // Deduplicate against VK_KHR_surface which gfx already asks for.
    for (const char *e : ext)
        gfx::vulkan_want_instance_extension(e);

    gfx::vulkan_set_surface_creator(make_surface, nullptr);

    logf("[platform] window created: %ux%u requested\n", width, height);
    return true;
}

bool window_ready()
{
    return g_window != nullptr && gfx::surface_ready();
}

void window_close()
{
    if (!g_window) return;
    gfx::swapchain_invalidate();
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
    if (g_sdl) {
        SDL_Quit();
        g_sdl = false;
    }
}

const char *window_video_driver()
{
    static char buf[64] = "";
    if (!buf[0]) {
        const char *v = SDL_GetCurrentVideoDriver();
        if (v) snprintf(buf, sizeof buf, "%s", v);
    }
    return buf;
}

const char *window_last_error()
{
    return g_last_error;
}

bool window_pump_events()
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_RESIZED ||
                e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                g_size_changed = true;
            } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                g_has_focus = true;
                logf("[platform] window focus gained");
            } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                g_has_focus = false;
                g_mouse_captured = false;
                SDL_SetRelativeMouseMode(SDL_FALSE);
                logf("[platform] window focus lost");
            }
            break;
        case SDL_QUIT:
            return false;
        case SDL_MOUSEWHEEL:
            g_mouse_wheel += e.wheel.y; // positive = up, negative = down
            break;
        case SDL_MOUSEBUTTONDOWN:
            // Click in window captures mouse for mouselook
            if (e.button.button == SDL_BUTTON_LEFT && g_has_focus && !g_mouse_captured) {
                g_mouse_captured = true;
                SDL_SetRelativeMouseMode(SDL_TRUE);
                logf("[platform] mouse captured");
            }
            break;
        case SDL_KEYDOWN:
            // Escape releases mouse capture
            if (e.key.keysym.sym == SDLK_ESCAPE && g_mouse_captured) {
                g_mouse_captured = false;
                SDL_SetRelativeMouseMode(SDL_FALSE);
                logf("[platform] mouse released (Escape)");
            }
            break;
        default:
            break;
        }
    }
    return true;
}

uint32_t window_width()
{
    int w = 0, h = 0;
    if (!g_window) return 0;
    SDL_Vulkan_GetDrawableSize(g_window, &w, &h);
    return (uint32_t)w;
}

uint32_t window_height()
{
    int w = 0, h = 0;
    if (!g_window) return 0;
    SDL_Vulkan_GetDrawableSize(g_window, &w, &h);
    return (uint32_t)h;
}

bool window_size_changed()
{
    return g_size_changed;
}

bool window_begin_frame()
{
    if (!g_window) return false;

    uint32_t w = window_width(), h = window_height();
    if (w == 0 || h == 0) {
        g_last_error[0] = '\0';
        return false; // minimised or no drawable
    }

    if (g_size_changed) {
        // The drawable changed size. The swapchain is rebuilt, and the drawable it was using is now
        // invalid -- gfx will hand us a fresh one. We just mark the flag and clear it; gfx::swapchain_create
        // does the actual rebuild.
        g_size_changed = false;
        gfx::swapchain_invalidate();
    }

    if (!gfx::swapchain_create(VK_FORMAT_UNDEFINED, false)) {
        // The surface could not make a swapchain (minimised, no drawable area). Nothing to draw.
        const char *e = gfx::last_error();
        snprintf(g_last_error, sizeof g_last_error, "swapchain create: %s", e ? e : "(no detail)");
        logf("[platform] frame begin: %s\n", g_last_error);
        return false;
    }

    uint32_t idx = 0;
    if (!gfx::swapchain_acquire(&idx)) {
        const char *e = gfx::last_error();
        snprintf(g_last_error, sizeof g_last_error, "acquire: %s", e ? e : "(no detail)");
        return false;
    }
    g_current_image = idx;

    gfx::set_render_target(gfx::swapchain_image(idx), nullptr);
    if (!gfx::ready()) {
        const char *e = gfx::last_error();
        snprintf(g_last_error, sizeof g_last_error, "set render target: %s", e ? e : "(no detail)");
        return false;
    }
    return true;
}

gfx::Image *window_target()
{
    if (!gfx::swapchain_alive()) return nullptr;
    // The image window_begin_frame acquired, not "the first one": with more than one image in the
    // swapchain those are different pictures.
    return gfx::swapchain_image(g_current_image);
}

bool window_end_frame()
{
    if (!gfx::swapchain_alive()) return false;
    if (!gfx::swapchain_present(g_current_image)) {
        const char *e = gfx::last_error();
        snprintf(g_last_error, sizeof g_last_error, "present: %s", e ? e : "(no detail)");
        return false;
    }
    g_frames++;
    return true;
}

uint32_t window_frames()
{
    return g_frames;
}

bool window_present_game_frame(bool *closed)
{
    if (closed) *closed = false;
    if (!g_window) return false;

    // Events first. It is what makes closing the window a request to leave rather than a frame that never
    // ends, and it is also what keeps SDL's keyboard, mouse and pad state current for the input layer: SDL
    // updates those from the event queue, not from the hardware directly.
    if (!window_pump_events()) {
        if (closed) *closed = true;
        return false;
    }

    const bool shown = window_end_frame();

    // The console's Swap waits for the next field, and that wait is the game's frame rate: timing_wait_vblank
    // is the 59.94 Hz clock the rest of the port already paces --window on.
    timing_wait_vblank();

    // Open the next frame on the next image, so the game's next Clear or draw has somewhere to go. A failed
    // begin (minimised window, surface that went away) is not fatal: the next Swap tries again.
    window_begin_frame();
    return shown;
}

// Window focus state
bool window_has_focus()
{
    return g_has_focus;
}

// Mouse capture state
bool window_mouse_captured()
{
    return g_mouse_captured;
}

void window_set_mouse_captured(bool captured)
{
    g_mouse_captured = captured;
    if (g_window) {
        SDL_SetRelativeMouseMode(captured ? SDL_TRUE : SDL_FALSE);
    }
}

// Mouse wheel delta
int window_mouse_wheel()
{
    return g_mouse_wheel;
}

void window_add_mouse_wheel(int delta)
{
    g_mouse_wheel += delta;
}

} // namespace platform