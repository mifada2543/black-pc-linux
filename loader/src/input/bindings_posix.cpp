// Keyboard/mouse bindings implementation for BLACK (FPS).
// Maps SDL keys/mouse to Xbox gamepad buttons per the user's table.
//
// User's mapping:
// | Action | Default |
// |---|---|
// | Move | W A S D |
// | Look | Mouse |
// | Fire / Aim | left / right mouse button |
// | Reload | R |
// | Action (use, pick up) | E |
#include "d3d/device_posix.h"
// | Crouch | C |
// | Grenade | G |
// | Melee | F |
// | Next / previous weapon | mouse wheel down / up |
// | Change firing mode | Z |
// | Toggle suppressor | Q |
// | Health pack | X |
// | Show objectives | Space |
// | Pause | Esc |
#include "input/bindings_posix.h"

#include "input/mouselook_posix.h"

#include "platform/posix/log.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace bindings {

// Game action indices (from the game's binding table). These are the indices
// the game's UI uses to reference each action.
enum GameAction : int {
    GA_MOVE_FORWARD = 0,   // W
    GA_MOVE_BACK = 1,      // S
    GA_MOVE_LEFT = 2,      // A
    GA_MOVE_RIGHT = 3,     // D
    GA_LOOK = 4,           // Mouse (handled separately)
    GA_FIRE = 5,           // Left mouse button
    GA_AIM = 6,            // Right mouse button
    GA_RELOAD = 7,         // R
    GA_ACTION = 8,         // E
    GA_CROUCH = 9,         // C
    GA_GRENADE = 10,       // G
    GA_MELEE = 11,         // F
    GA_NEXT_WEAPON = 12,   // Mouse wheel down
    GA_PREV_WEAPON = 13,   // Mouse wheel up
    GA_FIRE_MODE = 14,     // Z
    GA_SUPPRESSOR = 15,    // Q
    GA_HEALTH_PACK = 16,   // X
    GA_OBJECTIVES = 17,    // Space
    GA_PAUSE = 18,         // Esc
    GA_COUNT
};

namespace {

struct ActionDef {
    const char *ini;
    const char *name;
    int game;
    uint16_t defaults[2];
};

ActionDef g_actions[GA_COUNT] = {
    {"move_forward", "MOVE FORWARD", GA_MOVE_FORWARD, {KEY_SDL_BASE + SDL_SCANCODE_W, KEY_NONE}},
    {"move_back", "MOVE BACK", GA_MOVE_BACK, {KEY_SDL_BASE + SDL_SCANCODE_S, KEY_NONE}},
    {"move_left", "MOVE LEFT", GA_MOVE_LEFT, {KEY_SDL_BASE + SDL_SCANCODE_A, KEY_NONE}},
    {"move_right", "MOVE RIGHT", GA_MOVE_RIGHT, {KEY_SDL_BASE + SDL_SCANCODE_D, KEY_NONE}},
    {"look", "LOOK", GA_LOOK, {KEY_NONE, KEY_NONE}}, // Mouse, no key binding
    {"fire", "FIRE", GA_FIRE, {KEY_LBUTTON, KEY_NONE}}, // Left mouse
    {"aim", "AIM", GA_AIM, {KEY_RBUTTON, KEY_NONE}},    // Right mouse
    {"reload", "RELOAD", GA_RELOAD, {KEY_SDL_BASE + SDL_SCANCODE_R, KEY_NONE}},
    {"action", "ACTION", GA_ACTION, {KEY_SDL_BASE + SDL_SCANCODE_E, KEY_NONE}},
    {"crouch", "CROUCH", GA_CROUCH, {KEY_SDL_BASE + SDL_SCANCODE_C, KEY_NONE}},
    {"grenade", "GRENADE", GA_GRENADE, {KEY_SDL_BASE + SDL_SCANCODE_G, KEY_NONE}},
    {"melee", "MELEE", GA_MELEE, {KEY_SDL_BASE + SDL_SCANCODE_F, KEY_NONE}},
    {"next_weapon", "NEXT WEAPON", GA_NEXT_WEAPON, {KEY_WHEEL_DOWN, KEY_NONE}},
    {"prev_weapon", "PREV WEAPON", GA_PREV_WEAPON, {KEY_WHEEL_UP, KEY_NONE}},
    {"fire_mode", "FIRE MODE", GA_FIRE_MODE, {KEY_SDL_BASE + SDL_SCANCODE_Z, KEY_NONE}},
    {"suppressor", "SUPPRESSOR", GA_SUPPRESSOR, {KEY_SDL_BASE + SDL_SCANCODE_Q, KEY_NONE}},
    {"health_pack", "HEALTH PACK", GA_HEALTH_PACK, {KEY_SDL_BASE + SDL_SCANCODE_X, KEY_NONE}},
    {"objectives", "OBJECTIVES", GA_OBJECTIVES, {KEY_SDL_BASE + SDL_SCANCODE_SPACE, KEY_NONE}},
    {"pause", "PAUSE", GA_PAUSE, {KEY_SDL_BASE + SDL_SCANCODE_ESCAPE, KEY_NONE}},
};

uint16_t g_keys[GA_COUNT][2];
bool g_capturing = false;
int g_capture_action = -1;
int g_capture_slot = -1;
bool g_capture_change = false;
std::vector<std::string> g_script_holds;

} // namespace

