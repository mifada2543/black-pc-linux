// The HLE implementations that are pure state: what the game sets, and what it reads back.
//
// Everything in this file is portable from the Windows port without a graphics device, because none of it
// talks to one: these functions write the console's own state -- the device object and the arrays game code
// reads in place -- and return what the game asked for. What the Windows port also did here, and what is
// deliberately *not* here yet, is the host half: binding a Vulkan render target, pushing NV2A state. That is
// A4, and it is a separate edit to each of these functions rather than a rewrite, because the part that
// decides what the game observes -- which bytes moved -- is already complete and testable here.
//
// The rules each one follows, all taken from the console rather than from taste:
//
//   * Write the state where the game will read it. The game pokes the device object and the state arrays
//     directly, so a shadow copy the game cannot see is not state, it is a comment.
//   * Ignore an index the console would have ignored (a transform past the tenth, a texture stage past the
//     fourth, a render state past XRS_MAX) instead of clamping it. Clamping turns a call the console
//     dropped into one it performed, and the two are not the same call.
//   * A Get* that hands back a resource adds a reference, because on this console it is returning a
//     counted object and the game will Release it.
#include "d3d/device_posix.h"

#include "d3d/hle_table_posix.h"

#include "platform/posix/log.h"

#include <string.h>

namespace xd3d {

// ---- display ----

// NTSC SDTV modes (standard AV pack). Flags 0x20 = interlaced, 0x100 = 10x11 pixel aspect ratio; the
// RenderWare driver in this game insists on an exact match of size, bit depth and flags with what the game
// asks for (640x480, 0x120), which is why the list is this short and this specific.
static const DisplayMode kModes[] = {
    {640, 480, 60, 0x120, XFMT_LIN_A8R8G8B8},
};

uint32_t XD3D_STDCALL D3DDevice_GetDisplayMode(DisplayMode *pMode)
{
    if (!pMode) return D3D_E_POINTER;
    *pMode = kModes[0];
    // The size is the window the loader opened, not the mode: the game calls this to find out how big the
    // back buffer is, and answering with the TV mode would misreport it the moment the window is not
    // 640x480. A7 sets these when it opens a window.
    pMode->Width = state().display_width;
    pMode->Height = state().display_height;
    return D3D_OK;
}

// ---- render targets ----

// SetRenderTarget(rt, z): the console takes both at once. Both land in the device object as well as in the
// shadow, because GetRenderTarget2 reads the object on some code paths and the game writes the object on
// others.
uint32_t XD3D_STDCALL D3DDevice_SetRenderTarget(Surface *pRenderTarget, Surface *pNewZStencil)
{
    pb_flush();
    if (pRenderTarget) {
        state().render_target = pRenderTarget;
        dev_field<Surface *>(DEV_RENDERTARGET) = pRenderTarget;
    }
    state().depth_stencil = pNewZStencil;
    dev_field<Surface *>(DEV_ZBUFFER) = pNewZStencil;
    // A null target is not "unbind": the console keeps rendering into the back buffer, which is what the
    // Windows port did, and what a game that clears at the top of each frame relies on.
    return D3D_OK;
}

// GetRenderTarget2 / GetDepthStencilSurface2: the "2" is the console's own suffix for the pair that
// returns a surface with a reference added. The increment is the whole reason these two exist.
Surface *XD3D_STDCALL D3DDevice_GetRenderTarget2()
{
    Surface *s = state().render_target;
    if (s) s->Common++;
    return s;
}

Surface *XD3D_STDCALL D3DDevice_GetDepthStencilSurface2()
{
    Surface *s = state().depth_stencil;
    if (s) s->Common++;
    return s;
}

// ---- transforms and viewport ----

// SetTransform(state, matrix): the console keeps ten, world through texture6. Texture7 is a name with
// nowhere to go -- the game can ask for it and the console drops it, so this drops it too rather than
// writing over the viewport that follows in the object.
void XD3D_STDCALL D3DDevice_SetTransform(uint32_t State, const Matrix *pMatrix)
{
    if (!pMatrix || State >= TRANSFORM_COUNT) return;
    pb_flush();
    state().transforms[State] = *pMatrix;
    memcpy(device_object()->bytes + DEV_TRANSFORMS + State * sizeof(Matrix), pMatrix, sizeof(Matrix));
}

// SetViewport: the game asks for the viewport it wants. Clamping it to the current render target -- which
// the Windows port did, and which this game needs, because it draws a 640x480 viewport once a frame and
// then draws into a 160x120 target expecting the whole view there -- is A4, where the target is known. What
// is decided here is that the request is kept as the request: requested_viewport is what was asked for,
// viewport is what is in effect, and until A4 they are the same value.
void XD3D_STDCALL D3DDevice_SetViewport(const Viewport *pViewport)
{
    if (!pViewport) return;
    pb_flush();
    state().requested_viewport = *pViewport;
    state().viewport = *pViewport;
    memcpy(device_object()->bytes + DEV_VIEWPORT, pViewport, sizeof(Viewport));
}

// SetScreenSpaceOffset(x, y): floats, not integers -- the fields at 0xEF8/0xEFC hold the offset the screen
// transform is built from, and this game uses fractional offsets.
void XD3D_STDCALL D3DDevice_SetScreenSpaceOffset(float x, float y)
{
    state().screen_offset[0] = x;
    state().screen_offset[1] = y;
    dev_field<float>(DEV_SCREENOFFSET_X) = x;
    dev_field<float>(DEV_SCREENOFFSET_Y) = y;
}

// ---- textures ----

void XD3D_STDCALL D3DDevice_SetTexture(uint32_t Stage, Resource *pTexture)
{
    if (Stage >= TEXTURESTAGE_COUNT) return;
    pb_flush();
    state().textures[Stage] = pTexture;
    dev_field<Resource *>(DEV_TEXTURES + Stage * 4) = pTexture;
}

// ---- vertex shader ----

void XD3D_STDCALL D3DDevice_SetVertexShader(uint32_t Handle)
{
    pb_flush();
    state().vertex_shader = Handle;
}

// The inline variant (FUN_002690c0), reached from code the compiler inlined into the game: the handle
// arrives in eax and carries bit 4 as part of its tag, which the console masked off before storing it.
void XD3D_STDCALL D3D_SelectVertexShader(uint32_t Handle)
{
    pb_flush();
    state().vertex_shader = Handle & ~0x10u;
}

// ---- render state ----

// The not-inline path: the caller wants the array updated, so update it.
void XD3D_STDCALL D3DDevice_SetRenderStateNotInline(uint32_t State, uint32_t Value)
{
    pb_flush();
    if (State < RENDERSTATE_COUNT) render_state()[State] = Value;
}

// The inline path (FUN_0026e5b0, "ecx edx" -- the LTCG shape): the game's own code has *already* written
// D3D_g_RenderState before calling, and all that is left is to emit the register write. So there is nothing
// to store here, and storing anything would undo a store the game already made.
void XD3D_STDCALL D3DDevice_SetRenderState_Simple(uint32_t Method, uint32_t Value)
{
    pb_flush();
    (void)Method;
    (void)Value;
}

// ---- the table ----

#define H(addr, fn, params, ret) { addr, #fn, (void *)&fn, params, ret }

static const Hle kDeviceTable[] = {
    H(0x0026a320, D3DDevice_GetDisplayMode, "s", true),
    H(0x0026a420, D3DDevice_SetRenderTarget, "s s", true),
    H(0x0026aa60, D3DDevice_GetRenderTarget2, "", true),
    H(0x0026aaa0, D3DDevice_GetDepthStencilSurface2, "", true),
    H(0x0026aae0, D3DDevice_SetTransform, "s s", false),
    H(0x0026ac00, D3DDevice_SetViewport, "s", false),
    H(0x0026ba50, D3DDevice_SetScreenSpaceOffset, "s s", false),
    H(0x0026b370, D3DDevice_SetTexture, "s s", false),
    H(0x0026e5e0, D3DDevice_SetRenderStateNotInline, "s s", false),
    H(0x0026e5b0, D3DDevice_SetRenderState_Simple, "ecx edx", false),
    H(0x00269710, D3DDevice_SetVertexShader, "s", false),
    H(0x002690c0, D3D_SelectVertexShader, "eax", false),
};

#undef H

const Hle *device_rows(size_t *count)
{
    *count = sizeof kDeviceTable / sizeof kDeviceTable[0];
    return kDeviceTable;
}

// The other two tables are A4's; declared here so a test that walks "all the tables" does not have to know
// that yet, and so it fails loudly rather than silently skipping them.
const Hle *resource_rows(size_t *count)
{
    *count = 0;
    return nullptr;
}

const Hle *draw_rows(size_t *count)
{
    *count = 0;
    return nullptr;
}

} // namespace xd3d