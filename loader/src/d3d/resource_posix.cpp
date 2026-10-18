#include "d3d/resource_posix.h"

#include "d3d/device_posix.h" // XD3D_STDCALL, state()

#include "d3d/hle_table_posix.h"

#include "platform/posix/log.h"

#include <stdlib.h>
#include <string.h>

#include <map>

namespace xd3d {
namespace {

// Reached through generated thunks that already log the entry, so these lines say what the row *did* rather
// than that it was called. The Windows port's DTRACE did both; here the thunk does one and this does the
// other, and the text is kept so a log still reads the way it did.
#define DTRACE(...) logf("[d3d] " __VA_ARGS__)

uint32_t log2u(uint32_t v)
{
    uint32_t r = 0;
    while ((1u << r) < v) r++;
    return r;
}

std::map<Resource *, ResourceRecord> &records()
{
    static std::map<Resource *, ResourceRecord> m;
    return m;
}

// The console asked its kernel for contiguous memory and stored the physical address. Here the header's
// Data is the host block itself, which fits the same 32-bit field.
//
// Zero-filled, deliberately. A resource the game creates and then only reads -- a constant table uploaded
// once at startup -- must deliver zeros and not whatever the heap happened to hold, because there is no
// console behaviour to fall back on and the game has no way to tell the difference.
void *alloc_data(uint32_t bytes)
{
    if (!bytes) bytes = 1;
    void *p = calloc(bytes, 1);
    if (!p) logf("[d3d] out of memory for a 0x%x-byte resource", bytes);
    return p;
}

// A header the game may read and write, so it is exactly the console's size and nothing more.
void *alloc_header(size_t size)
{
    void *p = calloc(1, size);
    if (!p) logf("[d3d] out of memory for a %u-byte resource header", (unsigned)size);
    return p;
}

// ---- the one piece of geometry everything else measures from ----

// One level's pitch, dimensions and size, all at once.
//
// Every caller that needs a level's pitch needs its height too -- LockRect reports the pitch, the allocation
// has to cover pitch x height, and the level's offset is the sum of the ones below it. Computing the three
// in one place is what keeps them from drifting apart, and the arithmetic in here is the console's: DXT
// levels are addressed in 4x4 blocks, so both the pitch and the height round up to whole blocks. Leave that
// out and the last block of a texture is written past the end of its own memory.
void level_geometry(uint32_t tw, uint32_t th, const FormatInfo &fi, uint32_t level, uint32_t *pitch, uint32_t *w,
                    uint32_t *h, uint32_t *bytes)
{
    uint32_t x = tw >> level, y = th >> level;
    if (!x) x = 1;
    if (!y) y = 1;
    *w = x;
    *h = y;
    if (fi.compressed) {
        *pitch = ((x + 3) / 4) * fi.bpp * 2;
        *bytes = *pitch * ((y + 3) / 4);
    } else {
        *pitch = x * fi.bpp / 8;
        *bytes = *pitch * y;
    }
}

// The whole of one face, padded the way cube faces are. A non-cube container has one face and no padding,
// which is why the rounding only happens when there is more than one.
uint32_t face_bytes(const PixelContainer *pc, uint32_t tw, uint32_t th, uint32_t levels)
{
    const FormatInfo &fi = format_info(format_of(pc->Format));
    uint32_t total = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t p, w, h, b;
        level_geometry(tw, th, fi, l, &p, &w, &h, &b);
        total += b;
    }
    if ((pc->Format & FMT_CUBEMAP) && levels) total = (total + 127) & ~127u;
    return total;
}

Surface *new_surface(uint32_t data, uint32_t format, uint32_t size, PixelContainer *parent)
{
    auto *s = (Surface *)alloc_header(sizeof(Surface));
    if (!s) return nullptr;
    s->Common = 1 | (RTYPE_SURFACE << COMMON_TYPE_SHIFT) | COMMON_D3DCREATED;
    s->Data = data;
    s->Lock = 0;
    s->Format = format;
    s->Size = size;
    s->Parent = parent;
    if (parent) parent->Common++;
    return s;
}

// Format and Size words for one level, so a surface header describes just that level and not the container
// it came from.
void level_format(const PixelContainer *pc, uint32_t level, uint32_t *format, uint32_t *size)
{
    if (pc->Size) { // linear: one level, and the words are already about it
        *format = pc->Format;
        *size = pc->Size;
        return;
    }
    uint32_t w, h, d, levels;
    pixel_container_dims(pc, &w, &h, &d, &levels);
    w = w >> level ? w >> level : 1;
    h = h >> level ? h >> level : 1;
    *format = (pc->Format & ~(0xFFFu << FMT_MIPMAP_SHIFT) & ~FMT_CUBEMAP) | (1u << FMT_MIPMAP_SHIFT) |
              (log2u(w) << FMT_USIZE_SHIFT) | (log2u(h) << FMT_VSIZE_SHIFT);
    *size = 0;
}

// The start of (face, level), or null when the container has no data block of ours behind it.
uint8_t *level_bits(const PixelContainer *pc, uint32_t face, uint32_t level, uint32_t *pitch)
{
    uint32_t w, h, bytes;
    const uint32_t off = texture_level_offset(pc, face, level, pitch, &w, &h, &bytes);
    uint8_t *base = (uint8_t *)resource_data(pc);
    return base ? base + off : nullptr;
}

// Free a resource. The console's rule, unchanged: a surface with a parent frees only its own header and
// drops the parent's reference; a header without COMMON_D3DCREATED belongs to the game and its data with it;
// a resource still bound to a texture stage is kept, because a released-but-bound resource is rare and
// freeing it would be a use-after-free the game cannot have caused.
void destroy_resource(Resource *r)
{
    if (!r) return;
    if (!(r->Common & COMMON_D3DCREATED)) return; // header and data belong to the game

    if (resource_type(r) == RTYPE_SURFACE && ((Surface *)r)->Parent) {
        PixelContainer *parent = ((Surface *)r)->Parent;
        ResourceRecord *rec = resource_record(r);
        if (rec && rec->owns_data) free(resource_data(r));
        resource_forget(r);
        free(r);
        if ((parent->Common & COMMON_REFCOUNT_MASK) && (--parent->Common & COMMON_REFCOUNT_MASK) == 0)
            destroy_resource(parent);
        return;
    }

    for (int i = 0; i < 4; i++)
        if (state().textures[i] == r) { // still bound: mark it, and free it when it comes off
            ResourceRecord *bound = resource_record(r);
            if (bound) bound->pending_destroy = true;
            return;
        }

    ResourceRecord *rec = resource_record(r);
    if (rec) {
        if (rec->managed) {
            // A managed buffer we made owns only its device-side half; the host block belongs to whoever
            // allocated it, which here is us but is tracked separately so the two are not freed twice.
        }
        if (rec->owns_data) free(resource_data(r));
        resource_forget(r);
    }
    free(r);
}

} // namespace

