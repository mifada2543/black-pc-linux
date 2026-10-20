// Xbox textures into Vulkan images.
//
// The console's textures are not the host's: some are swizzled (a Morton walk over the texels, so that a
// texture cache can fetch a 2x2 group with one address pattern), some are pitch-linear, and the compressed
// ones are DXT blocks laid out in rows. None of that can be handed to Vulkan as-is, so every level is
// decoded on the CPU into B8G8R8A8, which is the one pixel format this port's colour targets already use.
//
// The decode is ported from the Windows port's texture.cpp, which is the evidence for the swizzle walk and
// the per-format bit unpacking. What changed: this decodes DXT itself instead of handing it to D3D9,
// because Vulkan's compressed formats would need the swizzled block order to be undone first and the same
// work is easier to verify in one place.
//
// The image is cached per resource and decoded at most once per frame. That is the movie's requirement --
// the game writes a new frame into the same texture every swap -- and it is also the reason the cache key
// includes the header's Data and Format: a game-owned header can be reused for a different texture, and a
// cache that trusted the address alone would show the old picture.
#include "d3d/texture_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <vector>

#include "d3d/device_posix.h"
#include "d3d/resource_posix.h"
#include "d3d/xd3d8_posix.h"
#include "platform/posix/log.h"

#define DTRACE(...) logf("[d3d] " __VA_ARGS__)

