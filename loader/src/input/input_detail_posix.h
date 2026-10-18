// Input detail: deadzone, sensitivity curve, rumble pattern from XBE config.
// This layer sits between SDL raw input and the Xbox gamepad state.
// When XBE is available, it reads the game's config (deadzone, sensitivity, rumble patterns)
// and overrides the defaults. Without XBE, uses sane defaults for BLACK FPS.
#pragma once

#include <cstdint>

namespace input_detail {

// Axis deadzone and sensitivity per stick
struct StickConfig {
    float deadzone = 0.15f;        // 0.0..1.0, radial deadzone
    float sensitivity = 1.0f;      // multiplier
    float acceleration = 0.0f;     // 0.0 = linear, >0 = exponential curve
    float max_output = 1.0f;       // clamp
    bool invert = false;           // Y axis
};

// Trigger deadzone and response curve
struct TriggerConfig {
    float deadzone = 0.10f;        // 0.0..1.0
    float sensitivity = 1.0f;      // multiplier
    float curve_power = 1.0f;      // 1.0 = linear, >1 = slow start
};

// Rumble effect definition (from XBE config)
struct RumbleEffect {
    uint16_t low_freq = 0;         // left motor (0..65535)
    uint16_t high_freq = 0;        // right motor (0..65535)
    uint32_t duration_ms = 0;      // 0 = until stopped
    uint32_t attack_ms = 0;        // fade in
    uint32_t decay_ms = 0;         // fade out
};

// Gamepad config loaded from XBE (or defaults)
struct GamepadConfig {
    StickConfig left_stick;
    StickConfig right_stick;
    TriggerConfig left_trigger;
    TriggerConfig right_trigger;

    // Rumble patterns by event name (from XBE config)
    RumbleEffect rumble_recoil;           // shooting
    RumbleEffect rumble_explosion;        // explosion nearby
    RumbleEffect rumble_damage;           // player hit
    RumbleEffect rumble_vehicle;          // driving
    RumbleEffect rumble_heartbeat;        // low health
};

// Global config (defaults for BLACK FPS, overridable by XBE)
const GamepadConfig &get_config();

// Apply deadzone to a single axis (-1.0..1.0 -> -1.0..1.0)
float apply_deadzone_axis(float value, const StickConfig &cfg);

// Apply radial deadzone to stick (x, y in -1.0..1.0)
void apply_deadzone_stick(float &x, float &y, const StickConfig &cfg);

// Apply trigger curve (0.0..1.0 -> 0.0..1.0)
float apply_trigger_curve(float value, const TriggerConfig &cfg);

// Apply sensitivity and acceleration curve
float apply_sensitivity(float value, const StickConfig &cfg);

// Convert SDL axis (-32768..32767) to normalized (-1.0..1.0) with config
void sdl_axis_to_norm(int16_t sdl_x, int16_t sdl_y, float &out_x, float &out_y, const StickConfig &cfg);
float sdl_trigger_to_norm(uint16_t sdl_val, const TriggerConfig &cfg);

// Rumble state machine
struct RumbleState {
    RumbleEffect current;
    uint32_t start_time = 0;
    bool active = false;
};

// Rumble functions
void rumble_start(int port, const RumbleEffect &effect);
void rumble_stop(int port);
void rumble_update(int port, uint32_t now_ms, uint16_t &out_left, uint16_t &out_right);

// Load config from XBE (called when XBE is mapped)
bool load_from_xbe(const char *xbe_path);

} // namespace input_detail