// ---- level geometry ----

void pixel_container_dims(const PixelContainer *pc, uint32_t *w, uint32_t *h, uint32_t *d, uint32_t *levels)
{
    if (pc->Size) {
        *w = (pc->Size & SIZE_WIDTH_MASK) + 1;
        *h = ((pc->Size >> SIZE_HEIGHT_SHIFT) & 0xFFF) + 1;
        *d = 1;
        *levels = 1;
    } else {
        *w = 1u << ((pc->Format >> FMT_USIZE_SHIFT) & 0xF);
        *h = 1u << ((pc->Format >> FMT_VSIZE_SHIFT) & 0xF);
        *d = 1u << ((pc->Format >> FMT_PSIZE_SHIFT) & 0xF);
        *levels = (pc->Format >> FMT_MIPMAP_SHIFT) & 0xF;
        if (!*levels) *levels = 1;
    }
}

// Any of the four outputs may be null. A caller that wants only the offset -- a test checking where a face
// starts, a bound computation -- should not have to declare four locals to throw away.
uint32_t texture_level_offset(const PixelContainer *pc, uint32_t face, uint32_t level, uint32_t *pitch, uint32_t *w,
                              uint32_t *h, uint32_t *bytes)
{
    uint32_t lp, lw, lh, lb;
    if (!pitch) pitch = &lp;
    if (!w) w = &lw;
    if (!h) h = &lh;
    if (!bytes) bytes = &lb;

    uint32_t tw, th, td, levels;
    pixel_container_dims(pc, &tw, &th, &td, &levels);
    const FormatInfo &fi = format_info(format_of(pc->Format));

    if (pc->Size) { // linear: one level, and its pitch is in the Size word
        *pitch = (((pc->Size >> SIZE_PITCH_SHIFT) & 0xFF) + 1) * 64;
        *w = tw;
        *h = th;
        *bytes = *pitch * (fi.compressed ? (th + 3) / 4 : th);
        return 0;
    }

    uint32_t off = 0;
    if (face) off = face * face_bytes(pc, tw, th, levels);
    for (uint32_t l = 0; l < level; l++) {
        uint32_t p, lw2, lh2, b;
        level_geometry(tw, th, fi, l, &p, &lw2, &lh2, &b);
        off += b;
    }
    level_geometry(tw, th, fi, level, pitch, w, h, bytes);
    return off;
}

