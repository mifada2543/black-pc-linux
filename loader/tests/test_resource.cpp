// The resource half: what the game creates, locks, reads back and destroys.
//
// Two things are under test and they are not the same thing. The first is that the calls behave: a created
// texture has the format words the console would have given it, a Lock hands back a pointer into the right
// level, a Release frees what it should and not what it should not. The second is that the arithmetic the
// calls are built on is the console's arithmetic -- the 128-byte cube stride, the DXT block rounding, the
// linear pitch in the Size word -- because getting one of those wrong produces a texture that looks
// correct right up until the last mip level walks off the end of its own memory.
#include "d3d/resource_posix.h"

#include "d3d/device_posix.h" // XD3D_STDCALL, state(), D3DDevice_SetTexture

#include "d3d/hle_table_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// The device rows are reached through generated thunks by the device test, because what is worth checking
// there is that the thunk works. Here the resource is what matters, so SetTexture is called directly -- which
// needs it named. The device header declares no row implementations at all.
namespace xd3d {
XD3D_STDCALL void D3DDevice_SetTexture(uint32_t Stage, Resource *pTexture);
} // namespace xd3d

static int g_checks = 0, g_failures = 0;

static void check(bool ok, const char *what)
{
    printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
    g_checks++;
    if (!ok) g_failures++;
}

static void check_eq(uint64_t got, uint64_t want, const char *what)
{
    if (got != want) printf("  ....  %s: dapat %llu, harap %llu\n", what, (unsigned long long)got,
                            (unsigned long long)want);
    check(got == want, what);
}

using namespace xd3d;

// A format the console treats as pitch-linear, and one that is not, and a compressed one. All three paths
// through the geometry have to be exercised, because they are three different pieces of arithmetic.
// Two 32-bit formats that are otherwise identical, and the difference is the whole of the geometry: the
// LIN_ one stores its pitch in the Size word and has exactly one level, the other keeps a mip chain in the
// Format word. A test that used the linear one for a mip chain would be testing a container with no levels
// to speak of, and would pass for the wrong reason.
static const uint32_t kFormatMip = XFMT_A8R8G8B8;
static const uint32_t kFormatLinear = XFMT_LIN_A8R8G8B8;
static const uint32_t kFormatDXT1 = XFMT_DXT1;

// ---- the arithmetic ----

static void test_level_geometry_of_a_mip_texture()
{
    printf("\nlevel geometry, a mip chain\n");

    // 64x64 A8R8G8B8 with a full chain: 64, 32, 16, 8, 4, 2, 1.
    PixelContainer *t = (PixelContainer *)D3DDevice_CreateTexture2(64, 64, 1, 0, 0, kFormatMip, 2);
    if (!t) {
        check(false, "a texture is created");
        return;
    }
    const ResourceRecord *rec = resource_record(t);
    check(rec != nullptr, "and we keep a record of it");
    if (rec) check(rec->bytes == texture_total_bytes(t), "whose byte count is the container's own total");

    // Level 0 is a full 64x64 at 256 bytes a row: 16384 bytes.
    uint32_t pitch, w, h, bytes;
    uint32_t off = texture_level_offset(t, 0, 0, &pitch, &w, &h, &bytes);
    check_eq(pitch, 64 * 4, "level 0 has a 256-byte pitch");
    check_eq(w, 64, "and is 64 wide");
    check_eq(h, 64, "and 64 high");
    check_eq(bytes, 64 * 64 * 4, "and 16 KiB in size");
    check_eq(off, 0, "and starts at zero");

    // Level 1 is half in each direction, so a quarter of the bytes, and it starts where level 0 ended.
    off = texture_level_offset(t, 0, 1, &pitch, &w, &h, &bytes);
    check_eq(w, 32, "level 1 is 32 wide");
    check_eq(h, 32, "and 32 high");
    check_eq(pitch, 32 * 4, "with a 128-byte pitch");
    check_eq(bytes, 32 * 32 * 4, "and 4 KiB in size");
    check_eq(off, 64 * 64 * 4, "starting where level 0 ended");

    // The last level is 1x1, and the chain must add up to exactly what was allocated -- the whole point of
    // the total is that the last level ends at the end and not one byte past it.
    off = texture_level_offset(t, 0, 6, &pitch, &w, &h, &bytes);
    check_eq(w, 1, "the last level is 1 wide");
    check_eq(off + bytes, rec ? rec->bytes : 0, "and ends exactly at the end of the allocation");

    // A level past the end is clamped to 1x1 rather than becoming zero, because a zero-sized level would
    // make the offsets above it wrong in a way that reads as corruption rather than as a bad argument.
    off = texture_level_offset(t, 0, 12, &pitch, &w, &h, &bytes);
    check_eq(w, 1, "a level past the end is 1 wide, not 0");
    D3DResource_Release(t);
}