namespace xd3d {
namespace {

constexpr uint32_t kFrameNever = 0xFFFFFFFFu;

struct Entry {
    gfx::Image image;
    uint32_t format = 0;
    uint32_t data = 0;
    uint32_t frame = kFrameNever;
    bool created = false;
};

std::map<Resource *, Entry> &entries()
{
    static std::map<Resource *, Entry> map;
    return map;
}

// One sampler per stage, rebuilt only when the stage's sampler state changes. Four entries, because there
// are four stages; a state change between two draws of the same stage rebuilds it, which is cheap.
struct SamplerEntry {
    gfx::Sampler sampler;
    uint32_t state[9] = {};
    bool created = false;
};
SamplerEntry g_samplers[4];

inline uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
inline uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }
inline uint32_t expand4(uint32_t v) { return v * 17u; }
inline uint32_t expand3(uint32_t v) { return (v << 5) | (v << 2) | (v >> 1); }

// One texel of an uncompressed Xbox format, as 0xAARRGGBB (which in memory is B,G,R,A -- the byte order
// VK_FORMAT_B8G8R8A8_UNORM expects).
uint32_t decode_texel(uint32_t fmt, const uint8_t *p)
{
    const uint32_t v16 = p[0] | ((uint32_t)p[1] << 8);
    switch (fmt) {
    case XFMT_A8R8G8B8: case XFMT_LIN_A8R8G8B8: {
        uint32_t v;
        memcpy(&v, p, 4);
        return v;
    }
    case XFMT_X8R8G8B8: case XFMT_LIN_X8R8G8B8: {
        uint32_t v;
        memcpy(&v, p, 4);
        return v | 0xFF000000u;
    }
    case XFMT_R5G6B5: case XFMT_LIN_R5G6B5:
        return 0xFF000000u | expand5(v16 >> 11) << 16 | expand6((v16 >> 5) & 63) << 8 | expand5(v16 & 31);
    case XFMT_A1R5G5B5: case XFMT_LIN_A1R5G5B5:
        return ((v16 & 0x8000) ? 0xFF000000u : 0u) | expand5((v16 >> 10) & 31) << 16 |
               expand5((v16 >> 5) & 31) << 8 | expand5(v16 & 31);
    case XFMT_X1R5G5B5: case XFMT_LIN_X1R5G5B5:
        return 0xFF000000u | expand5((v16 >> 10) & 31) << 16 | expand5((v16 >> 5) & 31) << 8 | expand5(v16 & 31);
    case XFMT_A4R4G4B4: case XFMT_LIN_A4R4G4B4:
        return expand4(v16 >> 12) << 24 | expand4((v16 >> 8) & 15) << 16 | expand4((v16 >> 4) & 15) << 8 |
               expand4(v16 & 15);
    case XFMT_R6G5B5: case XFMT_LIN_R6G5B5:
        return 0xFF000000u | expand6(v16 >> 10) << 16 | expand5((v16 >> 5) & 31) << 8 | expand5(v16 & 31);
    case XFMT_L8: case XFMT_LIN_L8: return 0xFF000000u | p[0] * 0x010101u;
    case XFMT_AL8: case XFMT_LIN_AL8: return p[0] * 0x01010101u;
    case XFMT_A8: case XFMT_LIN_A8: return ((uint32_t)p[0] << 24) | 0x00FFFFFFu;
    case XFMT_A8L8: case XFMT_LIN_A8L8: return ((uint32_t)p[1] << 24) | p[0] * 0x010101u;
    case XFMT_L16: case XFMT_LIN_L16: return 0xFF000000u | p[1] * 0x010101u;
    case XFMT_R8B8: case XFMT_LIN_R8B8: return 0xFF000000u | ((uint32_t)p[1] << 16) | p[0];
    case XFMT_G8B8: case XFMT_LIN_G8B8: return 0xFF000000u | ((uint32_t)p[1] << 8) | p[0];
    case XFMT_V8U8: return 0xFF000000u | ((p[0] ^ 0x80u) << 16) | ((p[1] ^ 0x80u) << 8);
    case XFMT_A8B8G8R8: case XFMT_LIN_A8B8G8R8: {
        uint32_t v;
        memcpy(&v, p, 4);
        return (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
    }
    case XFMT_B8G8R8A8: case XFMT_LIN_B8G8R8A8: {
        uint32_t v;
        memcpy(&v, p, 4);
        return (v << 24) | (v >> 8);
    }
    case XFMT_R8G8B8A8: case XFMT_LIN_R8G8B8A8: {
        uint32_t v;
        memcpy(&v, p, 4);
        return (v << 24) | (((v >> 24) & 0xFFu) << 16) | (((v >> 16) & 0xFFu) << 8) | ((v >> 8) & 0xFFu);
    }
    // Depth formats are readable as a texture on the console (a shadow map). The value is not a colour, so
    // the dot-product of the 24-bit depth into grey is what the Windows port did and what this keeps.
    case XFMT_D24S8: case XFMT_LIN_D24S8: case XFMT_F24S8: case XFMT_LIN_F24S8: {
        uint32_t v;
        memcpy(&v, p, 4);
        const uint32_t d = v >> 24;
        return 0xFF000000u | d * 0x010101u;
    }
    case XFMT_D16: case XFMT_LIN_D16: case XFMT_F16: case XFMT_LIN_F16: return 0xFF000000u | p[1] * 0x010101u;
    default: return 0xFFFF00FFu; // magenta: a format this table does not name
    }
}

bool format_is_known(uint32_t fmt)
{
    switch (fmt) {
    case XFMT_A8R8G8B8: case XFMT_LIN_A8R8G8B8: case XFMT_X8R8G8B8: case XFMT_LIN_X8R8G8B8:
    case XFMT_R5G6B5: case XFMT_LIN_R5G6B5: case XFMT_A1R5G5B5: case XFMT_LIN_A1R5G5B5: case XFMT_X1R5G5B5:
    case XFMT_LIN_X1R5G5B5: case XFMT_A4R4G4B4: case XFMT_LIN_A4R4G4B4: case XFMT_R6G5B5:
    case XFMT_LIN_R6G5B5: case XFMT_L8: case XFMT_LIN_L8: case XFMT_AL8: case XFMT_LIN_AL8: case XFMT_A8:
    case XFMT_LIN_A8: case XFMT_A8L8: case XFMT_LIN_A8L8: case XFMT_L16: case XFMT_LIN_L16: case XFMT_R8B8:
    case XFMT_LIN_R8B8: case XFMT_G8B8: case XFMT_LIN_G8B8: case XFMT_V8U8: case XFMT_A8B8G8R8:
    case XFMT_LIN_A8B8G8R8: case XFMT_B8G8R8A8: case XFMT_LIN_B8G8R8A8: case XFMT_R8G8B8A8:
    case XFMT_LIN_R8G8B8A8: case XFMT_D24S8: case XFMT_LIN_D24S8: case XFMT_F24S8: case XFMT_LIN_F24S8:
    case XFMT_D16: case XFMT_LIN_D16: case XFMT_F16: case XFMT_LIN_F16:
        return true;
    default:
        return false;
    }
}

// The swizzle: offset bit 0 is x's lowest bit, bit 1 is y's, bit 2 x's next, and so on. The masks are built
// by walking the texture's dimensions, so a 128x256 texture and a 64x64 one both get the interleave their
// own sizes imply.
struct Swizzle {
    uint32_t mx = 0, my = 0;
    Swizzle(uint32_t w, uint32_t h)
    {
        uint32_t bit = 1;
        for (uint32_t i = 1; i < w || i < h; i <<= 1) {
            if (i < w) {
                mx |= bit;
                bit <<= 1;
            }
            if (i < h) {
                my |= bit;
                bit <<= 1;
            }
        }
    }
    static uint32_t deposit(uint32_t v, uint32_t mask)
    {
        uint32_t r = 0;
        for (uint32_t b = 1; mask; b <<= 1) {
            const uint32_t low = mask & (~mask + 1);
            if (v & b) r |= low;
            mask &= mask - 1;
        }
        return r;
    }
};

// ---- DXT ----

uint32_t dxt_color(uint32_t c)
{
    return 0xFF000000u | expand5((c >> 11) & 31) << 16 | expand6((c >> 5) & 63) << 8 | expand5(c & 31);
}

uint32_t mix_rgb(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb, uint32_t div)
{
    const uint32_t r = (((a >> 16) & 0xFF) * wa + ((b >> 16) & 0xFF) * wb) / div;
    const uint32_t g = (((a >> 8) & 0xFF) * wa + ((b >> 8) & 0xFF) * wb) / div;
    const uint32_t bl = ((a & 0xFF) * wa + (b & 0xFF) * wb) / div;
    return 0xFF000000u | r << 16 | g << 8 | bl;
}

// The colour half of a DXT block. DXT3 and DXT5 always use the four-colour mode; DXT1 chooses by comparing
// the two endpoints, which is also how it carries one-bit transparency.
void dxt_color_block(const uint8_t *src, bool always_four, uint32_t out[16])
{
    const uint32_t c0 = src[0] | ((uint32_t)src[1] << 8);
    const uint32_t c1 = src[2] | ((uint32_t)src[3] << 8);
    uint32_t bits;
    memcpy(&bits, src + 4, 4);

    uint32_t col[4];
    col[0] = dxt_color(c0);
    col[1] = dxt_color(c1);
    if (always_four || c0 > c1) {
        col[2] = mix_rgb(col[0], col[1], 2, 1, 3);
        col[3] = mix_rgb(col[0], col[1], 1, 2, 3);
    } else {
        col[2] = mix_rgb(col[0], col[1], 1, 1, 2);
        col[2] = (col[2] & 0x00FFFFFFu) | 0xFF000000u;
        col[3] = 0; // transparent black
    }
    for (uint32_t i = 0; i < 16; i++) out[i] = col[(bits >> (2 * i)) & 3];
}

void dxt_block(uint32_t fmt, const uint8_t *src, uint32_t out[16])
{
    if (fmt == XFMT_DXT1) {
        dxt_color_block(src, false, out);
        return;
    }
    if (fmt == XFMT_DXT3) {
        uint64_t alpha;
        memcpy(&alpha, src, 8);
        dxt_color_block(src + 8, true, out);
        for (uint32_t i = 0; i < 16; i++) out[i] = (out[i] & 0x00FFFFFFu) | (expand4((alpha >> (4 * i)) & 0xF) << 24);
        return;
    }
    // DXT5 alpha: two endpoints and sixteen 3-bit indices, with the interpolation rule chosen by their
    // order. The 48 index bits are read as a 64-bit value so the shift below stays one expression.
    const uint32_t a0 = src[0], a1 = src[1];
    uint32_t a[8];
    a[0] = a0;
    a[1] = a1;
    if (a0 > a1) {
        a[2] = (6 * a0 + 1 * a1) / 7;
        a[3] = (5 * a0 + 2 * a1) / 7;
        a[4] = (4 * a0 + 3 * a1) / 7;
        a[5] = (3 * a0 + 4 * a1) / 7;
        a[6] = (2 * a0 + 5 * a1) / 7;
        a[7] = (1 * a0 + 6 * a1) / 7;
    } else {
        a[2] = (4 * a0 + 1 * a1) / 5;
        a[3] = (3 * a0 + 2 * a1) / 5;
        a[4] = (2 * a0 + 3 * a1) / 5;
        a[5] = (1 * a0 + 4 * a1) / 5;
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (uint32_t i = 0; i < 6; i++) bits |= (uint64_t)src[2 + i] << (8 * i);
    dxt_color_block(src + 8, true, out);
    for (uint32_t i = 0; i < 16; i++) out[i] = (out[i] & 0x00FFFFFFu) | (a[(bits >> (3 * i)) & 7] << 24);
}

uint32_t yuv_to_rgb(int y, int u, int v)
{
    const int c = y - 16, d = u - 128, e = v - 128;
    auto clamp = [](int x) { return x < 0 ? 0 : x > 255 ? 255 : x; };
    const int r = clamp((298 * c + 409 * e + 128) >> 8);
    const int g = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
    const int b = clamp((298 * c + 516 * d + 128) >> 8);
    return 0xFF000000u | r << 16 | g << 8 | b;
}

// Decode one level into B8G8R8A8. `w`/`h` come back as the level's own size, which for a mip is smaller
// than the container's.
bool decode_level(const PixelContainer *pc, uint32_t level, std::vector<uint8_t> *out, uint32_t *ow, uint32_t *oh,
                  const char **err)
{
    const uint32_t fmt = format_of(pc->Format);
    const FormatInfo &fi = format_info(fmt);

    uint32_t pitch = 0, w = 0, h = 0, bytes = 0;
    const uint32_t off = texture_level_offset(pc, 0, level, &pitch, &w, &h, &bytes);
    const uint8_t *base = (const uint8_t *)resource_data(pc);
    if (!base) {
        if (err) *err = "the texture has no data block";
        return false;
    }
    if (!w || !h) {
        if (err) *err = "the texture has no pixels";
        return false;
    }

    out->assign((size_t)w * h * 4, 0);
    uint32_t *dst = (uint32_t *)out->data();
    const uint8_t *src = base + off;

    if (fi.compressed) {
        if (fmt != XFMT_DXT1 && fmt != XFMT_DXT3 && fmt != XFMT_DXT5) {
            if (err) *err = "a compressed format this port has not named";
            return false;
        }
        // The table's bpp is bits per texel; a block covers 16 texels, so its stride is bpp * 16 / 8. Using
        // bpp itself (the first version of this line) walks DXT5 in 8-byte steps and overlaps every block by
        // half, which decodes to coloured stripes rather than to a picture.
        const uint32_t block_bytes = fi.bpp * 2;
        const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (uint32_t by = 0; by < bh; by++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                uint32_t texels[16];
                dxt_block(fmt, src + (size_t)by * pitch + (size_t)bx * block_bytes, texels);
                for (uint32_t y = 0; y < 4; y++) {
                    const uint32_t py = by * 4 + y;
                    if (py >= h) break;
                    for (uint32_t x = 0; x < 4; x++) {
                        const uint32_t px = bx * 4 + x;
                        if (px >= w) break;
                        dst[(size_t)py * w + px] = texels[y * 4 + x];
                    }
                }
            }
        }
        *ow = w;
        *oh = h;
        return true;
    }

    if (!format_is_known(fmt)) {
        if (err) *err = "a texture format this port has not translated";
        return false;
    }

    // Pitch-linear. The pitch is the container's, which texture_level_offset reads back out of Size.
    if (fi.linear || pc->Size) {
        const uint32_t bpp = fi.bpp / 8;
        if (bpp == 0 || pitch < (size_t)w * bpp) {
            if (err) *err = "a linear texture whose pitch is narrower than its width";
            return false;
        }
        if (fmt == XFMT_YUY2 || fmt == XFMT_UYVY) {
            for (uint32_t y = 0; y < h; y++) {
                const uint8_t *s = src + (size_t)y * pitch;
                uint32_t *d = dst + (size_t)y * w;
                for (uint32_t x = 0; x + 1 < w; x += 2, s += 4) {
                    int y0, y1, u, v;
                    if (fmt == XFMT_YUY2) {
                        y0 = s[0];
                        u = s[1];
                        y1 = s[2];
                        v = s[3];
                    } else {
                        u = s[0];
                        y0 = s[1];
                        v = s[2];
                        y1 = s[3];
                    }
                    d[x] = yuv_to_rgb(y0, u, v);
                    d[x + 1] = yuv_to_rgb(y1, u, v);
                }
            }
        } else {
            for (uint32_t y = 0; y < h; y++) {
                const uint8_t *s = src + (size_t)y * pitch;
                uint32_t *d = dst + (size_t)y * w;
                for (uint32_t x = 0; x < w; x++) d[x] = decode_texel(fmt, s + (size_t)x * bpp);
            }
        }
        *ow = w;
        *oh = h;
        return true;
    }

    // Swizzled: the texel at (x, y) is at the bit-interleaved offset of x and y. The walk keeps the Morton
    // offset incrementally, which is the Windows port's loop and not an accident: recomputing the interleave
    // per texel is the same result at several times the cost.
    Swizzle sw(w, h);
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t oy = Swizzle::deposit(y, sw.my);
        uint32_t *d = dst + (size_t)y * w;
        uint32_t ox = 0;
        for (uint32_t x = 0; x < w; x++) {
            d[x] = decode_texel(fmt, src + (size_t)(ox | oy) * (fi.bpp / 8));
            ox = (ox - sw.mx) & sw.mx;
        }
    }
    *ow = w;
    *oh = h;
    (void)bytes;
    return true;
}

gfx::AddressMode address_code(uint32_t v)
{
    switch (v) {
    case 2: return gfx::ADDRESS_MIRROR;
    case 3:
    case 5: return gfx::ADDRESS_CLAMP;
    case 4: return gfx::ADDRESS_BORDER;
    default: return gfx::ADDRESS_WRAP;
    }
}

// ---- optional texture dump ----
//
// BLACK_DUMP_TEXTURES=<dir> writes each distinct decoded picture this process produces as a PPM, and -- for
// the compressed formats -- as a DDS of the raw source bytes behind a standard header. The pair separates
// the two questions a garbled picture asks: whether the bytes are a picture at all (the DDS, read by any
// other decoder) and whether this port's decode of them is right. Keyed by the pixels, not the resource:
// the game reuses one texture allocation for a sequence of pictures, and an address-keyed dump shows only
// the first of them. The loop variable is not `h`, which is the height.
void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

void dump_texture(const PixelContainer *pc, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    static const char *dir = getenv("BLACK_DUMP_TEXTURES");
    if (!dir || !*dir) return;
    static std::map<uint64_t, int> seen;
    const uint32_t fmt = format_of(pc->Format);
    // Keyed by the pixels' own hash, not by the resource's address: the game reuses one texture allocation
    // for a sequence of different pictures on the loading screen, and an address-keyed dump shows only the
    // first of them -- which is exactly the picture that is not in question when a later one is garbled.
    uint64_t digest = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < (size_t)w * h * 4; i++) {
        digest ^= rgba[i];
        digest *= 0x100000001b3ull;
    }
    const uint64_t key = digest ^ ((uint64_t)fmt << 48);
    if (seen.count(key) || seen.size() >= 200) return;
    const int n = (int)seen.size();
    seen[key] = n;