uint32_t texture_level_bytes(const PixelContainer *pc, uint32_t face, uint32_t level)
{
    uint32_t pitch, w, h, bytes;
    texture_level_offset(pc, face, level, &pitch, &w, &h, &bytes);
    return bytes;
}

uint32_t texture_total_bytes(const PixelContainer *pc)
{
    uint32_t tw, th, td, levels;
    pixel_container_dims(pc, &tw, &th, &td, &levels);
    const FormatInfo &fi = format_info(format_of(pc->Format));

    if (pc->Size) { // linear: one level whose pitch is already fixed by the Size word
        const uint32_t pitch = (((pc->Size >> SIZE_PITCH_SHIFT) & 0xFF) + 1) * 64;
        return pitch * (fi.compressed ? (th + 3) / 4 : th);
    }

    const uint32_t faces = (pc->Format & FMT_CUBEMAP) ? 6u : 1u;
    const uint32_t one = face_bytes(pc, tw, th, levels);
    uint32_t total = one * faces;
    if (((pc->Format >> FMT_DIMENSION_SHIFT) & 3) == 3) total *= td; // a volume: every level is slices deep
    return total;
}

// ---- what we keep beside a header ----

ResourceRecord *resource_record(Resource *r)
{
    if (!r) return nullptr;
    std::map<Resource *, ResourceRecord>::iterator it = records().find(r);
    return it == records().end() ? nullptr : &it->second;
}

const ResourceRecord *resource_record(const Resource *r) { return resource_record(const_cast<Resource *>(r)); }

ResourceRecord &resource_attach(Resource *r, uint32_t bytes, bool owns_data)
{
    ResourceRecord &rec = records()[r];
    rec.bytes = bytes;
    rec.owns_data = owns_data;
    rec.owns_header = true;
    // Everything is dirty at first, for the same reason a managed buffer is: the game filled this memory
    // before the loader ever heard of it.
    rec.dirty = true;
    rec.dirty_lo = 0;
    rec.dirty_hi = bytes;
    return rec;
}

void *resource_data(const Resource *r) { return r ? (void *)(uintptr_t)r->Data : nullptr; }

void resource_mark_dirty(Resource *r, uint64_t lo, uint64_t hi)
{
    ResourceRecord *rec = resource_record(r);
    if (!rec) return;
    if (hi > rec->bytes) hi = rec->bytes;
    if (lo >= hi) return;
    if (rec->dirty) {
        if (lo < rec->dirty_lo) rec->dirty_lo = lo;
        if (hi > rec->dirty_hi) rec->dirty_hi = hi;
    } else {
        rec->dirty_lo = lo;
        rec->dirty_hi = hi;
        rec->dirty = true;
    }
}

void resource_forget(Resource *r) { records().erase(r); }

// Free a resource that reached zero while bound and has now come off its stage. Called from the texture
// binding path, which is the only place a binding ends. Returns true when something was freed, so the
// caller can tell a no-op from a free.
bool resource_release_if_unbound(Resource *r)
{
    ResourceRecord *rec = resource_record(r);
    if (!rec || !rec->pending_destroy) return false;
    for (int i = 0; i < 4; i++)
        if (state().textures[i] == r) return false; // bound again in between: still not ours to free
    destroy_resource(r);
    return true;
}

