// Input detail implementation: deadzone, sensitivity curve, rumble pattern.
#include "input/input_detail_posix.h"

#include "platform/posix/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace input_detail {

namespace {

// Default config for BLACK FPS (tuned for typical PC play)
GamepadConfig g_default_config = {
    // Left stick: movement
    {0.15f, 1.0f, 0.0f, 1.0f, false},   // left_stick
    {0.12f, 1.2f, 0.1f, 1.0f, true},    // right_stick: higher sens, slight accel, inverted Y
    {0.10f, 1.0f, 1.0f},                // left_trigger
    {0.10f, 1.0f, 1.0f},                // right_trigger

    // Rumble effects (tuned for BLACK)
    {0, 60000, 150, 0, 100},            // recoil: strong high-freq, short
    {40000, 60000, 500, 50, 200},       // explosion: both motors, longer
    {20000, 40000, 200, 0, 100},        // damage: medium both
    {30000, 20000, 1000, 100, 300},     // vehicle: low-freq dominant, long
    {10000, 30000, 1000, 200, 500},     // heartbeat: slow pulse high-freq
};

GamepadConfig g_config = g_default_config;
RumbleState g_rumble[4] = {};

} // namespace

const GamepadConfig &get_config() { return g_config; }

float apply_deadzone_axis(float value, const StickConfig &cfg)
{
    if (std::abs(value) < cfg.deadzone) return 0.0f;
    // Rescale to preserve range
    float sign = value >= 0.0f ? 1.0f : -1.0f;
    float abs_val = std::abs(value);
    float scaled = (abs_val - cfg.deadzone) / (1.0f - cfg.deadzone);
    return sign * std::clamp(scaled * cfg.sensitivity, 0.0f, cfg.max_output);
}

void apply_deadzone_stick(float &x, float &y, const StickConfig &cfg)
{
    float mag = std::sqrt(x * x + y * y);
    if (mag < cfg.deadzone) {
        x = y = 0.0f;
        return;
    }
    // Radial deadzone: preserve direction, scale magnitude
    float scale = (mag - cfg.deadzone) / (1.0f - cfg.deadzone) / mag;
    x *= scale * cfg.sensitivity;
    y *= scale * cfg.sensitivity;
    // Clamp to max_output
    float new_mag = std::sqrt(x * x + y * y);
    if (new_mag > cfg.max_output) {
        float clamp_scale = cfg.max_output / new_mag;
        x *= clamp_scale;
        y *= clamp_scale;
    }
}

float apply_trigger_curve(float value, const TriggerConfig &cfg)
{
    if (value < cfg.deadzone) return 0.0f;
    float scaled = (value - cfg.deadzone) / (1.0f - cfg.deadzone);
    if (cfg.curve_power != 1.0f) {
        scaled = std::pow(scaled, cfg.curve_power);
    }
    return std::clamp(scaled * cfg.sensitivity, 0.0f, 1.0f);
}

float apply_sensitivity(float value, const StickConfig &cfg)
{
    if (cfg.acceleration == 0.0f) return value * cfg.sensitivity;
    float sign = value >= 0.0f ? 1.0f : -1.0f;
    float abs_val = std::abs(value);
    return sign * std::pow(abs_val, 1.0f + cfg.acceleration) * cfg.sensitivity;
}

void sdl_axis_to_norm(int16_t sdl_x, int16_t sdl_y, float &out_x, float &out_y, const StickConfig &cfg)
{
    // SDL axis is -32768..32767, convert to -1.0..1.0
    float x = static_cast<float>(sdl_x) / 32767.0f;
    float y = static_cast<float>(sdl_y) / 32767.0f;
    // Clamp to [-1, 1] in case of 32768
    x = std::clamp(x, -1.0f, 1.0f);
    y = std::clamp(y, -1.0f, 1.0f);
    // Apply config
    apply_deadzone_stick(x, y, cfg);
    if (cfg.invert) y = -y;
    out_x = std::clamp(x, -1.0f, 1.0f);
    out_y = std::clamp(y, -1.0f, 1.0f);
}

float sdl_trigger_to_norm(uint16_t sdl_val, const TriggerConfig &cfg)
{
    // SDL trigger is 0..32767 (or sometimes 0..65535, handle both)
    float norm = static_cast<float>(sdl_val) / 32767.0f;
    if (norm > 1.0f) norm = static_cast<float>(sdl_val) / 65535.0f;
    return apply_trigger_curve(std::clamp(norm, 0.0f, 1.0f), cfg);
}

void rumble_start(int port, const RumbleEffect &effect)
{
    if (port < 0 || port >= 4) return;
    g_rumble[port].current = effect;
    g_rumble[port].start_time = 0; // will be set on first update
    g_rumble[port].active = true;
    logf("[input] rumble start port %d: low=%u high=%u dur=%u",
         port, effect.low_freq, effect.high_freq, effect.duration_ms);
}

void rumble_stop(int port)
{
    if (port < 0 || port >= 4) return;
    g_rumble[port].active = false;
    g_rumble[port].current = {};
    logf("[input] rumble stop port %d", port);
}

void rumble_update(int port, uint32_t now_ms, uint16_t &out_left, uint16_t &out_right)
{
    out_left = 0;
    out_right = 0;
    if (port < 0 || port >= 4) return;
    RumbleState &rs = g_rumble[port];
    if (!rs.active) return;

    if (rs.start_time == 0) rs.start_time = now_ms;
    uint32_t elapsed = now_ms - rs.start_time;

    const RumbleEffect &fx = rs.current;

    // Check duration
    if (fx.duration_ms > 0 && elapsed >= fx.duration_ms) {
        rs.active = false;
        return;
    }

    float intensity = 1.0f;

    // Attack phase
    if (fx.attack_ms > 0 && elapsed < fx.attack_ms) {
        intensity = static_cast<float>(elapsed) / fx.attack_ms;
    }
    // Decay phase
    else if (fx.decay_ms > 0 && fx.duration_ms > 0 && elapsed >= fx.duration_ms - fx.decay_ms) {
        intensity = 1.0f - static_cast<float>(elapsed - (fx.duration_ms - fx.decay_ms)) / fx.decay_ms;
    }

    intensity = std::clamp(intensity, 0.0f, 1.0f);

    out_left = static_cast<uint16_t>(fx.low_freq * intensity);
    out_right = static_cast<uint16_t>(fx.high_freq * intensity);
}

bool load_from_xbe(const char *xbe_path)
{
    // TODO: Parse XBE for input config
    // XBE may contain:
    // - Deadzone/sensitivity in game settings section
    // - Rumble patterns in effects section
    // - Per-vehicle/weapon configs
    // For now, return false to indicate not implemented
    (void)xbe_path;
    logf("[input] XBE config loading not yet implemented, using defaults");
    return false;
}

} // namespace input_detail