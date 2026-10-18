#include "d3d/device_posix.h"

#include <string.h>

namespace xd3d {
namespace {

// The fallback storage for each thing that normally lives inside the image. One per array, sized by the
// ABI, and named after what it stands in for so a log line or a backtrace says which one was meant.
uint32_t g_render_state[RENDERSTATE_COUNT];
uint32_t g_texture_state[TEXTURESTAGE_COUNT][TEXTURESTATE_PER_STAGE];
uint32_t g_streams[16];
uint32_t *g_render_state_ptr = g_render_state;
uint32_t (*g_texture_state_ptr)[TEXTURESTATE_PER_STAGE] = g_texture_state;
uint32_t *g_streams_ptr = g_streams;

// The device object before the image is mapped: same size, same field offsets, somewhere else.
DeviceObject g_device_object;
DeviceObject *g_device_object_ptr = &g_device_object;

DeviceState g_state;

} // namespace

DeviceState &state() { return g_state; }

DeviceObject *device_object() { return g_device_object_ptr; }

void device_object_attach(DeviceObject *object) { g_device_object_ptr = object ? object : &g_device_object; }

uint32_t *render_state() { return g_render_state_ptr; }

uint32_t *texture_state(int stage)
{
    if (stage < 0 || (uint32_t)stage >= TEXTURESTAGE_COUNT) return nullptr;
    return g_texture_state_ptr[stage];
}

uint32_t *stream_source(int stream)
{
    if (stream < 0 || stream >= 16) return nullptr;
    return g_streams_ptr + stream * 2;
}

void pb_flush()
{
    // Nothing is queued before A4. See the note in the header: this is a placeholder with a reason, not a
    // stub nobody remembered.
}

} // namespace xd3d