// Public API functions (external linkage) - defined outside anonymous namespace
// but can access internal variables because they're in the same translation unit.

const Action *actions(int *count)
{
    *count = GA_COUNT;
    return reinterpret_cast<const Action *>(g_actions);
}

uint16_t key(int action, int slot)
{
    if (action >= 0 && action < GA_COUNT && slot >= 0 && slot < 2)
        return g_keys[action][slot];
    return KEY_NONE;
}

void set_key(int action, int slot, uint16_t k)
{
    if (action >= 0 && action < GA_COUNT && slot >= 0 && slot < 2)
        g_keys[action][slot] = k;
}

void reset_defaults()
{
    for (int i = 0; i < GA_COUNT; i++)
        for (int s = 0; s < 2; s++)
            g_keys[i][s] = g_actions[i].defaults[s];
}

bool fixed(int action, int slot)
{
    return action == GA_PAUSE && slot == 0;
}

std::string key_name(uint16_t k)
{
    if (k == KEY_NONE) return "NONE";
    if (k == KEY_WHEEL_UP) return "WHEEL UP";
    if (k == KEY_WHEEL_DOWN) return "WHEEL DOWN";
    if (k == KEY_LBUTTON) return "LMB";
    if (k == KEY_RBUTTON) return "RMB";
    if (k == KEY_MBUTTON) return "MMB";
    if (k == KEY_XBUTTON1) return "XBUTTON1";
    if (k == KEY_XBUTTON2) return "XBUTTON2";
    if (k >= KEY_SDL_BASE) {
        SDL_Scancode sc = static_cast<SDL_Scancode>(k - KEY_SDL_BASE);
        const char *n = SDL_GetScancodeName(sc);
        return n ? n : "UNKNOWN";
    }
    return "UNKNOWN";
}