    char path[512];
    snprintf(path, sizeof path, "%s/tex-%03d-%08x-%ux%u.ppm", dir, n, pc->Data, w, h);
    if (FILE *f = fopen(path, "wb")) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        // The buffer is B,G,R,A in memory because that is what B8G8R8A8_UNORM uploads; a PPM wants R,G,B.
        for (size_t i = 0; i < (size_t)w * h; i++) {
            const uint8_t rgb[3] = {rgba[i * 4 + 2], rgba[i * 4 + 1], rgba[i * 4]};
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }

    if (fmt != XFMT_DXT1 && fmt != XFMT_DXT3 && fmt != XFMT_DXT5) return;
    const uint8_t *src = (const uint8_t *)resource_data(pc);
    if (!src) return;
    uint32_t p = 0, lw = 0, lh = 0, b = 0;
    const uint32_t off = texture_level_offset(pc, 0, 0, &p, &lw, &lh, &b);
    snprintf(path, sizeof path, "%s/tex-%03d-%08x-%ux%u.dds", dir, n, pc->Data, w, h);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const uint32_t fourcc = fmt == XFMT_DXT1 ? 0x31545844u : fmt == XFMT_DXT3 ? 0x33545844u : 0x35545844u;
    put32(f, 0x20534444u); // "DDS "
    put32(f, 124);
    put32(f, 0x00081007u); // caps | height | width | pixel format | linear size
    put32(f, h);
    put32(f, w);
    put32(f, b); // the whole top level
    put32(f, 0);
    put32(f, 0);
    for (int i = 0; i < 11; i++) put32(f, 0);
    put32(f, 32);
    put32(f, 0x4u); // DDPF_FOURCC
    put32(f, fourcc);
    for (int i = 0; i < 5; i++) put32(f, 0);
    put32(f, 0x1000u); // DDSCAPS_TEXTURE
    for (int i = 0; i < 4; i++) put32(f, 0);
    fwrite(src + off, 1, b, f);
    fclose(f);
}