size_t resource_record_count() { return records().size(); }

size_t resource_record_each(Resource **out, size_t max)
{
    size_t n = 0;
    for (std::map<Resource *, ResourceRecord>::iterator it = records().begin(); it != records().end() && n < max;
         ++it) {
        if (out) out[n] = it->first;
        n++;
    }
    return n;
}

// ---- creation ----

// Fill Format and Size for a new pixel container, then allocate its data.
//
// The allocation size is read back from the container's own words rather than taken from the caller's width
// and height, because the words are what the game will read and the arithmetic that follows reads them
// too. A block a few bytes short allocates without complaint and the game finds out when the last mip level
// writes into whatever the heap put next.
static uint32_t init_pixel_container(PixelContainer *t, uint32_t Width, uint32_t Height, uint32_t Depth,
                                     uint32_t *Levels, uint32_t Format, bool cube, bool volume)
{
    const FormatInfo &fi = format_info(Format);
    if (!Depth) Depth = 1;
    t->Lock = 0;
    if (fi.linear) {
        const uint32_t pitch = ((Width * fi.bpp / 8) + 63) & ~63u;
        t->Format = 1 | (2u << FMT_DIMENSION_SHIFT) | (Format << FMT_FORMAT_SHIFT) | (1u << FMT_MIPMAP_SHIFT);
        t->Size = (Width - 1) | ((Height - 1) << SIZE_HEIGHT_SHIFT) | ((pitch / 64 - 1) << SIZE_PITCH_SHIFT);
        *Levels = 1;
    } else {
        const uint32_t maxl = log2u(Width > Height ? Width : Height) + 1;
        if (!*Levels || *Levels > maxl) *Levels = maxl;
        t->Format = 1 | (cube ? FMT_CUBEMAP : 0) | ((volume ? 3u : 2u) << FMT_DIMENSION_SHIFT) |
                    (Format << FMT_FORMAT_SHIFT) | (*Levels << FMT_MIPMAP_SHIFT) |
                    (log2u(Width) << FMT_USIZE_SHIFT) | (log2u(Height) << FMT_VSIZE_SHIFT) |
                    (log2u(Depth) << FMT_PSIZE_SHIFT);
        t->Size = 0;
    }
    const uint32_t bytes = texture_total_bytes(t);
    t->Data = (uint32_t)(uintptr_t)alloc_data(bytes);
    resource_attach(t, bytes, true);
    return bytes;
}

Resource *XD3D_STDCALL D3DDevice_CreateTexture2(uint32_t Width, uint32_t Height, uint32_t Depth, uint32_t Levels,
                                               uint32_t Usage, uint32_t Format, uint32_t ResourceType)
{
    auto *t = (PixelContainer *)alloc_header(sizeof(PixelContainer));
    if (!t) return nullptr;
    t->Common = 1 | (RTYPE_TEXTURE << COMMON_TYPE_SHIFT) | COMMON_D3DCREATED;
    uint32_t levels = Levels;
    const uint32_t bytes =
        init_pixel_container(t, Width, Height, Depth, &levels, Format, ResourceType == 5, ResourceType == 4);
    DTRACE("CreateTexture2 %ux%ux%u, levels %u, usage 0x%x, %s, type %u -> %p, data 0x%x, 0x%x bytes", Width, Height, Depth, Levels,
           Usage, format_info(Format).name, ResourceType, t, t->Data, bytes);
    return t;
}

// CreateSurface2: a stand-alone surface -- a render target, or a swap chain buffer -- with no container above
// it. The top bit of Common is the console's own "the game made this surface" mark, kept because the game
// reads the field.
Surface *XD3D_STDCALL D3D_CreateSurface2(uint32_t Width, uint32_t Height, uint32_t Usage, uint32_t Format)
{
    auto *s = (Surface *)alloc_header(sizeof(Surface));
    if (!s) return nullptr;
    s->Common = 0x80000000u | 1 | (RTYPE_SURFACE << COMMON_TYPE_SHIFT) | COMMON_D3DCREATED;
    s->Parent = nullptr;
    uint32_t levels = 1;
    const uint32_t bytes = init_pixel_container(s, Width, Height, 1, &levels, Format, false, false);
    DTRACE("CreateSurface2 %ux%u, usage 0x%x, %s -> %p, data 0x%x, 0x%x bytes", Width, Height, Usage, format_info(Format).name, s,
           s->Data, bytes);
    return s;
}