static void test_a_compressed_level_rounds_to_blocks()
{
    printf("\ncompressed levels round to whole blocks\n");

    // 4x4 DXT1 is one block: 8 bytes a row and 8 bytes in all. If the block rounding is missing, this is
    // zero bytes and the texture has nowhere to live.
    PixelContainer *t = (PixelContainer *)D3DDevice_CreateTexture2(4, 4, 1, 1, 0, kFormatDXT1, 2);
    if (!t) {
        check(false, "a DXT1 texture is created");
        return;
    }
    uint32_t pitch, w, h, bytes;
    texture_level_offset(t, 0, 0, &pitch, &w, &h, &bytes);
    check_eq(w, 4, "a 4x4 texture is 4 wide");
    check_eq(pitch, 8, "with an 8-byte pitch -- one DXT1 block is 8 bytes");
    check_eq(bytes, 8, "and 8 bytes in total");

    // 3x3 is still one block. Rounding down here is the bug: the block covers the fourth column and row
    // anyway, so the texture is 8 bytes and not 6.
    D3DResource_Release(t);
    t = (PixelContainer *)D3DDevice_CreateTexture2(3, 3, 1, 1, 0, kFormatDXT1, 2);
    if (t) {
        texture_level_offset(t, 0, 0, &pitch, &w, &h, &bytes);
        check_eq(pitch, 8, "a 3x3 DXT1 texture still has an 8-byte pitch");
        check_eq(bytes, 8, "and still 8 bytes in total");
        D3DResource_Release(t);
    }
}

static void test_cube_faces_are_128_byte_aligned()
{
    printf("\ncube faces are padded\n");

    // 16x16 A8R8G8B8 cube, full chain: face 0 is 16*16*4 + 8*8*4 + 4*4*4 + 2*2*4 + 1*1*4 + 1*1*4 + 1*1*4
    // = 1024 + 256 + 64 + 16 + 4 + 4 + 4 = 1372, rounded to 1408.
    PixelContainer *t = (PixelContainer *)D3DDevice_CreateTexture2(16, 16, 1, 0, 0, kFormatMip, 5);
    if (!t) {
        check(false, "a cube texture is created");
        return;
    }
    check((t->Format & FMT_CUBEMAP) != 0, "and it carries the cube flag");
    check_eq(format_mipmaps(t->Format), 5, "with a 5-level chain for a 16x16 base");

    const uint32_t face0 = texture_level_offset(t, 0, 0, nullptr, nullptr, nullptr, nullptr) == 0 ? 0 : 1;
    check_eq(face0, 0, "face 0 starts at zero");

    uint32_t pitch, w, h, bytes;
    const uint32_t f1 = texture_level_offset(t, 1, 0, &pitch, &w, &h, &bytes);
    check_eq(f1, 1408, "face 1 starts at the 128-byte-aligned end of face 0");
    check_eq(f1 % 128, 0, "which is itself aligned");

    const uint32_t f5 = texture_level_offset(t, 5, 0, &pitch, &w, &h, &bytes);
    check_eq(f5, 5 * 1408, "and face 5 is five of those along");

    const ResourceRecord *rec = resource_record(t);
    check_eq(rec ? rec->bytes : 0, 6 * 1408, "and the allocation is six faces");

    // Within a face the levels still add up.
    const uint32_t lvl1 = texture_level_offset(t, 2, 1, &pitch, &w, &h, &bytes);
    check_eq(lvl1, 2 * 1408 + 16 * 16 * 4, "level 1 of face 2 follows level 0 of it");
    D3DResource_Release(t);
}