gfx::FilterMode filter_code(uint32_t v, bool mip)
{
    // The console's filter values are D3DTEXF_*: 0 none, 1 point, 2 linear, 3 anisotropic, 4/5 the
    // pyramidal ones. Anything this port has not translated becomes linear, which is the filter the game
    // asks for on every texture it draws at this point.
    if (v == 1 || (v == 0 && !mip)) return gfx::FILTER_POINT;
    return gfx::FILTER_LINEAR;
}

} // namespace

const gfx::Image *texture_image(Resource *texture, const char **err)
{
    if (err) *err = nullptr;
    if (!texture || !texture->Data) {
        if (err) *err = "the texture has no data";
        return nullptr;
    }
    const uint32_t type = resource_type(texture);
    if (type != RTYPE_TEXTURE && type != RTYPE_SURFACE) {
        if (err) *err = "the resource is not a texture";
        return nullptr;
    }
    if (!gfx::ready()) {
        if (err) *err = "there is no graphics device yet";
        return nullptr;
    }

    PixelContainer *pc = (PixelContainer *)texture;
    const uint32_t fmt = format_of(pc->Format);
    if (pc->Format & FMT_CUBEMAP) {
        if (err) *err = "cube textures are not uploaded yet";
        return nullptr;
    }
    uint32_t w = 0, h = 0, d = 0, levels = 0;
    pixel_container_dims(pc, &w, &h, &d, &levels);
    if (d != 1) {
        if (err) *err = "volume textures are not uploaded yet";
        return nullptr;
    }
    if (w == 0 || h == 0 || w > 4096 || h > 4096) {
        if (err) *err = "the texture's size is not one this port can upload";
        return nullptr;
    }

    Entry &e = entries()[texture];
    if (!e.created || e.format != pc->Format || e.data != pc->Data) {
        DTRACE("texture %p data 0x%08x fmt 0x%08x %ux%u levels %u size 0x%08x", (void *)texture, pc->Data,
               pc->Format, w, h, levels, pc->Size);
        if (e.created) gfx::destroy_image(&e.image);
        e.image = gfx::Image{};
        if (!gfx::create_texture(&e.image, w, h, gfx::ATT_B8G8R8A8_UNORM, 1, true)) {
            if (err) *err = gfx::last_error();
            e.created = false;
            return nullptr;
        }
        e.created = true;
        e.format = pc->Format;
        e.data = pc->Data;
        e.frame = kFrameNever;
    }

    if (e.frame == frame_count()) return &e.image;

    std::vector<uint8_t> pixels;
    uint32_t lw = 0, lh = 0;
    const char *why = nullptr;
    if (!decode_level(pc, 0, &pixels, &lw, &lh, &why)) {
        if (err) *err = why;
        return nullptr;
    }
    if (lw != w || lh != h) {
        if (err) *err = "the texture's first level is not its declared size";
        return nullptr;
    }
    dump_texture(pc, pixels.data(), lw, lh);
    if (!gfx::upload_texture(&e.image, pixels.data(), (uint64_t)pixels.size())) {
        if (err) *err = gfx::last_error();
        return nullptr;
    }
    e.frame = frame_count();
    (void)fmt;
    return &e.image;
}

