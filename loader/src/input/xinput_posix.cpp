// Linux XInput HLE implementation using SDL2.
// Maps SDL gamepad/keyboard/mouse to Xbox port 0 XINPUT_GAMEPAD.
#include "input/xinput_posix.h"

#include "input/bindings_posix.h"
#include "input/mouselook_posix.h"
#include "input/input_detail_posix.h"

#include "platform/posix/log.h"
#include "platform/posix/options.h"
#include "d3d/device_posix.h"
#include "platform/posix/window.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace xinput {

namespace {

struct SdlPad {
    SDL_GameController *ctrl = nullptr;
    uint32_t packet = 0;
    XGamepad last{};
};

SdlPad g_pads[4];
bool g_reported_insertion = false;
bool g_sdl_initialized = false;
uint32_t g_polls = 0;

uint16_t sdl_buttons_to_xbox(SDL_GameController *ctrl)
{
    uint16_t btn = 0;
    if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_UP)) btn |= 0x0001;
    if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) btn |= 0x0002;
    if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) btn |= 0x0004;
    if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= 0x0008;
    if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_START)) btn |= 0x0010;
    if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_BACK)) btn |= 0x0020;
    if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_LEFTSTICK)) btn |= 0x0040;
    if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= 0x0080;
    if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) btn |= 0x0100;
    if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) btn |= 0x0200;
    if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_A)) btn |= 0x1000;
    if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_B)) btn |= 0x2000;
    if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_X)) btn |= 0x4000;
    if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_Y)) btn |= 0x8000;
    return btn;
}

int16_t sdl_axis_to_int16(Sint16 v) { return static_cast<int16_t>(v); }

uint8_t sdl_trigger_to_uint8(Sint16 v)
{
    int val = v;
    if (val < 0) val = 0;
    if (val > 32767) val = 32767;
    return static_cast<uint8_t>((val * 255) / 32767);
}

void init_sdl_input()
{
    if (g_sdl_initialized) return;
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
        logf("[input] SDL_InitSubSystem(GAMECONTROLLER) failed: %s", SDL_GetError());
        return;
    }
    SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
    g_sdl_initialized = true;
    logf("[input] SDL2 gamepad subsystem initialized");
}

