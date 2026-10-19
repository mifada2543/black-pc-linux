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

// D3D_g_pDevice before the image is mapped: a pointer of its own with the same meaning.
uint8_t *g_pdevice_storage = nullptr;
uint8_t **g_pdevice_slot_ptr = &g_pdevice_storage;

DeviceState g_state;

// Global frame counter (mirrors Windows g_frame_count in device.cpp)
uint32_t g_frame_count = 0;

} // namespace

DeviceState &state() { return g_state; }

DeviceObject *device_object() { return g_device_object_ptr; }

void device_object_attach(DeviceObject *object) { g_device_object_ptr = object ? object : &g_device_object; }

uint8_t **g_pdevice_slot() { return g_pdevice_slot_ptr; }

void state_attach_to_image()
{
    // Every address here is a fact about one known build, and each one is already written down next to the
    // layout constants it was derived with (xd3d8_posix.h). Nothing is computed: attaching to a different
    // image would be attaching to the wrong bytes, and the loader only hooks a table for this build.
    device_object_attach((DeviceObject *)(uintptr_t)ADDR_DEVICE_OBJECT);
    g_render_state_ptr = (uint32_t *)(uintptr_t)ADDR_RENDERSTATE;
    g_texture_state_ptr = (uint32_t (*)[TEXTURESTATE_PER_STAGE])(uintptr_t)ADDR_TEXTURESTATE;
    g_streams_ptr = (uint32_t *)(uintptr_t)ADDR_STREAMS;
    g_pdevice_slot_ptr = (uint8_t **)(uintptr_t)ADDR_G_PDEVICE;
}

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

uint32_t frame_count() { return g_frame_count; }
void frame_count_increment() { g_frame_count++; }

void pb_flush()
{
    // Nothing is queued before A4. See the note in the header: this is a placeholder with a reason, not a
    // stub nobody remembered.
}

void pb_reset()
{
    // Same reasoning as pb_flush(), and the same reason to exist as a named function: the console
    // rewinds its push buffer rather than growing it, and D3D_MakeRequestedSpace and D3DDevice_BeginPush
    // both call this. When A4 gives the buffer a real implementation, the rewind has to move the device
    // object's DEV_PUT cursor back to the base as well as reset the shadow -- which is why it is not left
    // implicit in the two callers.
}

} // namespace xd3d