static void test_a_linear_container_reads_its_pitch_from_the_size_word()
{
    printf("\na linear container's pitch lives in its Size word\n");

    // A linear surface: the pitch is not derivable from the format, so it is stored. 100 pixels of
    // A8R8G8B8 is 400 bytes, which rounds up to 448 -- seven 64-byte units.
    Surface *s = D3D_CreateSurface2(100, 50, 0, kFormatLinear);
    if (!s) {
        check(false, "a linear surface is created");
        return;
    }
    check((s->Common & 0x80000000u) != 0, "and carries the console's stand-alone-surface mark");

    uint32_t pitch, w, h, bytes;
    const uint32_t off = texture_level_offset(s, 0, 0, &pitch, &w, &h, &bytes);
    check_eq(w, 100, "it is 100 wide");
    check_eq(h, 50, "and 50 high");
    check_eq(pitch, 448, "with the 448-byte pitch its Size word records");
    check_eq(off, 0, "and it is the only level");

    SurfaceDesc desc;
    memset(&desc, 0, sizeof desc);
    D3DSurface_GetDesc(s, &desc);
    check_eq(desc.Width, 100, "GetDesc reports the width");
    check_eq(desc.Height, 50, "and the height");
    check_eq(desc.Type, 1, "and that it is a surface");
    check_eq(desc.MultiSampleType, 0x0011, "and that it is not multisampled");
    check_eq(desc.Size, 448 * 50, "and its size in bytes");
    D3DResource_Release(s);
}

// ---- what the calls do ----

static void test_a_vertex_buffer_locks_onto_its_own_block()
{
    printf("\na vertex buffer locks onto its own block\n");

    const size_t before = resource_record_count();
    Resource *vb = D3DDevice_CreateVertexBuffer2(4096);
    if (!vb) {
        check(false, "a vertex buffer is created");
        return;
    }
    check_eq(resource_record_count(), before + 1, "and is recorded");
    check_eq(vb->Data != 0, true, "and has a data block");

    void *p = D3DVertexBuffer_Lock2(vb, 0);
    check(p != nullptr, "Lock hands back a pointer");
    check_eq((uintptr_t)p, (uintptr_t)resource_data(vb), "and it is the block behind Data");

    // Writing through the pointer has to be visible, because that is the whole contract: on the console the
    // pointer the game holds *is* the memory the GPU reads.
    memset(p, 0xAB, 4096);
    check_eq(((uint8_t *)resource_data(vb))[4095], 0xAB, "and what the game writes is what stays");
    check_eq(vb->Lock, 0, "while the Lock field itself is untouched");

    check_eq(D3DResource_AddRef(vb), 2, "AddRef counts");
    check_eq(D3DResource_Release(vb), 1, "and Release gives one back");
    check_eq(D3DResource_Release(vb), 0, "and the last one drops it to zero");
    check_eq(resource_record_count(), before, "which forgets the record -- so nothing leaked");
}

static void test_a_palette_locks_like_a_vertex_buffer()
{
    printf("\na palette is the same shape\n");

    Resource *p = (Resource *)D3DDevice_CreateVertexBuffer2(256 * 4);
    if (!p) {
        check(false, "a 256-entry palette-sized buffer is created");
        return;
    }
    void *bits = D3DPalette_Lock2(p, 0);
    check(bits != nullptr, "and its Lock hands back a pointer");
    check_eq((uintptr_t)bits, (uintptr_t)resource_data(p), "the same one Create made");
    D3DResource_Release(p);
}

static void test_levels_are_addressable_through_surfaces()
{
    printf("\nlevels are reachable, and each surface knows its level\n");

    PixelContainer *t = (PixelContainer *)D3DDevice_CreateTexture2(64, 64, 1, 0, 0, kFormatMip, 2);
    if (!t) {
        check(false, "a texture is created");
        return;
    }

    Surface *l0 = D3DTexture_GetSurfaceLevel2(t, 0);
    Surface *l3 = D3DTexture_GetSurfaceLevel2(t, 3);
    check(l0 && l3, "two levels come back");
    if (!l0 || !l3) return;

    check_eq(l0->Parent == t, true, "a level names its container as its parent");
    check_eq(l3->Parent == t, true, "as does every level");
    check_eq(l0->Data, t->Data, "level 0's data is the container's");
    check(l3->Data > l0->Data, "and level 3's is further along");

    // The reference the container gained for each surface has to be there, or the container is freed while
    // the game still holds a level.
    const uint32_t refs = t->Common & COMMON_REFCOUNT_MASK;
    check_eq(refs, 3, "and the container counted two more references");

    // Format words: a level's header describes the level, so its mipmap count is one and its dimensions are
    // the level's, not the container's.
    check_eq(format_mipmaps(l3->Format), 1, "a level's header says one level");
    check_eq(1u << ((l3->Format >> FMT_USIZE_SHIFT) & 0xF), 8, "and the level's own width");

    D3DResource_Release(l3);
    D3DResource_Release(l0);
    check_eq(t->Common & COMMON_REFCOUNT_MASK, 1, "releasing them hands the references back");
    D3DResource_Release(t);
}

