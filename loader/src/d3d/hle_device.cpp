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

#include "gfx/gfx_vulkan.h"

#include "d3d/resource_posix.h"
#include "d3d/vshader_store_posix.h"

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

// Clear: the first row in this file that touches a graphics device rather than writing bytes the game reads.
//
// Three things make it more than a colour write, and each of them is a place the console's behaviour and
// ours differ:
//
//   * The flags are the console's mask, not D3D9's. XCLEAR_TARGET is 0xF0 -- a colour mask, not a bit -- so a
//     translation that mapped TARGET onto "the colour bit" would clear nothing at all and say nothing.
//   * A null rectangle list means the whole target, and a rectangle covering the whole target means the same
//     thing. The console treated them alike; treating a full-screen rectangle as a rectangle is what pushed
//     the work through a path this driver is known to break.
//   * The colour arrives as 0xAARRGGBB in one dword and leaves as four floats. The byte order is A8R8G8B8,
//     which is the console's, not D3D8's documented DXT format for the same name; getting it backwards
//     swaps red and blue on every clear, which reads as a wrong background rather than as an error.
uint32_t XD3D_STDCALL D3DDevice_Clear(uint32_t Count, const D3DRect *pRects, uint32_t Flags, uint32_t Color, float Z,
                                      uint32_t Stencil)
{
    pb_flush();
    (void)Stencil; // no stencil attachment is ever bound, so the flag has nothing to act on

    if (!(Flags & (XCLEAR_TARGET | XCLEAR_ZBUFFER | XCLEAR_STENCIL))) {
        // Nothing asked to be cleared. The console drew nothing and so does this; refusing would be wrong,
        // because a game that ORs several flags and clears none of them is legal.
        return D3D_OK;
    }

    const float rgba[4] = {
        (float)((Color >> 16) & 0xFF) / 255.0f,
        (float)((Color >> 8) & 0xFF) / 255.0f,
        (float)(Color & 0xFF) / 255.0f,
        (float)((Color >> 24) & 0xFF) / 255.0f,
    };

    bool ok = true;
    if (Flags & XCLEAR_TARGET) {
        if (!pRects || Count == 0) {
            ok = gfx::clear_color(rgba);
        } else {
            for (uint32_t i = 0; i < Count && ok; i++) {
                const D3DRect &r = pRects[i];
                // The console's rectangles are half-open, so the width is a subtraction rather than an
                // increment -- and a rectangle that comes out empty is not an error, it clears nothing.
                if (r.x2 <= r.x1 || r.y2 <= r.y1) continue;
                ok = gfx::clear_color_rect(rgba, (uint32_t)r.x1, (uint32_t)r.y1, (uint32_t)(r.x2 - r.x1),
                                           (uint32_t)(r.y2 - r.y1));
            }
        }
    }
    if (ok && (Flags & XCLEAR_ZBUFFER)) ok = gfx::clear_depth(Z);
    if (!ok) logf("[d3d] Clear: %s", gfx::last_error());
    return ok ? D3D_OK : D3DERR_NOTAVAILABLE;
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
    // Whatever this stage held may have been released to zero while it was here, and kept only because
    // freeing it then would have been a use-after-free. It comes off the stage now, so it can go.
    Resource *previous = state().textures[Stage];
    state().textures[Stage] = pTexture;
    dev_field<Resource *>(DEV_TEXTURES + Stage * 4) = pTexture;
    if (previous && previous != pTexture) resource_release_if_unbound(previous);
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

// ---- direct3d: adapters and modes ----

uint32_t XD3D_STDCALL Direct3D_GetAdapterModeCount(uint32_t Adapter)
{
    // One adapter is one Xbox, so the count does not depend on which one was asked for. The argument is
    // named in the table because the thunk must match the spec; the value is not used.
    (void)Adapter;
    return (uint32_t)(sizeof kModes / sizeof kModes[0]);
}

uint32_t XD3D_STDCALL Direct3D_EnumAdapterModes(uint32_t Adapter, uint32_t Mode, DisplayMode *pMode)
{
    (void)Adapter;
    // Out of range is refused, not clamped: a caller that asked for mode 7 of a one-mode adapter has a
    // bug, and handing it mode 0 would hide it until something else went wrong.
    if (Mode >= sizeof kModes / sizeof kModes[0]) return D3DERR_INVALIDCALL;
    if (!pMode) return D3D_E_POINTER;
    *pMode = kModes[Mode];
    return D3D_OK;
}

// ---- display ----

// D3DFIELD_STATUS is {Field, VBlankCount}, and the fields alternate: 1 is odd, 2 is even. The count is
// reported as the field number, which is what the console did -- a caller that only polls the parity
// gets the same answer either way, and one that reads the count still gets a moving value.
void XD3D_STDCALL D3DDevice_GetDisplayFieldStatus(uint32_t *pFieldStatus)
{
    if (!pFieldStatus) return;
    const uint32_t vb = state().vblank_count;
    pFieldStatus[0] = (vb & 1u) ? 1u : 2u;
    pFieldStatus[1] = vb;
}

// D3DDevice_Swap: the console's end-of-frame. It flushes the push buffer, paces to the vblank,
// presents the back buffer, and updates the swap count. The loader's version does the CPU-side
// work (swap count, fence) and leaves the actual GPU present to the platform layer's
// window_end_frame() which calls gfx::swapchain_present().
uint32_t XD3D_STDCALL D3DDevice_Swap(uint32_t Flags)
{
    (void)Flags;
    logf("[d3d] Swap");
    pb_flush();

    // Increment swap count in device object and state.
    dev_field<uint32_t>(DEV_SWAPCOUNT)++;
    state().swap_count++;

    // The actual present is done by window_end_frame() -> gfx::swapchain_present().
    // This function returns immediately; the platform layer handles vblank pacing.
    return 0;
}

// Direct3D_CreateDevice: the entry point the game calls to get a device. The console's implementation
// opens a window, creates a D3D9 device, allocates the push buffer, builds the device object at its
// fixed address, creates the back buffer and depth surface, initialises all render state to the console's
// defaults, and starts the vblank thread. The loader's version does the same logical setup but uses the
// platform window (already open from --window) and the Vulkan swapchain (already wired through gfx).
uint32_t XD3D_STDCALL Direct3D_CreateDevice(uint32_t Adapter, uint32_t DeviceType, uint32_t hFocusWindow,
                                            uint32_t BehaviorFlags, PresentParameters *pp, uint8_t **ppDevice)
{
    if (!pp) return D3D_E_POINTER;
    logf("[d3d] Direct3D_CreateDevice(adapter %u, type %u, flags 0x%x): %ux%u fmt 0x%x x%u, ms 0x%x, swap %u, depth %u fmt 0x%x, "
         "flags 0x%x, rate %u, interval 0x%x",
         Adapter, DeviceType, BehaviorFlags, pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount,
         pp->MultiSampleType, pp->SwapEffect, pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat, pp->Flags,
         pp->FullScreen_RefreshRateInHz, pp->FullScreen_PresentationInterval);

    // The game asks for a back buffer size; we use that for the device's logical size.
    // The actual swapchain size comes from the window (A7).
    uint32_t width = pp->BackBufferWidth ? pp->BackBufferWidth : 640;
    uint32_t height = pp->BackBufferHeight ? pp->BackBufferHeight : 480;

    state().display_width = width;
    state().display_height = height;

    // Create the swapchain. The window is already open from --window; gfx::init() was called before
    // we get here. swapchain_create() will use the window's size and the surface created by gfx::init().
    // preferred_format = VK_FORMAT_UNDEFINED means "pick something reasonable"; immediate = false means
    // present mode FIFO (vblank-paced).
    if (!gfx::swapchain_create(VK_FORMAT_UNDEFINED, false)) {
        logf("[d3d] CreateDevice: swapchain create failed: %s", gfx::last_error());
        return D3DERR_NOTAVAILABLE;
    }

    // The console allocates the device object at a fixed address (0x00279B50) and the push buffer at
    // another fixed address. The loader keeps the same layout in g_device_object (device_posix.cpp).
    // We initialise the fields the game reads directly.
    DeviceObject *dev = device_object();
    memset(dev->bytes, 0, DEVICE_SIZE);

    // Push buffer pointers (placeholder; real push buffer is A6)
    dev_field<uint32_t *>(DEV_PUT) = (uint32_t *)0x00200000;
    dev_field<uint32_t *>(DEV_THRESHOLD) = (uint32_t *)0x00210000;

    // Back buffer scale (read by GetBackBufferScale)
    dev_field<float>(0x096C) = 1.0f;
    dev_field<float>(0x0970) = 1.0f;

    // Render target and depth stencil will be set when the game calls SetRenderTarget.
    // For now, leave null -- the game will call SetRenderTarget after CreateDevice.

    // Viewport: the game will call SetViewport, but we set the default to the back buffer size.
    Viewport vp = {0, 0, width, height, 0.0f, 1.0f};
    state().viewport = vp;
    state().requested_viewport = vp;
    memcpy(dev->bytes + DEV_VIEWPORT, &vp, sizeof(Viewport));

    // Transforms: the library starts every transform as identity.
    for (uint32_t i = 0; i < TRANSFORM_COUNT; i++) {
        Matrix m = {};
        m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
        state().transforms[i] = m;
        memcpy(dev->bytes + DEV_TRANSFORMS + i * sizeof(Matrix), &m, sizeof(Matrix));
    }

    // Screen space offset (GetViewportOffsetAndScale reads this)
    dev_field<float>(DEV_SCREENOFFSET_X) = 0.0f;
    dev_field<float>(DEV_SCREENOFFSET_Y) = 0.0f;

    // Swap count (incremented at Swap)
    dev_field<uint32_t>(DEV_SWAPCOUNT) = 0;
    state().swap_count = 0;

    // Presentation interval (used by the pacing logic)
    render_state()[XRS_PRESENTATIONINTERVAL] = pp->FullScreen_PresentationInterval;

    // The game expects ppDevice to point at the device object.
    if (ppDevice) *ppDevice = dev->bytes;

    logf("[d3d] device object %p, push buffer placeholder, back buffer %ux%u, presentation interval %u",
         dev->bytes, width, height, pp->FullScreen_PresentationInterval);

    return D3D_OK;
}

// Release on the device is a call the game makes once at shutdown. The device is the singleton at
// ADDR_DEVICE_OBJECT and lives until the process ends, so there is nothing to free; returning 1 keeps
// the reference count the caller expects without pretending otherwise.
uint32_t XD3D_STDCALL D3DDevice_Release()
{
    return 1;
}

// ---- push buffer ----

// MakeRequestedSpace: hand back where the caller may write. The HLE queues nothing, so the answer is
// simply the write pointer -- but it is read from the device object rather than from our own shadow,
// because that is where the game expects the cursor to live.
uint32_t *XD3D_STDCALL D3D_MakeRequestedSpace(uint32_t MinimumSpace, uint32_t RequestedSpace)
{
    (void)MinimumSpace;
    (void)RequestedSpace;
    pb_flush();
    pb_reset();
    return dev_field<uint32_t *>(DEV_PUT);
}

// BeginPush asks the same question with room for a whole primitive. The threshold test is kept because it
// is what decides whether the buffer is rewound first: a caller handed a pointer with less than
// Count + 16 dwords left would run past the guard the console keeps there.
uint32_t *XD3D_STDCALL D3DDevice_BeginPush(uint32_t Count)
{
    uint32_t *put = dev_field<uint32_t *>(DEV_PUT);
    uint32_t *threshold = dev_field<uint32_t *>(DEV_THRESHOLD);
    // Both are null until the device exists. Comparing them then would be a comparison of two unrelated
    // pointers, so the test is skipped and the null pointer is handed back as the honest answer.
    if (put && threshold && put + Count + 16 >= threshold) {
        pb_flush();
        pb_reset();
        put = dev_field<uint32_t *>(DEV_PUT);
    }
    return put;
}

// EndPush: the caller's pointer IS the new write cursor, so it is stored before the flush. Flushing first
// would emit whatever the previous primitive left behind, under a cursor the game has already moved.
void XD3D_STDCALL D3DDevice_EndPush(uint32_t *pPush)
{
    dev_field<uint32_t *>(DEV_PUT) = pPush;
    pb_flush();
}

void XD3D_STDCALL D3DDevice_End()
{
    pb_flush();
}

// ---- the viewport transform ----

// NV2A screen space: screen = clip / w * scale + offset. The depth scale is the D24 range (16777215),
// which is a constant here and not a format lookup: the console's z buffer is D24 whatever the colour
// format is, so asking the depth format here would be asking the wrong question.
void viewport_transform(float *pScale, float *pOffset)
{
    const Viewport &v = state().viewport;
    const float zscale = 16777215.0f;
    const float sx = state().screen_offset[0], sy = state().screen_offset[1];
    pScale[0] = v.Width * 0.5f;
    pScale[1] = -(v.Height * 0.5f);
    pScale[2] = (v.MaxZ - v.MinZ) * zscale;
    pScale[3] = 1.0f;
    pOffset[0] = v.X + v.Width * 0.5f + sx;
    pOffset[1] = v.Y + v.Height * 0.5f + sy;
    pOffset[2] = v.MinZ * zscale;
    pOffset[3] = 0.0f;
}

// Scale first, offset second, even though the transform reads "clip * scale + offset". That argument
// order is the console's and the game's; reversing it would mirror the image about both axes.
void XD3D_STDCALL D3DDevice_GetViewportOffsetAndScale(float *pOffset, float *pScale)
{
    if (!pOffset || !pScale) return;
    viewport_transform(pScale, pOffset);
}

// FUN_0026eaf0: game code that poked the screen-space offset fields directly calls this to have the
// transform rebuilt. The HLE derives the transform when it is read, so there is nothing to push and the
// pointer comes back unchanged -- which is what the invoice returned too.
uint32_t *XD3D_STDCALL D3D_WriteViewportTransform(uint32_t *pPush)
{
    return pPush;
}

// ---- palettes and streams ----

void XD3D_STDCALL D3DDevice_SetPalette(uint32_t Stage, Palette *pPalette)
{
    if (Stage >= TEXTURESTAGE_COUNT) return;
    state().palettes[Stage] = pPalette;
}

void XD3D_STDCALL D3DDevice_SetStreamSource(uint32_t StreamNumber, VertexBuffer *pStreamData, uint32_t Stride)
{
    if (StreamNumber >= 16) return;
    pb_flush();
    state().streams[StreamNumber].buffer = pStreamData;
    state().streams[StreamNumber].stride = Stride;
}

// ---- the four single-state render entry points ----
//
// Each is one render-state index with the index baked into its own function, because the original
// library compiled them that way and the game calls each by its own address. A macro rather than four
// near-identical bodies: the four differ only in the index they write, and a hand-copied body is exactly
// where a name and an index drift apart.

#define RS_SETTER(fn, index)                 \
    void XD3D_STDCALL fn(uint32_t Value)     \
    {                                        \
        pb_flush();                          \
        render_state()[index] = Value;       \
    }
RS_SETTER(D3DDevice_SetRenderState_NormalizeNormals, XRS_NORMALIZENORMALS)
RS_SETTER(D3DDevice_SetRenderState_StencilEnable, XRS_STENCILENABLE)
RS_SETTER(D3DDevice_SetRenderState_StencilFail, XRS_STENCILFAIL)
RS_SETTER(D3DDevice_SetRenderState_TextureFactor, XRS_TEXTUREFACTOR)
#undef RS_SETTER

void XD3D_STDCALL D3DDevice_SetTextureStageStateNotInline(uint32_t Stage, uint32_t Type, uint32_t Value)
{
    pb_flush();
    if (Stage < TEXTURESTAGE_COUNT && Type < 32) texture_state((int)Stage)[Type] = Value;
}

// FUN_002709a0: the original library's lazy flush, which processed dirty flags here. The HLE applies
// state when a draw is issued instead, so there is nothing to do. Doing nothing is the correct
// behaviour, not a stub: a draw reads whatever the arrays hold at the moment it is issued.
void XD3D_STDCALL D3D_FlushDirtyState() {}

// FUN_002695f0 (esi = device): the fog and vertex-shader state upload. Fog reaches the hardware as two
// render states the game sets itself, so there is nothing to upload here either.
void XD3D_STDCALL D3D_UpdateFogState() {}

// ---- shaders and their constants ----

// CreateVertexShader: the program and its declaration are copied out of the game's buffers and kept, and
// the handle is an index into that shelf with bit 0 set. The convention is the console's and the game relies
// on it: a handle with bit 0 clear is a fixed-function vertex format, set by passing the FVF straight to
// SetVertexShader, and any scheme of ours that claimed bit 0 for itself would turn every fixed-function
// draw into a shader lookup that misses.
//
// What is deliberately not here: the translation. It happens per draw rather than per creation, because the
// pipeline that consumes the SPIR-V also depends on the render target's format and the render states, and
// caching a translation at creation would be a cache with no idea what it is for. The microcode is copied
// all the same, because the game owns both buffers and may reuse them.
uint32_t XD3D_STDCALL D3DDevice_CreateVertexShader(const uint32_t *pDeclaration, const uint32_t *pFunction,
                                                  uint32_t *pHandle, uint32_t Usage)
{
    if (!pHandle) return D3D_E_POINTER;
    const char *why = nullptr;
    const uint32_t handle = vertex_program_create(pDeclaration, pFunction, Usage, &why);
    if (!handle) {
        logf("[d3d] CreateVertexShader refused: %s", why ? why : "?");
        return D3DERR_INVALIDCALL;
    }
    *pHandle = handle;
    return D3D_OK;
}

// Delete still has to clear the binding: a deleted shader left bound means the next draw reads a shader
// that is no longer there. The handle itself is released in A5.
void XD3D_STDCALL D3DDevice_DeleteVertexShader(uint32_t Handle)
{
    if (state().vertex_shader == Handle) state().vertex_shader = 0;
    vertex_program_destroy(Handle);
}

// One register is four floats, so a register's worth is 16 bytes and `count` is in registers. A register
// outside the bank is dropped rather than clamped: 192 is where the XDK's shader epilogue stops, and a
// clamped write would land somewhere the game never wrote.
static void set_vs_constants(int Slot, const float *pData, uint32_t Count)
{
    pb_flush();
    if (!pData) return;
    for (uint32_t i = 0; i < Count; i++) {
        const int r = Slot + (int)i;
        if (r >= 0 && r < (int)VS_CONSTANT_COUNT) memcpy(state().vs_constants[r], pData + 4 * i, 16);
    }
}

void XD3D_STDCALL D3DDevice_SetVertexShaderConstant1(int Register, const float *pConstantData)
{
    set_vs_constants(Register, pConstantData, 1);
}

void XD3D_STDCALL D3DDevice_SetVertexShaderConstant1Fast(int Register, const float *pConstantData)
{
    set_vs_constants(Register, pConstantData, 1);
}

void XD3D_STDCALL D3DDevice_SetVertexShaderConstant4(int Register, const float *pConstantData)
{
    set_vs_constants(Register, pConstantData, 4);
}

// The count here is in DWORDs, not registers: FUN_002699e0 pushes that many with SET_TRANSFORM_CONSTANT.
// Read as a register count it writes four times too far, overwriting the following slots with memory past
// the caller's data -- the invoice records losing three slots that way.
void XD3D_STDCALL D3DDevice_SetVertexShaderConstantNotInlineFast(int Register, const float *pConstantData,
                                                                 uint32_t DwordCount)
{
    set_vs_constants(Register, pConstantData, DwordCount / 4);
}

// The console kept a bound pixel shader's program in render states 0-56, which game code patches in
// place. Binding is therefore a copy into those states, not a pointer the loader dereferences later. A5
// does the translation, so there is nothing to copy in yet -- but the handle is stored regardless, so
// DeletePixelShader and SetPixelShaderConstant see the same binding the game believes is current.
void XD3D_STDCALL D3DDevice_SetPixelShader(uint32_t Handle)
{
    pb_flush();
    state().pixel_shader = Handle;
}

void XD3D_STDCALL D3DDevice_DeletePixelShader(uint32_t Handle)
{
    if (state().pixel_shader == Handle) state().pixel_shader = 0;
}

void XD3D_STDCALL D3DDevice_SetPixelShaderConstant(uint32_t Register, const float *pConstantData,
                                                   uint32_t ConstantCount)
{
    pb_flush();
    if (!pConstantData) return;
    for (uint32_t i = 0; i < ConstantCount; i++) {
        const uint32_t r = Register + i;
        if (r < PS_CONSTANT_COUNT) memcpy(state().ps_constants[r], pConstantData + 4 * i, 16);
    }
}

// ---- fences and time ----

// A fence on this console completed when it was asked for, because the HLE does the work when it is
// asked. So the token is a counter, and the device object's copy at DEV_GPUTIME is updated to match --
// game code reads that field directly rather than through this function.
uint32_t XD3D_STDCALL D3D_SetFence(uint32_t Flags)
{
    (void)Flags;
    pb_flush();
    ++state().gpu_time;
    dev_field<uint32_t>(DEV_GPUTIME) = state().gpu_time;
    return state().gpu_time;
}

uint32_t XD3D_STDCALL D3DDevice_IsFencePending(uint32_t Fence)
{
    (void)Fence;
    return 0; // nothing is ever outstanding
}

// BlockOnTime is the console's "wait until this timestamp". Nothing here is deferred, so it returns.
void XD3D_STDCALL D3D_BlockOnTime(uint32_t Time, uint32_t MakeSpace)
{
    (void)Time;
    (void)MakeSpace;
}

// ---- the table ----

#define H(addr, fn, params, ret) { addr, #fn, (void *)&fn, params, ret }

// Every row here is one console entry point: the address the game calls, the spec the thunk has to
// match, and whether it hands a value back. The order and the addresses are facts about the image, so
// they are copied from the invoice's own table rather than written from memory.
//
// Four invoice rows are absent on purpose: they take a structure this port cannot name yet.
// D3DDevice_SetMaterial, D3DDevice_SetLight and D3DDevice_LightEnable (D3DMATERIAL9 / D3DLIGHT9)
// and D3DDevice_SetScissors (D3DRECT). The invoice took all three types from Windows d3d9 headers,
// which a Linux i386 build does not have -- and the console's own D3D8 layouts are not something to
// type from memory: getting a size wrong means copying past the end of whatever the game allocated.
// They stay out until the image is available and their layout can be read off it, which is A4/B1 work.
// Saying so here is the point: a caller the game makes to an address with no thunk is reported by the
// dispatcher, so the gap is visible while it lasts.
static const Hle kDeviceTable[] = {
    // ---- Direct3D: adapters and device creation ----
    H(0x00277580, Direct3D_GetAdapterModeCount, "s", true),
    H(0x00277640, Direct3D_EnumAdapterModes, "s s s", true),
    H(0x0026c620, Direct3D_CreateDevice, "s s s s s s", true),

    // ---- device lifetime and display ----
    H(0x0026b5d0, D3DDevice_Release, "", true),
    H(0x0026a320, D3DDevice_GetDisplayMode, "s", true),
    H(0x0026b610, D3DDevice_GetDisplayFieldStatus, "s", false),

    // ---- push buffer ----
    H(0x0026cdc0, D3D_MakeRequestedSpace, "s eax", true),
    H(0x0026d110, D3DDevice_BeginPush, "s", true),
    H(0x0026d170, D3DDevice_EndPush, "s", false),
    H(0x0026a1f0, D3DDevice_End, "", false),
    H(0x002773a0, D3DDevice_Swap, "s", true),
    H(0x00268880, D3DDevice_Clear, "s s s s s s", true),

    // ---- render targets ----
    H(0x0026a420, D3DDevice_SetRenderTarget, "s s", true),
    H(0x0026aa60, D3DDevice_GetRenderTarget2, "", true),
    H(0x0026aaa0, D3DDevice_GetDepthStencilSurface2, "", true),

    // ---- transforms, viewport, material ----
    H(0x0026aae0, D3DDevice_SetTransform, "s s", false),
    H(0x0026ac00, D3DDevice_SetViewport, "s", false),
    H(0x0026e9c0, D3DDevice_GetViewportOffsetAndScale, "s s", false),
    H(0x0026ba50, D3DDevice_SetScreenSpaceOffset, "s s", false),
    H(0x0026eaf0, D3D_WriteViewportTransform, "eax", true),

    // ---- textures, palettes, streams ----
    H(0x0026b370, D3DDevice_SetTexture, "s s", false),
    H(0x0026b530, D3DDevice_SetPalette, "s s", false),
    H(0x00268de0, D3DDevice_SetStreamSource, "s s s", false),

    // ---- render state and texture-stage state ----
    H(0x0026e5e0, D3DDevice_SetRenderStateNotInline, "s s", false),
    H(0x0026e5b0, D3DDevice_SetRenderState_Simple, "ecx edx", false),
    H(0x0026dd60, D3DDevice_SetRenderState_NormalizeNormals, "s", false),
    H(0x0026de50, D3DDevice_SetRenderState_StencilEnable, "s", false),
    H(0x0026df00, D3DDevice_SetRenderState_StencilFail, "s", false),
    H(0x0026df80, D3DDevice_SetRenderState_TextureFactor, "s", false),
    H(0x0026e860, D3DDevice_SetTextureStageStateNotInline, "s s s", false),
    H(0x002709a0, D3D_FlushDirtyState, "", false),
    H(0x002695f0, D3D_UpdateFogState, "", false),

    // ---- vertex shaders ----
    H(0x00269480, D3DDevice_CreateVertexShader, "s s s s", true),
    H(0x002695d0, D3DDevice_DeleteVertexShader, "s", false),
    H(0x00269710, D3DDevice_SetVertexShader, "s", false),
    H(0x002690c0, D3D_SelectVertexShader, "eax", false),
    H(0x00269880, D3DDevice_SetVertexShaderConstant1, "ecx edx", false),
    H(0x002698e0, D3DDevice_SetVertexShaderConstant1Fast, "ecx edx", false),
    H(0x00269930, D3DDevice_SetVertexShaderConstant4, "ecx edx", false),
    H(0x002699e0, D3DDevice_SetVertexShaderConstantNotInlineFast, "ecx edx s", false),

    // ---- pixel shaders ----
    H(0x0026bd00, D3DDevice_SetPixelShader, "s", false),
    H(0x0026bce0, D3DDevice_DeletePixelShader, "s", false),
    H(0x0026bf10, D3DDevice_SetPixelShaderConstant, "ecx s eax", false),

    // ---- fences and time ----
    H(0x0026c920, D3D_SetFence, "s", true),
    H(0x0026d0f0, D3DDevice_IsFencePending, "s", true),
    H(0x0026cc10, D3D_BlockOnTime, "s s", false),
};

#undef H

const Hle *device_rows(size_t *count)
{
    *count = sizeof kDeviceTable / sizeof kDeviceTable[0];
    return kDeviceTable;
}

} // namespace xd3d