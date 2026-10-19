// The resource half: what the game creates, locks, reads back and destroys.
//
// Eighteen HLE rows, ported from the Windows loader's resources.cpp, and the two things that file leaned
// on the kernel for and this one cannot. There was `MmAllocateContiguousMemoryEx` for a resource's pixels
// and `ExAllocatePoolWithTag` for its header; on Linux both are plain host memory, and `Data` holds a
// pointer rather than a physical address. That difference is small and it is not cosmetic: a 32-bit host
// pointer fits the field the console sized for a 32-bit physical address, so nothing about the game's view
// of the header changes.
//
// Two conventions from the Windows port are *not* carried over, and both were that port's own:
//
//   * `GetLevelInfo` returned `data | 0x80000000`, a flag the Windows loader invented to tell its own host
//     D3D9 bridge that the value was a virtual address, and `CreateSurfaceHeader` masked the top nibble back
//     off for the same reason. On real hardware neither bit exists, and the game may hand the value straight
//     back. So this returns a plain pointer and reads a plain pointer.
//   * `phys_to_virt` existed because the console allocated physical memory and the loader mapped it. There is
//     nothing to translate here, which is the only reason Lock can return the block itself.
//
// What is deliberately still missing is the other half of a texture: LockRect hands out a pointer and marks
// what the game wrote, but nothing copies those bytes into a Vulkan image yet. The dirty range is recorded
// for that step, and the step is A4's next slice, not this one's.
#pragma once

#include "d3d/device_posix.h" // XD3D_STDCALL

#include "d3d/xd3d8_posix.h"

#include <stdint.h>