static void test_lock_rect_hands_back_the_level()
{
    printf("\nLockRect points into the level it names\n");

    PixelContainer *t = (PixelContainer *)D3DDevice_CreateTexture2(64, 64, 1, 0, 0, kFormatMip, 2);
    if (!t) return;

    LockedRect lr;
    memset(&lr, 0, sizeof lr);
    const uint32_t hr = D3DTexture_LockRect(t, 2, &lr, nullptr, 0);
    check_eq(hr, D3D_OK, "a level locks");
    check_eq(lr.Pitch, 16 * 4, "reporting that level's pitch");
    check(lr.pBits != nullptr, "and a pointer");

    const uint8_t *base = (const uint8_t *)resource_data(t);
    // Levels 0 and 1 come first: 16384 + 4096 = 20480.
    check_eq((const uint8_t *)lr.pBits - base, 16384 + 4096, "which is the start of that level");

    // Writing through it lands in the container's own block.
    memset(lr.pBits, 0x5A, (size_t)lr.Pitch * 16);
    check_eq(base[20480], 0x5A, "and what the game writes is in the container");

    // The lock marks the level dirty, and marks all of it: the bytes outside a rectangle lock are still
    // whatever the game last put there.
    ResourceRecord *rec = resource_record(t);
    check(rec && rec->dirty, "and marks the container dirty");
    check(rec && rec->dirty_lo <= 20480 && rec->dirty_hi >= 20480 + (uint64_t)lr.Pitch * 16,
          "covering the whole level");

    D3DResource_Release(t);
}

static void test_a_rectangle_lock_moves_the_pointer()
{
    printf("\na rectangle lock offsets the pointer\n");

    Surface *s = D3D_CreateSurface2(64, 64, 0, kFormatLinear);
    if (!s) return;
    LockedRect lr;
    memset(&lr, 0, sizeof lr);
    Rect r;
    r.left = 3;
    r.top = 5;
    r.right = 20;
    r.bottom = 20;

    const uint32_t hr = D3DSurface_LockRect(s, &lr, &r, 0);
    check_eq(hr, D3D_OK, "a rectangle locks");
    check_eq(lr.Pitch, 64 * 4, "with the surface's pitch unchanged -- a rectangle is not a sub-buffer");
    const uint8_t *base = (const uint8_t *)resource_data(s);
    check_eq((const uint8_t *)lr.pBits - base, 5 * (64 * 4) + 3 * 4, "and the pointer moved by the rectangle");
    D3DResource_Release(s);
}

static void test_get_level_info_agrees_with_lock_rect()
{
    printf("\nGetLevelInfo and LockRect must not disagree\n");

    PixelContainer *t = (PixelContainer *)D3DDevice_CreateTexture2(32, 32, 1, 0, 0, kFormatDXT1, 2);
    if (!t) return;

    for (uint32_t level = 0; level < format_mipmaps(t->Format); level++) {
        uint32_t data = 0, pitch = 0, bytes = 0, fmt = 0, size = 0;
        D3D_GetLevelInfo(t, 0, level, &data, &pitch, &bytes, &fmt, &size);

        LockedRect lr;
        memset(&lr, 0, sizeof lr);
        D3DTexture_LockRect(t, level, &lr, nullptr, 0);
        const bool same = data == (uint32_t)(uintptr_t)lr.pBits && pitch == (uint32_t)lr.Pitch &&
                          bytes == texture_level_bytes(t, 0, level);
        if (!same)
            printf("  ....  level %u: info %u/%u/%u, lock %u/%u\n", level, data, pitch, bytes, (uint32_t)(uintptr_t)lr.pBits,
                   (uint32_t)lr.Pitch);
        check(same, "a level's info and its lock describe the same bytes");

        // And the next level starts where this one ended, which is what makes the allocation add up.
        if (level) {
            uint32_t d0 = 0, p0 = 0, b0 = 0, f0 = 0, s0 = 0;
            D3D_GetLevelInfo(t, 0, level - 1, &d0, &p0, &b0, &f0, &s0);
            check_eq(data, d0 + b0, "and the next level starts where the last one ended");
        }
    }
    D3DResource_Release(t);
}

