// The device object, and the state the HLE is the source of truth for.
//
// Two different things live here, and keeping them apart is the point of the split:
//
//   * The **device object** is 0x24A8 bytes the console had. Game code pokes fields of it directly -- the
//     render target at 0x1A04, the swap count at 0x2478, the gamma ramp at 0x1E04 -- so the loader keeps
//     those bytes byte-for-byte where the game expects them and never reinterprets the layout.
//   * The **state** is what those bytes *mean*: which surface is bound, which textures, which transforms.
//     Nothing outside this layer needs it, and nothing inside it should need the host graphics API.
//
// The addresses in xd3d8_posix.h are where each of these lives inside the game image. That is only a fact
// about the image, not about our memory: until the image is mapped (A6) the same shapes are kept in
// ordinary storage and the arrays are reached through the accessors below. bind_image_arrays() is what
// repoints them at the real thing, and it is deliberately the only place where the two meet.
#pragma once

#include "d3d/xd3d8_posix.h"

// The console's calling convention, spelled the way this toolchain spells it. Not decoration: every HLE
// thunk calls its implementation and then pops its own arguments off the stack, so an implementation that
// was not __stdcall corrupts the game's frame.
#define XD3D_STDCALL __attribute__((stdcall))

namespace xd3d {

// HRESULT values the Xbox D3D hands back. Spelled out here rather than pulled from a Windows header,
// because there is no d3d8.h to pull them from. E_POINTER is the only failure an entry point can usefully
// report for a null argument, and the console does report it -- a game that passes null is not the case to
// paper over.
constexpr uint32_t D3D_OK = 0;
constexpr uint32_t D3D_E_POINTER = 0x80004003;

// The state we own. Kept in one place so that "what does the loader think is bound" has one answer.
struct DeviceState {
    Surface *render_target = nullptr;
    Surface *depth_stencil = nullptr;
    Resource *textures[TEXTURESTAGE_COUNT] = {};
    Palette *palettes[TEXTURESTAGE_COUNT] = {};
    struct Stream {
        VertexBuffer *buffer = nullptr;
        uint32_t stride = 0;
    } streams[16];

    uint32_t vertex_shader = 0; // the handle the game passed, not an index into anything of ours
    Matrix transforms[TRANSFORM_COUNT] = {};
    Viewport viewport = {};          // what is in effect
    Viewport requested_viewport = {}; // what the game asked for, before it is fitted to the target
    float screen_offset[2] = {0.0f, 0.0f};

    uint32_t swap_count = 0;
    uint32_t frame_count = 0;

    // The window the loader opened, which is what GetDisplayMode reports back: the game uses the answer to
    // decide how big its back buffer should be, so it has to be the real size rather than the TV mode.
    uint32_t display_width = 640;
    uint32_t display_height = 480;
};

DeviceState &state();

// ---- the device object ----

// The console's object. Until the image is mapped this is a static of the right size and shape, so the
// same code paths run with and without an image -- the alternative is a second set of tests that only
// exercise the easy case.
DeviceObject *device_object();

// Point at the image's own object. The object's address is the game's ADDR_DEVICE_OBJECT, and the game
// writes fields of it, so from here on the bytes are the game's.
void device_object_attach(DeviceObject *object);

// A field of the device object by offset, typed by the caller. This is the same accessor the Windows port
// used (`template <typename T> T &dev_field(uint32_t off)`) and it stays that way: the offsets are facts
// about the console, and naming the type at each use is what makes them checkable.
template <class T> T &dev_field(uint32_t off)
{
    return *(T *)(device_object()->bytes + off);
}

// ---- the arrays the game writes in place ----
//
// D3D_g_RenderState, D3D_g_DeferredTextureState and friends are not ours: game code stores into them
// directly, which is why the HLE reads them where they are instead of intercepting the write. With the
// image mapped they are simply at those addresses; until then these accessors name storage of the same
// shape.
uint32_t *render_state();
uint32_t *texture_state(int stage);
uint32_t *stream_source(int stream);

// The game's address for each of those arrays, so the loader can report where the state really is and can
// repoint the accessors once the image exists.
constexpr uint32_t ADDR_RENDERSTATE_ARRAY = ADDR_RENDERSTATE;
constexpr uint32_t ADDR_TEXTURESTATE_ARRAY = ADDR_TEXTURESTATE;

// Flush anything the loader has queued for the GPU. Nothing is queued yet -- the draw path is A4 -- so this
// is a named no-op rather than a missing call: the Xbox D3D flushes before state changes for a reason
// (the push buffer must not observe state mid-frame), and every implementation that would flush calls it,
// so A4 has one place to make real.
void pb_flush();

} // namespace xd3d