// CreateVertexBuffer2 takes only a length: the console sized the block from that and the game always wrote
// through Lock, so there is nothing else to record.
//
// This is also the row that hands the game a pointer it writes into directly, which makes it the natural
// place to hang a managed buffer -- the bytes the game writes are the bytes the GPU reads, once a copy has
// been made. That copy belongs to A4's draw path; the handle lives in the record from here on so it has
// somewhere to be stored.
Resource *XD3D_STDCALL D3DDevice_CreateVertexBuffer2(uint32_t Length)
{
    auto *vb = (Resource *)alloc_header(sizeof(Resource));
    if (!vb) return nullptr;
    vb->Common = 1 | (RTYPE_VERTEXBUFFER << COMMON_TYPE_SHIFT) | COMMON_D3DCREATED;
    vb->Lock = 0;
    const uint32_t bytes = Length ? Length : 1;
    vb->Data = (uint32_t)(uintptr_t)alloc_data(bytes);
    resource_attach(vb, bytes, true);
    DTRACE("CreateVertexBuffer2 %u -> %p, data 0x%x", Length, vb, vb->Data);
    return vb;
}

void *XD3D_STDCALL D3DVertexBuffer_Lock2(Resource *pVB, uint32_t Flags)
{
    DTRACE("%p, 0x%x", pVB, Flags);
    return resource_data(pVB);
}

void *XD3D_STDCALL D3DPalette_Lock2(Resource *pThis, uint32_t Flags)
{
    DTRACE("%p, 0x%x", pThis, Flags);
    return resource_data(pThis);
}

// ---- lifetime ----

uint32_t XD3D_STDCALL D3DResource_AddRef(Resource *pThis)
{
    DTRACE("%p", pThis);
    if (!pThis) return 0;
    pThis->Common++;
    return pThis->Common & COMMON_REFCOUNT_MASK;
}

uint32_t XD3D_STDCALL D3DResource_Release(Resource *pThis)
{
    DTRACE("%p", pThis);
    if (!pThis) return 0;
    const uint32_t n = pThis->Common & COMMON_REFCOUNT_MASK;
    if (n == 0) return 0; // an over-release is the game's; the console clamped it and so does this
    pThis->Common--;
    if (n == 1) destroy_resource(pThis);
    return n - 1;
}

void XD3D_STDCALL D3D_DestroyResource(Resource *pResource)
{
    DTRACE("%p", pResource);
    destroy_resource(pResource);
}

void XD3D_STDCALL D3DResource_BlockUntilNotBusy(Resource *pThis) { DTRACE("%p", pThis); }

// ---- surfaces ----

Surface *XD3D_STDCALL D3DTexture_GetSurfaceLevel2(PixelContainer *pThis, uint32_t Level)
{
    uint32_t pitch, w, h, bytes, fmt, size;
    const uint32_t off = texture_level_offset(pThis, 0, Level, &pitch, &w, &h, &bytes);
    level_format(pThis, Level, &fmt, &size);
    Surface *s = new_surface(pThis->Data + off, fmt, size, pThis);
    DTRACE("GetSurfaceLevel2 %p, %u -> %p, data 0x%x", pThis, Level, s, pThis ? pThis->Data + off : 0u);
    return s;
}

Surface *XD3D_STDCALL D3DCubeTexture_GetCubeMapSurface2(PixelContainer *pThis, uint32_t Face, uint32_t Level)
{
    uint32_t pitch, w, h, bytes, fmt, size;
    const uint32_t off = texture_level_offset(pThis, Face, Level, &pitch, &w, &h, &bytes);
    level_format(pThis, Level, &fmt, &size);
    Surface *s = new_surface(pThis->Data + off, fmt, size, pThis);
    DTRACE("GetCubeMapSurface2 %p, %u, %u -> %p, data 0x%x", pThis, Face, Level, s, pThis ? pThis->Data + off : 0u);
    return s;
}