const gfx::Sampler *texture_sampler(uint32_t stage, const char **err)
{
    if (err) *err = nullptr;
    if (stage >= 4) {
        if (err) *err = "a texture stage beyond the fourth";
        return nullptr;
    }
    const uint32_t *ts = texture_state((int)stage);
    uint32_t state[9];
    for (uint32_t i = 0; i < 9; i++) state[i] = ts[i];

    SamplerEntry &e = g_samplers[stage];
    if (e.created && memcmp(e.state, state, sizeof state) == 0) return &e.sampler;
    if (e.created) {
        gfx::destroy_sampler(&e.sampler);
        e.created = false;
    }

    gfx::SamplerDesc desc;
    desc.u = address_code(ts[0]);
    desc.v = address_code(ts[1]);
    desc.w = address_code(ts[2]);
    desc.mag_filter = filter_code(ts[3], false);
    desc.min_filter = filter_code(ts[4], false);
    desc.mip_filter = filter_code(ts[5], true);
    float bias;
    memcpy(&bias, &ts[6], sizeof bias);
    desc.lod_bias = bias;
    desc.max_anisotropy = ts[8] ? (float)ts[8] : 1.0f;
    if (ts[8] == 0) desc.max_anisotropy = 1.0f;

    if (!gfx::create_sampler(&e.sampler, desc)) {
        if (err) *err = gfx::last_error();
        return nullptr;
    }
    memcpy(e.state, state, sizeof state);
    e.created = true;
    return &e.sampler;
}

void texture_forget(Resource *texture)
{
    auto it = entries().find(texture);
    if (it == entries().end()) return;
    if (it->second.created) gfx::destroy_image(&it->second.image);
    entries().erase(it);
}

void texture_forget_all()
{
    for (auto &p : entries())
        if (p.second.created) gfx::destroy_image(&p.second.image);
    entries().clear();
}

uint32_t texture_cache_size()
{
    return (uint32_t)entries().size();
}

} // namespace xd3d