namespace xd3d {

// D3D8's own two structures, from its published 32-bit ABI. Not guessed and not read out of memory: a
// locked-rect is two dwords and a rect is four, in both the console's headers and every document written
// about them since.
struct LockedRect {
    int32_t Pitch;
    void *pBits;
};
static_assert(sizeof(LockedRect) == 8, "a locked rect is a pitch and a pointer");

// Xbox D3DSURFACE_DESC, as the Windows loader wrote it: Format, Type, Usage, Size, MultiSampleType, Width,
// Height. The field order is the console's and the game reads the fields by position.
struct SurfaceDesc {
    uint32_t Format, Type, Usage, Size, MultiSampleType, Width, Height;
};
static_assert(sizeof(SurfaceDesc) == 28, "seven dwords");

// ---- the eighteen rows ----
//
// Declared here rather than only in the .cpp so a test can call one directly. The table does not need them
// -- it takes their addresses -- but calling through a generated thunk tests the thunk, and calling the
// function tests the resource. Both matter, and only one of them can say what went wrong.
XD3D_STDCALL Resource *D3DDevice_CreateTexture2(uint32_t Width, uint32_t Height, uint32_t Depth, uint32_t Levels,
                                                uint32_t Usage, uint32_t Format, uint32_t ResourceType);
XD3D_STDCALL Surface *D3D_CreateSurface2(uint32_t Width, uint32_t Height, uint32_t Usage, uint32_t Format);
XD3D_STDCALL Resource *D3DDevice_CreateVertexBuffer2(uint32_t Length);
XD3D_STDCALL void *D3DVertexBuffer_Lock2(Resource *pVB, uint32_t Flags);
XD3D_STDCALL void *D3DPalette_Lock2(Resource *pThis, uint32_t Flags);
XD3D_STDCALL uint32_t D3DResource_AddRef(Resource *pThis);
XD3D_STDCALL uint32_t D3DResource_Release(Resource *pThis);
XD3D_STDCALL void D3D_DestroyResource(Resource *pResource);
XD3D_STDCALL void D3DResource_BlockUntilNotBusy(Resource *pThis);
XD3D_STDCALL Surface *D3DTexture_GetSurfaceLevel2(PixelContainer *pThis, uint32_t Level);
XD3D_STDCALL Surface *D3DCubeTexture_GetCubeMapSurface2(PixelContainer *pThis, uint32_t Face, uint32_t Level);
XD3D_STDCALL Surface *D3D_CreateSurfaceHeader(uint32_t data, PixelContainer *parent, uint32_t format, uint32_t size);
XD3D_STDCALL void D3D_GetLevelInfo(PixelContainer *pc, uint32_t Face, uint32_t Level, uint32_t *pData,
                                   uint32_t *pPitch, uint32_t *pBytes, uint32_t *pFormat, uint32_t *pSize);
XD3D_STDCALL uint32_t D3DTexture_LockRect(PixelContainer *pThis, uint32_t Level, LockedRect *pLockedRect,
                                          const Rect *pRect, uint32_t Flags);
XD3D_STDCALL uint32_t D3DCubeTexture_LockRect(PixelContainer *pThis, uint32_t Face, uint32_t Level,
                                              LockedRect *pLockedRect, const Rect *pRect, uint32_t Flags);
XD3D_STDCALL uint32_t D3DSurface_LockRect(Surface *pThis, LockedRect *pLockedRect, const Rect *pRect, uint32_t Flags);
XD3D_STDCALL void D3D_Get2DSurfaceDesc(PixelContainer *pc, uint32_t Level, SurfaceDesc *pDesc);
XD3D_STDCALL void D3DSurface_GetDesc(Surface *pThis, SurfaceDesc *pDesc);

// ---- level geometry ----
//
// The arithmetic that says where inside a pixel container a given face and level live. Ported whole from the
// Windows loader because it is the console's own layout -- the 128-byte cube face stride and the DXT block
// rounding are things the game reads back through LockRect, so a cleaner arrangement would be a wrong one.

void pixel_container_dims(const PixelContainer *pc, uint32_t *w, uint32_t *h, uint32_t *d, uint32_t *levels);

// Byte offset of (face, level) from the start of the container's data, plus that level's pitch, width,
// height and size. `pitch` is what LockRect reports; `w`/`h` are the level's dimensions, which for a mip
// chain are smaller than the container's; `bytes` is that level's own size, which is what an allocation has
// to cover.
uint32_t texture_level_offset(const PixelContainer *pc, uint32_t face, uint32_t level, uint32_t *pitch, uint32_t *w,
                              uint32_t *h, uint32_t *bytes);

// How many bytes one level of one face occupies, counted from the start of that face rather than from the
// start of the container.
uint32_t texture_level_bytes(const PixelContainer *pc, uint32_t face, uint32_t level);

// How many bytes the whole container needs. This is what CreateTexture2 allocates, and getting it short is
// the failure that does not announce itself: the last mip level writes past the end and the next allocation
// is the thing that breaks.
uint32_t texture_total_bytes(const PixelContainer *pc);

// ---- what we keep beside a header ----
//
// The console kept none of this: its kernel knew how big an allocation was, and the loader asked. Here the
// header is a 12-byte struct the game can read and write, so the size of its data, and the fact that we
// allocated it at all, have to live somewhere the game cannot see.
struct ResourceRecord {
    uint32_t bytes = 0;   // the data block's size
    uint32_t managed = 0; // the gfx managed buffer, when this resource has one; 0 otherwise
    uint64_t dirty_lo = 0;
    uint64_t dirty_hi = 0;
    bool dirty = false;
    // A header the game built itself out of its own memory must not be freed by us, and a data block we did
    // not allocate must not be freed either. The console recorded the first in COMMON_D3DCREATED; the second
    // is ours to know, because there is no kernel table to ask.
    bool owns_data = false;
    bool owns_header = false;
    // Released to zero while still bound to a texture stage. The console destroyed it anyway; the Windows
    // loader kept it, because its host D3D9 bridge had the resource in a texture of its own and freeing it
    // would have been a use-after-free. Neither half applies here, but the hazard the rule guards against is
    // real, so the resource is kept until it is unbound -- and then freed, which is what the Windows loader
    // never did. "Released resources are rare" is a comment about that loader, not a reason to leak.
    bool pending_destroy = false;
};

// The record for a resource, or null for a pointer that is not one of our headers. Null is the answer for a
// header the game made, and the callers below all treat that as "do not free anything", which is the only
// safe reading.
ResourceRecord *resource_record(Resource *r);
const ResourceRecord *resource_record(const Resource *r);

// Attach a record to a header we have just allocated. `bytes` is the size of the data block.
ResourceRecord &resource_attach(Resource *r, uint32_t bytes, bool owns_data);

// The host block behind a resource's Data, or null when there is none.
void *resource_data(const Resource *r);

// Note that the game wrote [lo, hi) inside a resource's data. Ranges widen rather than replace, for the same
// reason the managed buffers' do: two writes to opposite ends must produce one copy spanning both.
void resource_mark_dirty(Resource *r, uint64_t lo, uint64_t hi);

// Free a resource that was released to zero while it was still bound and has now come off its texture
// stage. This is the only place a bound resource's free is deferred, and the only place it is carried out.
bool resource_release_if_unbound(Resource *r);

// Forget everything about a header. Called from the destroy path once the header is on its way out, and
// separately by a test that wants to check the table empties.
void resource_forget(Resource *r);

// How many records are live. For tests: a create/destroy pair that does not return to zero is a leak.
size_t resource_record_count();

// Every record currently live, for tests and for --probe. The pointers are ours; the headers are the game's.
size_t resource_record_each(Resource **out, size_t max);

} // namespace xd3d