static void test_cube_faces_lock_apart()
{
    printf("\ncube faces lock to their own face\n");

    PixelContainer *t = (PixelContainer *)D3DDevice_CreateTexture2(32, 32, 1, 0, 0, kFormatMip, 5);
    if (!t) return;
    LockedRect a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    D3DCubeTexture_LockRect(t, 0, 0, &a, nullptr, 0);
    D3DCubeTexture_LockRect(t, 3, 0, &b, nullptr, 0);
    check(a.pBits && b.pBits, "two cube faces lock");
    check(b.pBits != a.pBits, "to different memory");
    // A 32x32 face is 4096 + 1024 + 256 + 64 + 16 + 4 = 5460 bytes, rounded up to the 128-byte stride.
    const uint32_t one_face = 5504;
    check_eq((const uint8_t *)b.pBits - (const uint8_t *)a.pBits, 3 * one_face, "three faces along");
    D3DResource_Release(t);
}

static void test_create_surface_header_does_not_claim_the_data()
{
    printf("\na header for memory the game owns\n");

    PixelContainer *t = (PixelContainer *)D3DDevice_CreateTexture2(32, 32, 1, 1, 0, kFormatMip, 2);
    if (!t) return;
    const size_t before = resource_record_count();

    uint32_t bits = 0;
    D3D_GetLevelInfo(t, 0, 0, &bits, nullptr, nullptr, nullptr, nullptr);
    Surface *s = D3D_CreateSurfaceHeader(bits, t, 0, 0);
    check(s != nullptr, "a surface header is made over memory the game named");
    if (s) {
        check_eq(s->Data, bits, "and it points there");
        check_eq(s->Parent == t, true, "with its parent set");
        check_eq(resource_record_count(), before, "and no record: we did not allocate that memory");
    }
    // Destroying it must not free the parent's pixels. Doing so would be a free of memory the game still
    // holds, and it would show up much later as a crash somewhere else.
    if (s) D3DResource_Release(s);
    check_eq(resource_record_count(), before, "destroying it frees the header and nothing else");
    check_eq(((uint8_t *)resource_data(t))[0], 0, "and the parent's pixels are still there to be written");
    D3DResource_Release(t);
}

static void test_get_level_info_returns_a_plain_pointer()
{
    printf("\nGetLevelInfo hands back a plain pointer\n");

    // The Windows loader set bit 31 here and masked it off again in CreateSurfaceHeader, both to tell its
    // own host bridge that the value was virtual. The console had no such bit, and a game that echoed the
    // value back would have had it masked -- which is not the pointer it handed us.
    PixelContainer *t = (PixelContainer *)D3DDevice_CreateTexture2(32, 32, 1, 1, 0, kFormatMip, 2);
    if (!t) return;
    uint32_t data = 0;
    D3D_GetLevelInfo(t, 0, 0, &data, nullptr, nullptr, nullptr, nullptr);
    check_eq(data, (uint32_t)(uintptr_t)resource_data(t), "the data address is the block itself");
    check((data & 0xF0000000u) != 0xF0000000u, "with no flag nibble in it");
    D3DResource_Release(t);
}

// ---- lifetime, and what must not be freed ----