bool load_ini(const char *key, const char *value)
{
    if (strncmp(key, "key_", 4) != 0) return false;
    const char *act_ini = key + 4;
    for (int i = 0; i < GA_COUNT; i++) {
        if (strcmp(g_actions[i].ini, act_ini) == 0) {
            char buf[256];
            strncpy(buf, value, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            char *tok = strtok(buf, ",");
            for (int s = 0; tok && s < 2; s++) {
                while (*tok == ' ') tok++;
                char *end = tok + strlen(tok) - 1;
                while (end > tok && *end == ' ') *end-- = '\0';
                uint16_t code = KEY_NONE;
                if (strcasecmp(tok, "WHEELUP") == 0 || strcasecmp(tok, "WHEEL UP") == 0) code = KEY_WHEEL_UP;
                else if (strcasecmp(tok, "WHEELDOWN") == 0 || strcasecmp(tok, "WHEEL DOWN") == 0) code = KEY_WHEEL_DOWN;
                else if (strcasecmp(tok, "LMB") == 0 || strcasecmp(tok, "LEFT MOUSE") == 0) code = KEY_LBUTTON;
                else if (strcasecmp(tok, "RMB") == 0 || strcasecmp(tok, "RIGHT MOUSE") == 0) code = KEY_RBUTTON;
                else if (strcasecmp(tok, "MMB") == 0 || strcasecmp(tok, "MIDDLE MOUSE") == 0) code = KEY_MBUTTON;
                else {
                    SDL_Scancode sc = SDL_GetScancodeFromName(tok);
                    if (sc != SDL_SCANCODE_UNKNOWN) code = KEY_SDL_BASE + sc;
                }
                if (code != KEY_NONE) g_keys[i][s] = code;
                tok = strtok(nullptr, ",");
            }
            return true;
        }
    }
    return false;
}

void save_ini(FILE *f)
{
    for (int i = 0; i < GA_COUNT; i++) {
        char keys[2][64];
        for (int s = 0; s < 2; s++) {
            uint16_t k = g_keys[i][s];
            if (k == KEY_NONE) keys[s][0] = '\0';
            else if (k == KEY_WHEEL_UP) strcpy(keys[s], "WHEEL UP");
            else if (k == KEY_WHEEL_DOWN) strcpy(keys[s], "WHEEL DOWN");
            else if (k == KEY_LBUTTON) strcpy(keys[s], "LMB");
            else if (k == KEY_RBUTTON) strcpy(keys[s], "RMB");
            else if (k == KEY_MBUTTON) strcpy(keys[s], "MMB");
            else if (k >= KEY_SDL_BASE) {
                SDL_Scancode sc = static_cast<SDL_Scancode>(k - KEY_SDL_BASE);
                const char *n = SDL_GetScancodeName(sc);
                strcpy(keys[s], n ? n : "UNKNOWN");
            } else strcpy(keys[s], "UNKNOWN");
        }
        if (keys[0][0] || keys[1][0]) {
            fprintf(f, "key_%s = %s", g_actions[i].ini, keys[0]);
            if (keys[1][0]) fprintf(f, ",%s", keys[1]);
            fprintf(f, "\n");
        }
    }
}

void script_hold(const std::string &name)
{
    g_script_holds.push_back(name);
}

bool is_key_pressed(uint16_t k, bool focus)
{
    (void)focus;
    if (k == KEY_NONE) return false;
    if (k == KEY_WHEEL_UP || k == KEY_WHEEL_DOWN) return false;
    if (k == KEY_LBUTTON) return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    if (k == KEY_RBUTTON) return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
    if (k == KEY_MBUTTON) return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(SDL_BUTTON_MIDDLE)) != 0;
    if (k >= KEY_SDL_BASE) {
        SDL_Scancode sc = static_cast<SDL_Scancode>(k - KEY_SDL_BASE);
        const uint8_t *state = SDL_GetKeyboardState(nullptr);
        return state[sc];
    }
    return false;
}

int g_menu_input = 0;
bool g_menu_select_held = false;

int take_menu_input()
{
    int r = g_menu_input;
    g_menu_input = 0;
    return r;
}

bool menu_select_held()
{
    return g_menu_select_held;
}

void capture_start(int action, int slot)
{
    g_capturing = true;
    g_capture_action = action;
    g_capture_slot = slot;
    g_capture_change = false;
    SDL_PumpEvents();
}

bool capturing()
{
    return g_capturing;
}