void poll_sdl_pads(XGamepad &g)
{
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i].ctrl) continue;
        SDL_GameControllerUpdate();
        XGamepad pg{};
        pg.wButtons = sdl_buttons_to_xbox(g_pads[i].ctrl);
        pg.bAnalogButtons[0] = SDL_GameControllerGetButton(g_pads[i].ctrl, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
        pg.bAnalogButtons[1] = SDL_GameControllerGetButton(g_pads[i].ctrl, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
        pg.bAnalogButtons[2] = SDL_GameControllerGetButton(g_pads[i].ctrl, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
        pg.bAnalogButtons[3] = SDL_GameControllerGetButton(g_pads[i].ctrl, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
        pg.bAnalogButtons[4] = SDL_GameControllerGetButton(g_pads[i].ctrl, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;
        pg.bAnalogButtons[5] = SDL_GameControllerGetButton(g_pads[i].ctrl, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
        pg.bAnalogButtons[6] = sdl_trigger_to_uint8(SDL_GameControllerGetAxis(g_pads[i].ctrl, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
        pg.bAnalogButtons[7] = sdl_trigger_to_uint8(SDL_GameControllerGetAxis(g_pads[i].ctrl, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
        pg.sThumbLX = sdl_axis_to_int16(SDL_GameControllerGetAxis(g_pads[i].ctrl, SDL_CONTROLLER_AXIS_LEFTX));
        pg.sThumbLY = sdl_axis_to_int16(SDL_GameControllerGetAxis(g_pads[i].ctrl, SDL_CONTROLLER_AXIS_LEFTY));
        pg.sThumbRX = sdl_axis_to_int16(SDL_GameControllerGetAxis(g_pads[i].ctrl, SDL_CONTROLLER_AXIS_RIGHTX));
        pg.sThumbRY = sdl_axis_to_int16(SDL_GameControllerGetAxis(g_pads[i].ctrl, SDL_CONTROLLER_AXIS_RIGHTY));

        auto merge = [](int16_t &dst_x, int16_t &dst_y, int16_t src_x, int16_t src_y) {
            int64_t dst_mag = int64_t(dst_x) * dst_x + int64_t(dst_y) * dst_y;
            int64_t src_mag = int64_t(src_x) * src_x + int64_t(src_y) * src_y;
            if (src_mag > dst_mag) { dst_x = src_x; dst_y = src_y; }
        };
        merge(g.sThumbLX, g.sThumbLY, pg.sThumbLX, pg.sThumbLY);
        merge(g.sThumbRX, g.sThumbRY, pg.sThumbRX, pg.sThumbRY);
        for (int b = 0; b < 8; b++) if (pg.bAnalogButtons[b] > g.bAnalogButtons[b]) g.bAnalogButtons[b] = pg.bAnalogButtons[b];
        g.wButtons |= pg.wButtons;
    }
}

void parse_presses()
{
    static bool done = false;
    if (done) return;
    done = true;
}

void apply_presses(XGamepad &g)
{
    parse_presses();
    (void)g;
}

void poll_pad(XGamepad &g)
{
    memset(&g, 0, sizeof g);
    init_sdl_input();
    poll_sdl_pads(g);
    apply_presses(g);

    bool focus = platform::window_has_focus();
    bool live = focus && platform::window_mouse_captured();

    int wheel = platform::window_mouse_wheel();

    int mx = 0, my = 0;
    if (live) SDL_GetRelativeMouseState(&mx, &my);

    float k = 0.022f * get_mouse_sens();
    mouselook_set_frame(-mx * k, (get_mouse_invert() ? -my : my) * k);

    bindings::apply(g.wButtons, g.bAnalogButtons, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY,
                    focus, live, wheel);
}

} // namespace

// ---- HLE entry points ----

uint32_t XD3D_STDCALL XInitDevices(uint32_t dwPreallocTypeCount, void *PreallocTypes)
{
    (void)dwPreallocTypeCount; (void)PreallocTypes;
    logf("[input] XInitDevices");
    init_sdl_input();
    return 0;
}

uint32_t XD3D_STDCALL XGetDeviceChanges(void *DeviceType, uint32_t *pdwInsertions, uint32_t *pdwRemovals)
{
    *pdwInsertions = 0; *pdwRemovals = 0;
    if (DeviceType == (void *)(uintptr_t)0x0027cf4c && !g_reported_insertion) {
        g_reported_insertion = true;
        *pdwInsertions = 1;
        logf("[input] XGetDeviceChanges: gamepad inserted on port 0");
        return 1;
    }
    return 0;
}

void *XD3D_STDCALL XInputOpen(void *DeviceType, uint32_t dwPort, uint32_t dwSlot, void *pPollingParameters)
{
    (void)DeviceType; (void)dwSlot; (void)pPollingParameters;
    if (dwPort != 0) return nullptr;

    static Device dev;
    dev.magic = DEV_MAGIC;
    dev.port = dwPort;
    dev.packet = 0;
    memset(&dev.last, 0, sizeof dev.last);

    for (int i = 0; i < 4; i++) {
        if (SDL_IsGameController(i)) {
            g_pads[i].ctrl = SDL_GameControllerOpen(i);
            if (g_pads[i].ctrl) logf("[input] Opened SDL gamepad %d: %s", i, SDL_GameControllerName(g_pads[i].ctrl));
        }
    }
    logf("[input] XInputOpen(port %u) -> %p", dwPort, &dev);
    return &dev;
}

void XD3D_STDCALL XInputClose(void *h)
{
    Device *dev = static_cast<Device *>(h);
    if (dev && dev->magic == DEV_MAGIC) {
        dev->magic = 0;
        for (int i = 0; i < 4; i++) {
            if (g_pads[i].ctrl) { SDL_GameControllerClose(g_pads[i].ctrl); g_pads[i].ctrl = nullptr; }
        }
        logf("[input] XInputClose");
    }
}

uint32_t XD3D_STDCALL XInputGetState(void *h, XState *pState)
{
    Device *dev = static_cast<Device *>(h);
    if (!dev || dev->magic != DEV_MAGIC) return 0x80070015;

    XGamepad g;
    poll_pad(g);

    if (memcmp(&g, &dev->last, sizeof g) != 0) {
        dev->last = g;
        dev->packet++;
    }
    pState->dwPacketNumber = dev->packet;
    pState->Gamepad = g;

    g_polls++;
    if ((g_polls % 600) == 1) {
        logf("[input] XInputGetState poll #%u (frame %u)", g_polls, xd3d::frame_count());
    }
    return 0;
}

uint32_t XD3D_STDCALL XInputSetState(void *h, uint8_t *pFeedback)
{
    Device *dev = static_cast<Device *>(h);
    if (!dev || dev->magic != DEV_MAGIC) return 0x80070015;

    uint16_t left = *reinterpret_cast<uint16_t *>(pFeedback + 68);
    uint16_t right = *reinterpret_cast<uint16_t *>(pFeedback + 70);

    // Use input_detail rumble system
    input_detail::RumbleEffect effect;
    effect.low_freq = left;
    effect.high_freq = right;
    effect.duration_ms = 500; // default duration

    for (int i = 0; i < 4; i++) {
        if (g_pads[i].ctrl) {
            input_detail::rumble_start(i, effect);
            // Also apply directly via SDL for immediate feedback
            SDL_Joystick *joy = SDL_GameControllerGetJoystick(g_pads[i].ctrl);
            if (joy) {
                SDL_Haptic *haptic = SDL_HapticOpenFromJoystick(joy);
                if (haptic) {
                    SDL_HapticRumbleInit(haptic);
                    float strength = (left + right) / 2.0f / 65535.0f;
                    SDL_HapticRumblePlay(haptic, strength, 500);
                    SDL_HapticClose(haptic);
                }
            }
        }
    }
    *reinterpret_cast<uint32_t *>(pFeedback) = 0;
    return 0x000003E5;
}

const xd3d::Hle *hle_input_table(size_t *count)
{
    static const xd3d::Hle kTable[] = {
        {0x0027dd24, "XInitDevices", (void *)XInitDevices, "s s", false},
        {0x0027dd29, "XGetDeviceChanges", (void *)XGetDeviceChanges, "s s s", true},
        {0x0027dd96, "XInputOpen", (void *)XInputOpen, "s s s s", true},
        {0x0027ddec, "XInputClose", (void *)XInputClose, "s", false},
        {0x0027ddf8, "XInputGetState", (void *)XInputGetState, "s s", true},
        {0x0027de6b, "XInputSetState", (void *)XInputSetState, "s s", true},
    };
    *count = sizeof kTable / sizeof kTable[0];
    return kTable;
}

} // namespace xinput