static void test_what_destroy_will_not_free()
{
    printf("\nwhat destroy will not free\n");

    // A header the game built. COMMON_D3DCREATED is how the console recorded that a header is the kernel's
    // to free, and a header without it belongs to the game.
    uint8_t scratch[sizeof(PixelContainer)] = {0};
    PixelContainer *fake = (PixelContainer *)scratch;
    fake->Common = 1 | (RTYPE_TEXTURE << COMMON_TYPE_SHIFT);
    fake->Data = 0x11111111u;
    D3DResource_Release(fake);
    check_eq(scratch[0] == 0, true, "a header without D3DCREATED is left alone, not freed");

    // A resource still bound to a texture stage. Freeing that would be a use-after-free in a game that
    // releases textures at times, which is to say eventually.
    PixelContainer *t = (PixelContainer *)D3DDevice_CreateTexture2(16, 16, 1, 1, 0, kFormatMip, 2);
    if (t) {
        D3DDevice_SetTexture(0, t);
        const size_t before = resource_record_count();
        D3DResource_Release(t);
        check_eq(resource_record_count(), before, "a bound texture survives a release rather than being freed");
        // ...and it goes as soon as it comes off the stage. The Windows loader kept it forever and called
        // that fine; here the free is deferred rather than skipped, which is the difference between a
        // resource that is late and one that is lost.
        D3DDevice_SetTexture(0, nullptr);
        check_eq(resource_record_count(), before - 1, "and is freed once it comes off its texture stage");
        D3DDevice_SetTexture(0, nullptr);
        check_eq(resource_record_count(), before - 1, "and unbinding again frees nothing a second time");
    }

    // An over-release is the game's, and the console clamped it rather than wrapping the counter to 65535
    // and turning every later comparison into a lie.
    PixelContainer *u = (PixelContainer *)D3DDevice_CreateTexture2(8, 8, 1, 1, 0, kFormatMip, 2);
    if (u) {
        u->Common = (RTYPE_TEXTURE << COMMON_TYPE_SHIFT) | COMMON_D3DCREATED; // a count of zero already
        check_eq(D3DResource_Release(u), 0, "releasing an already-zero-count resource answers zero");
        // The header is still there -- a zero count means "nothing to release", not "release anyway" -- so
        // reading it back is legal, and the count must not have wrapped around to 65535.
        check_eq(u->Common & COMMON_REFCOUNT_MASK, 0, "and leaves the count at zero instead of wrapping");
        D3D_DestroyResource(u);
    }

    // Null is a pointer the game should never pass, but a null is not a reason to fault.
    D3DResource_AddRef(nullptr);
    D3DResource_Release(nullptr);
    D3D_DestroyResource(nullptr);
    check(true, "null is survived by every lifetime row");
}

static void test_the_table_is_whole()
{
    printf("\nthe table\n");

    size_t n = 0;
    const Hle *rows = resource_rows(&n);
    check(rows != nullptr && n == 18, "all eighteen resource rows are present");
    if (!rows) return;

    // Every row has to be buildable, or the game runs the original console code for that entry.
    TableReport report = build_table(rows, n);
    check_eq(report.built, n, "and every one of them builds a thunk");
    for (size_t i = 0; i < report.refused.size(); i++)
        printf("  ....  %s at 0x%08x: %s\n", report.refused[i].name.c_str(), report.refused[i].address,
               report.refused[i].reason.c_str());

    // No two rows at one address: whichever installed last would win and the other never runs.
    size_t clashes = 0;
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (rows[i].addr == rows[j].addr) clashes++;
    check_eq(clashes, 0, "and no two claim the same entry point");

    // The per-build fact: an address outside the D3D section is a row copied from another build, and it
    // would patch the wrong function without saying so.
    size_t out_of_section = 0, without_impl = 0, unnamed = 0;
    for (size_t i = 0; i < n; i++) {
        if (rows[i].addr < 0x00260000 || rows[i].addr > 0x00280000) out_of_section++;
        if (!rows[i].impl) without_impl++;
        if (!rows[i].name || !rows[i].name[0]) unnamed++;
    }
    check_eq(out_of_section, 0, "every address is in the image's D3D section");
    check_eq(without_impl, 0, "every row names an implementation");
    check_eq(unnamed, 0, "and a name to be reported by when something is wrong");
}

int main()
{
    printf("BLACK loader -- resource creation, locking and lifetime\n");
    const size_t at_start = resource_record_count();

    test_level_geometry_of_a_mip_texture();
    test_a_compressed_level_rounds_to_blocks();
    test_cube_faces_are_128_byte_aligned();
    test_a_linear_container_reads_its_pitch_from_the_size_word();
    test_a_vertex_buffer_locks_onto_its_own_block();
    test_a_palette_locks_like_a_vertex_buffer();
    test_levels_are_addressable_through_surfaces();
    test_lock_rect_hands_back_the_level();
    test_a_rectangle_lock_moves_the_pointer();
    test_get_level_info_agrees_with_lock_rect();
    test_cube_faces_lock_apart();
    test_create_surface_header_does_not_claim_the_data();
    test_get_level_info_returns_a_plain_pointer();
    test_what_destroy_will_not_free();
    test_the_table_is_whole();

    // Nothing above should have leaked. A record that outlives its resource is a leak the game cannot cause
    // and this port cannot see, so it is checked here rather than left to a valgrind run nobody has.
    check_eq(resource_record_count(), at_start, "every resource created was released");
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}