// GetLevelInfo: where one level's data starts, its pitch, its size in bytes, and the Format/Size words a
// surface header for that level needs.
void XD3D_STDCALL D3D_GetLevelInfo(PixelContainer *pc, uint32_t Face, uint32_t Level, uint32_t *pData,
                                   uint32_t *pPitch, uint32_t *pBytes, uint32_t *pFormat, uint32_t *pSize)
{
    uint32_t pitch = 0, fmt = 0, size = 0;
    uint8_t *bits = level_bits(pc, Face, Level, &pitch);
    level_format(pc, Level, &fmt, &size);
    uint32_t w, h, bytes;
    texture_level_offset(pc, Face, Level, &pitch, &w, &h, &bytes);
    // A plain pointer, with no flag bit. The Windows port set bit 31 here and masked it off again in
    // CreateSurfaceHeader; both halves were that port's own convention for its host D3D9 bridge, and the
    // console had neither. The game may hand this value straight back, and a masked pointer is not the
    // pointer it handed us.
    if (pData) *pData = (uint32_t)(uintptr_t)bits;
    if (pPitch) *pPitch = pitch;
    if (pBytes) *pBytes = bytes;
    if (pFormat) *pFormat = fmt;
    if (pSize) *pSize = size;
    DTRACE("GetLevelInfo %p, %u, %u -> data 0x%x pitch %u", pc, Face, Level, (uint32_t)(uintptr_t)bits, pitch);
}

// CreateSurfaceHeader: a header for a level of a parent, pointing at memory the caller already has.
Surface *XD3D_STDCALL D3D_CreateSurfaceHeader(uint32_t data, PixelContainer *parent, uint32_t format, uint32_t size)
{
    Surface *s = new_surface(data, format, size, parent);
    // Deliberately no record. We allocated the header but not the data, so the destroy path must free exactly
    // one of the two; a record claiming the data would hand the game back memory it owns.
    DTRACE("CreateSurfaceHeader data 0x%x, parent %p, fmt 0x%x, size 0x%x -> %p", data, parent, format, size, s);
    return s;
}

static uint32_t lock_rect(PixelContainer *pc, uint32_t face, uint32_t level, LockedRect *lr, const Rect *rect)
{
    uint32_t pitch = 0;
    uint8_t *bits = level_bits(pc, face, level, &pitch);
    if (!lr || !bits) return D3D_E_POINTER;
    if (rect && pc->Size) {
        const FormatInfo &fi = format_info(format_of(pc->Format));
        bits += (uint32_t)rect->top * pitch + (uint32_t)rect->left * fi.bpp / 8;
    }
    lr->Pitch = (int32_t)pitch;
    lr->pBits = bits;

    // The whole level is marked, not just the rectangle. The bytes outside the rectangle are still whatever
    // the game last wrote there, and a copy that skipped them would upload a texture with a hole in it --
    // which is the failure a rectangle lock is *for*.
    uint32_t w, h, bytes;
    const uint32_t off = texture_level_offset(pc, face, level, &pitch, &w, &h, &bytes);
    resource_mark_dirty(pc, off, off + bytes);
    return D3D_OK;
}

uint32_t XD3D_STDCALL D3DTexture_LockRect(PixelContainer *pThis, uint32_t Level, LockedRect *pLockedRect,
                                          const Rect *pRect, uint32_t Flags)
{
    DTRACE("%p, %u, %p, %p, 0x%x", pThis, Level, pLockedRect, pRect, Flags);
    return lock_rect(pThis, 0, Level, pLockedRect, pRect);
}

uint32_t XD3D_STDCALL D3DCubeTexture_LockRect(PixelContainer *pThis, uint32_t Face, uint32_t Level,
                                              LockedRect *pLockedRect, const Rect *pRect, uint32_t Flags)
{
    DTRACE("%p, %u, %u, %p, %p, 0x%x", pThis, Face, Level, pLockedRect, pRect, Flags);
    return lock_rect(pThis, Face, Level, pLockedRect, pRect);
}