bool capture_poll(bool focus, int wheel)
{
    (void)focus;
    (void)wheel;
    if (!g_capturing) return false;

    const uint8_t *kb = SDL_GetKeyboardState(nullptr);
    uint32_t mouse = SDL_GetMouseState(nullptr, nullptr);

    if (kb[SDL_SCANCODE_ESCAPE]) {
        g_capturing = false;
        return true;
    }

    if (kb[SDL_SCANCODE_DELETE]) {
        g_keys[g_capture_action][g_capture_slot] = KEY_NONE;
        g_capturing = false;
        g_capture_change = true;
        return true;
    }

    for (SDL_Scancode sc = SDL_SCANCODE_A; sc < SDL_NUM_SCANCODES; sc = static_cast<SDL_Scancode>(sc + 1)) {
        if (kb[sc]) {
            for (int i = 0; i < GA_COUNT; i++)
                for (int s = 0; s < 2; s++)
                    if (g_keys[i][s] == KEY_SDL_BASE + sc) g_keys[i][s] = KEY_NONE;
            g_keys[g_capture_action][g_capture_slot] = KEY_SDL_BASE + sc;
            g_capturing = false;
            g_capture_change = true;
            return true;
        }
    }

    if (mouse & SDL_BUTTON(SDL_BUTTON_LEFT)) {
        for (int i = 0; i < GA_COUNT; i++)
            for (int s = 0; s < 2; s++)
                if (g_keys[i][s] == KEY_LBUTTON) g_keys[i][s] = KEY_NONE;
        g_keys[g_capture_action][g_capture_slot] = KEY_LBUTTON;
        g_capturing = false;
        g_capture_change = true;
        return true;
    }
    if (mouse & SDL_BUTTON(SDL_BUTTON_RIGHT)) {
        for (int i = 0; i < GA_COUNT; i++)
            for (int s = 0; s < 2; s++)
                if (g_keys[i][s] == KEY_RBUTTON) g_keys[i][s] = KEY_NONE;
        g_keys[g_capture_action][g_capture_slot] = KEY_RBUTTON;
        g_capturing = false;
        g_capture_change = true;
        return true;
    }
    if (mouse & SDL_BUTTON(SDL_BUTTON_MIDDLE)) {
        for (int i = 0; i < GA_COUNT; i++)
            for (int s = 0; s < 2; s++)
                if (g_keys[i][s] == KEY_MBUTTON) g_keys[i][s] = KEY_NONE;
        g_keys[g_capture_action][g_capture_slot] = KEY_MBUTTON;
        g_capturing = false;
        g_capture_change = true;
        return true;
    }

    if (wheel > 0) {
        for (int i = 0; i < GA_COUNT; i++)
            for (int s = 0; s < 2; s++)
                if (g_keys[i][s] == KEY_WHEEL_UP) g_keys[i][s] = KEY_NONE;
        g_keys[g_capture_action][g_capture_slot] = KEY_WHEEL_UP;
        g_capturing = false;
        g_capture_change = true;
        return true;
    }
    if (wheel < 0) {
        for (int i = 0; i < GA_COUNT; i++)
            for (int s = 0; s < 2; s++)
                if (g_keys[i][s] == KEY_WHEEL_DOWN) g_keys[i][s] = KEY_NONE;
        g_keys[g_capture_action][g_capture_slot] = KEY_WHEEL_DOWN;
        g_capturing = false;
        g_capture_change = true;
        return true;
    }

    return true;
}

bool capture_take_change()
{
    if (g_capture_change) {
        g_capture_change = false;
        return true;
    }
    return false;
}

void apply(uint16_t &buttons, uint8_t analog[8], int16_t &lx, int16_t &ly, int16_t &rx, int16_t &ry,
           bool focus, bool mouse_live, int wheel)
{
    (void)mouse_live;
    for (const auto &name : g_script_holds) {
        for (int i = 0; i < GA_COUNT; i++) {
            if (strcasecmp(g_actions[i].ini, name.c_str()) == 0) {
                // Simulated press - handled by is_key_pressed below
                break;
            }
        }
    }
    g_script_holds.clear();

    // Movement (left stick) - WASD
    bool forward = is_key_pressed(g_keys[GA_MOVE_FORWARD][0], focus) || is_key_pressed(g_keys[GA_MOVE_FORWARD][1], focus);
    bool back = is_key_pressed(g_keys[GA_MOVE_BACK][0], focus) || is_key_pressed(g_keys[GA_MOVE_BACK][1], focus);
    bool left = is_key_pressed(g_keys[GA_MOVE_LEFT][0], focus) || is_key_pressed(g_keys[GA_MOVE_LEFT][1], focus);
    bool right = is_key_pressed(g_keys[GA_MOVE_RIGHT][0], focus) || is_key_pressed(g_keys[GA_MOVE_RIGHT][1], focus);

    int ly_val = 0, lx_val = 0;
    if (forward) ly_val += 32767;
    if (back) ly_val -= 32767;
    if (left) lx_val -= 32767;
    if (right) lx_val += 32767;

    ly = static_cast<int16_t>(std::clamp(ly_val, -32767, 32767));
    lx = static_cast<int16_t>(std::clamp(lx_val, -32767, 32767));

    // Fire (A button) - Left mouse
    bool fire = is_key_pressed(g_keys[GA_FIRE][0], focus) || is_key_pressed(g_keys[GA_FIRE][1], focus);
    if (fire) {
        buttons |= 0x1000;
        analog[0] = 255;
    }

    // Aim (Left trigger) - Right mouse
    bool aim = is_key_pressed(g_keys[GA_AIM][0], focus) || is_key_pressed(g_keys[GA_AIM][1], focus);
    if (aim) {
        analog[6] = 255;
    }

    // Reload (X button)
    bool reload = is_key_pressed(g_keys[GA_RELOAD][0], focus) || is_key_pressed(g_keys[GA_RELOAD][1], focus);
    if (reload) {
        buttons |= 0x4000;
        analog[2] = 255;
    }

    // Action/Use (B button)
    bool action = is_key_pressed(g_keys[GA_ACTION][0], focus) || is_key_pressed(g_keys[GA_ACTION][1], focus);
    if (action) {
        buttons |= 0x2000;
        analog[1] = 255;
    }

    // Crouch (Left stick click)
    bool crouch = is_key_pressed(g_keys[GA_CROUCH][0], focus) || is_key_pressed(g_keys[GA_CROUCH][1], focus);
    if (crouch) {
        buttons |= 0x0040;
    }

    // Grenade (Y button)
    bool grenade = is_key_pressed(g_keys[GA_GRENADE][0], focus) || is_key_pressed(g_keys[GA_GRENADE][1], focus);
    if (grenade) {
        buttons |= 0x8000;
        analog[3] = 255;
    }

    // Melee (Right stick click)
    bool melee = is_key_pressed(g_keys[GA_MELEE][0], focus) || is_key_pressed(g_keys[GA_MELEE][1], focus);
    if (melee) {
        buttons |= 0x0080;
    }

    // Next/Prev weapon (D-pad)
    if (wheel > 0) {
        bool prev = is_key_pressed(g_keys[GA_PREV_WEAPON][0], focus) || is_key_pressed(g_keys[GA_PREV_WEAPON][1], focus);
        if (prev) buttons |= 0x0001;
    }
    if (wheel < 0) {
        bool next = is_key_pressed(g_keys[GA_NEXT_WEAPON][0], focus) || is_key_pressed(g_keys[GA_NEXT_WEAPON][1], focus);
        if (next) buttons |= 0x0002;
    }

    // Fire mode (D-pad left)
    bool fire_mode = is_key_pressed(g_keys[GA_FIRE_MODE][0], focus) || is_key_pressed(g_keys[GA_FIRE_MODE][1], focus);
    if (fire_mode) buttons |= 0x0004;

    // Suppressor (D-pad right)
    bool suppressor = is_key_pressed(g_keys[GA_SUPPRESSOR][0], focus) || is_key_pressed(g_keys[GA_SUPPRESSOR][1], focus);
    if (suppressor) buttons |= 0x0008;

    // Health pack (Right shoulder / Black)
    bool health = is_key_pressed(g_keys[GA_HEALTH_PACK][0], focus) || is_key_pressed(g_keys[GA_HEALTH_PACK][1], focus);
    if (health) {
        buttons |= 0x0200;
        analog[4] = 255;
    }

    // Objectives (Back button)
    bool obj = is_key_pressed(g_keys[GA_OBJECTIVES][0], focus) || is_key_pressed(g_keys[GA_OBJECTIVES][1], focus);
    if (obj) buttons |= 0x0020;

    // Pause (Start button)
    bool pause = is_key_pressed(g_keys[GA_PAUSE][0], focus) || is_key_pressed(g_keys[GA_PAUSE][1], focus);
    if (pause) buttons |= 0x0010;

    // Menu navigation (when not in play)
    if (!in_play()) {
        int menu_input = 0;
        if (is_key_pressed(g_keys[GA_MOVE_FORWARD][0], focus) || is_key_pressed(g_keys[GA_MOVE_FORWARD][1], focus)) menu_input = 1;
        else if (is_key_pressed(g_keys[GA_MOVE_BACK][0], focus) || is_key_pressed(g_keys[GA_MOVE_BACK][1], focus)) menu_input = 2;
        else if (is_key_pressed(g_keys[GA_MOVE_LEFT][0], focus) || is_key_pressed(g_keys[GA_MOVE_LEFT][1], focus)) menu_input = 3;
        else if (is_key_pressed(g_keys[GA_MOVE_RIGHT][0], focus) || is_key_pressed(g_keys[GA_MOVE_RIGHT][1], focus)) menu_input = 4;
        else if (is_key_pressed(g_keys[GA_ACTION][0], focus) || is_key_pressed(g_keys[GA_ACTION][1], focus)) menu_input = 5;
        else if (is_key_pressed(g_keys[GA_PAUSE][0], focus) || is_key_pressed(g_keys[GA_PAUSE][1], focus)) menu_input = 6;
        g_menu_input = menu_input;
        g_menu_select_held = (menu_input == 5);
    }
}

bool in_play()
{
    return mouselook_play_frame() == xd3d::frame_count() && xd3d::frame_count() > 0;
}

} // namespace bindings