uint32_t XD3D_STDCALL D3DSurface_LockRect(Surface *pThis, LockedRect *pLockedRect, const Rect *pRect, uint32_t Flags)
{
    DTRACE("%p, %p, %p, 0x%x", pThis, pLockedRect, pRect, Flags);
    const uint32_t hr = lock_rect(pThis, 0, 0, pLockedRect, pRect);
    // A surface is a window onto its parent, so what the game wrote is the parent's too. Marking the parent
    // from the surface's own record would mark the wrong container: a surface's record describes the parent's
    // pixels through a header, and its dirty range belongs to the parent.
    if (hr == D3D_OK && pThis && pThis->Parent) {
        ResourceRecord *parent = resource_record(pThis->Parent);
        if (parent && !parent->dirty) {
            parent->dirty = true;
            parent->dirty_lo = 0;
            parent->dirty_hi = parent->bytes;
        }
    }
    return hr;
}

static void fill_desc(PixelContainer *pc, uint32_t level, SurfaceDesc *d)
{
    uint32_t pitch, w, h, bytes;
    texture_level_offset(pc, 0, level, &pitch, &w, &h, &bytes);
    d->Format = format_of(pc->Format);
    d->Type = 1;               // D3DRTYPE_SURFACE
    d->Usage = 0;
    d->Size = bytes;
    d->MultiSampleType = 0x0011; // D3DMULTISAMPLE_NONE on the console
    d->Width = w;
    d->Height = h;
}

// Get2DSurfaceDesc
void XD3D_STDCALL D3D_Get2DSurfaceDesc(PixelContainer *pc, uint32_t Level, SurfaceDesc *pDesc)
{
    DTRACE("%p, %u, %p", pc, Level, pDesc);
    if (pc && pDesc) fill_desc(pc, Level, pDesc);
}

void XD3D_STDCALL D3DSurface_GetDesc(Surface *pThis, SurfaceDesc *pDesc)
{
    DTRACE("%p, %p", pThis, pDesc);
    if (pThis && pDesc) fill_desc(pThis, 0, pDesc);
}

// ---- the table ----

#define H(addr, fn, params, ret)                                                                                      \
    {                                                                                                                  \
        addr, #fn, (void *)&fn, params, ret                                                                           \
    }

static const Hle kTable[] = {
    H(0x0026c6d0, D3DDevice_CreateTexture2, "s s s s s s s", true),
    H(0x00269b30, D3D_CreateSurface2, "s s s s", true),
    H(0x0026c1e0, D3DDevice_CreateVertexBuffer2, "s", true),
    H(0x0026c230, D3DVertexBuffer_Lock2, "s s", true),
    H(0x0026c2c0, D3DPalette_Lock2, "s s", true),
    H(0x0026c470, D3DResource_AddRef, "s", true),
    H(0x0026c4b0, D3DResource_Release, "s", true),
    H(0x0026c360, D3D_DestroyResource, "edi", false),
    H(0x0026c610, D3DResource_BlockUntilNotBusy, "s", false),
    H(0x0026c780, D3DTexture_GetSurfaceLevel2, "s s", true),
    H(0x0026c800, D3DCubeTexture_GetCubeMapSurface2, "s s s", true),
    H(0x00269ac0, D3D_CreateSurfaceHeader, "eax edi s s", true),
    H(0x0026d6d0, D3D_GetLevelInfo, "s s s s s s s s", false),
    H(0x0026c7d0, D3DTexture_LockRect, "s s s s s", true),
    H(0x0026c850, D3DCubeTexture_LockRect, "s s s s s s", true),
    H(0x00269be0, D3DSurface_LockRect, "s s s s", true),
    H(0x0026d630, D3D_Get2DSurfaceDesc, "edi ebx esi", false),
    H(0x00269bc0, D3DSurface_GetDesc, "s s", false),
};

#undef H

const Hle *resource_rows(size_t *count)
{
    *count = sizeof kTable / sizeof kTable[0];
    return kTable;
}

} // namespace xd3d