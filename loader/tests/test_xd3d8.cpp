// The Xbox D3D8 ABI, checked against values recomputed here rather than read from the header.
//
// The point of this suite is that it is a *second* source for every number the loader will live or die by.
// The header (src/d3d/xd3d8_posix.h) states a layout; this file derives the same layout from the console's
// own rules -- a dword is four bytes, a pointer is four bytes in the game, thirteen dwords then four
// pointers is 0x44 -- and compares. If someone edits a struct in the header and updates its static_assert,
// this file still fails, because it does not use the header's constants to decide what the answer should
// be. That is the whole reason A1 exists as a step rather than as a comment: four ABI bugs in this port
// (the SEH record order, the raise call's stdcall, the DPC's ret 16, the 64-bit argument taking two stack
// slots) were all found exactly this way -- by a second, independent statement of the same fact.
//
// The other half is the enum arithmetic: the render-state numbers are sparse and block-structured, and a
// value shifted by one would write the wrong slot of a 166-entry array.
#include "d3d/xd3d8_posix.h"

#include <stdio.h>
#include <string.h>

#include <string>

using namespace xd3d;

static int g_failures, g_checks;

static void check(bool cond, const std::string &what)
{
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s\n", what.c_str());
        g_failures++;
    }
    fflush(stdout);
}

static void check_eq(uint64_t got, uint64_t want, const std::string &what)
{
    char buf[200];
    snprintf(buf, sizeof buf, "%s (got %llu, want %llu)", what.c_str(), (unsigned long long)got,
             (unsigned long long)want);
    check(got == want, buf);
}

// ---- the rule everything is derived from ----------------------------------------------
//
// In the game's address space a pointer is four bytes. That is not an assumption about this port's build:
// the whole loader is an i386 process because the image is, and the header's fields that are pointers are
// pointers *in the game's memory*, so every offset below is computed with 4.

static constexpr uint32_t kDword = 4;

static void test_struct_sizes()
{
    printf("\nstruct sizes, derived from the field types\n");

    // Resource: three dwords named Common, Data, Lock.
    check_eq(sizeof(Resource), 3 * kDword, "Resource is three dwords");
    check_eq(offsetof(Resource, Data), 1 * kDword, "Data is the second dword");
    check_eq(offsetof(Resource, Lock), 2 * kDword, "Lock is the third");

    // PixelContainer adds two dwords; Surface adds one pointer to its parent container. These types derive,
    // so offsetof on them is only conditionally supported; the sizes say the same thing here and say it
    // without asking the compiler anything: the additions are exactly two dwords and exactly one pointer.
    check_eq(sizeof(PixelContainer), sizeof(Resource) + 2 * kDword, "PixelContainer adds Format and Size");
    check_eq(sizeof(Surface), sizeof(PixelContainer) + kDword, "Surface adds one pointer for Parent");
    check_eq(sizeof(Surface), 6 * kDword, "a surface is six dwords all told");

    // Palette and VertexBuffer are bare headers -- nothing after Lock.
    check_eq(sizeof(Palette), sizeof(Resource), "a palette is a bare resource header");
    check_eq(sizeof(VertexBuffer), sizeof(Resource), "so is a vertex buffer");

    // DisplayMode and Viewport are the shapes CreateDevice / SetViewport take.
    check_eq(sizeof(DisplayMode), 5 * kDword, "DisplayMode is five dwords");
    check_eq(sizeof(Viewport), 4 * kDword + 2 * sizeof(float), "Viewport is four dwords and two floats");

    // A 4x4 matrix: sixteen floats, no padding, so the game can fill it row by row.
    check_eq(sizeof(Matrix), 16 * sizeof(float), "a matrix is sixteen floats");
    check_eq(sizeof(float[4]), 4 * sizeof(float), "and each row is four of them");
}

static void test_present_parameters()
{
    printf("\nPresentParameters, counted rather than trusted\n");

    // Thirteen dwords, then BufferSurfaces[3] (three pointers) and DepthStencilSurface. 13*4 + 4*4 = 68.
    const uint32_t dwords = 13;
    check_eq(dwords * kDword + 4 * kDword, 0x44, "thirteen dwords then four pointers is 0x44");
    check_eq(sizeof(PresentParameters), 0x44, "and that is the size the game filled in");
    check_eq(offsetof(PresentParameters, Windowed), 7 * kDword, "the windowed flag is the eighth dword");
    check_eq(offsetof(PresentParameters, Flags), 10 * kDword, "Flags is the eleventh");
    check_eq(offsetof(PresentParameters, BufferSurfaces), dwords * kDword, "the surface pointers start at 0x34");
    check_eq(offsetof(PresentParameters, DepthStencilSurface), dwords * kDword + 3 * kDword,
             "DepthStencilSurface follows the three back buffers");
}

static void test_format_word()
{
    printf("\nthe Format bitfield\n");

    // The fields and their widths, from the console's layout: three flag bits, then dimension, format, mipmaps
    // and three dimensions. If two fields overlapped, one value would corrupt its neighbour.
    struct Field {
        uint32_t shift, mask;
        const char *name;
    };
    const Field fields[] = {
        {FMT_DIMENSION_SHIFT, 0x3, "dimension"},
        {FMT_FORMAT_SHIFT, 0xFF, "format"},
        {FMT_MIPMAP_SHIFT, 0xF, "mipmaps"},
        {FMT_USIZE_SHIFT, 0xF, "u size"},
        {FMT_VSIZE_SHIFT, 0xF, "v size"},
        {FMT_PSIZE_SHIFT, 0xF, "p size"},
    };
    for (const Field &f : fields) {
        check_eq((1u << f.shift) - 1u < f.mask ? 0u : 1u, 1u,
                 std::string(f.name) + " does not spill into the field below it");
    }

    // Build one and take it apart again: this is what game code does when it writes the field by hand and
    // what the HLE does when it reads it.
    const uint32_t packed = format_pack(XFMT_A8R8G8B8, 2, 3, 15, 15, 15, FMT_CUBEMAP);
    check_eq(format_of(packed), XFMT_A8R8G8B8, "the format survived the round trip");
    check_eq(format_usize(packed), 15, "and the u size");
    check_eq(format_vsize(packed), 15, "and the v size");
    check_eq(format_psize(packed), 15, "and the p size");
    check_eq(format_mipmaps(packed), 3, "and the mipmap count");
    check(format_is_cubemap(packed), "and the cube map flag");
    check_eq(format_pack(XFMT_L8, 0, 0, 1, 1, 1), format_pack(XFMT_L8, 0, 0, 1, 1, 1),
             "packing is deterministic");

    // The Size field: width in the low 12 bits, height from 12, pitch from 24. Zero width means swizzled.
    const uint32_t sz = size_pack(640, 480, 128);
    check_eq(size_width(sz), 640, "width round-trips");
    check_eq(size_height(sz), 480, "height round-trips");
    check_eq(size_pitch(sz), 128, "pitch round-trips");
    check_eq(size_width(size_pack(0, 0, 0)), 0, "a zero width is the swizzled case, not an error");
    // Pitch is eight bits. A 640x480 surface's real pitch is 2560, which does not fit -- and is not meant to:
    // the console leaves pitch out of this field for anything that wide and the upload path computes it from
    // width x bpp. So the helper masks it instead of letting the overflow land in the field above.
    check_eq(size_pitch(size_pack(640, 480, 2560)), 0, "a pitch wider than 8 bits is dropped, not wrapped");
}

static void test_format_table()
{
    printf("\nthe format table\n");

    // Two names for one value would make a switch silently drop a case; the table is indexed by the number,
    // so a duplicate is a bug the compiler cannot see.
    struct Named {
        uint32_t value;
        const char *name;
    };
    const Named named[] = {
        {XFMT_L8, "L8"}, {XFMT_AL8, "AL8"}, {XFMT_A1R5G5B5, "A1R5G5B5"}, {XFMT_X1R5G5B5, "X1R5G5B5"},
        {XFMT_A4R4G4B4, "A4R4G4B4"}, {XFMT_R5G6B5, "R5G6B5"}, {XFMT_A8R8G8B8, "A8R8G8B8"},
        {XFMT_X8R8G8B8, "X8R8G8B8"}, {XFMT_P8, "P8"}, {XFMT_DXT1, "DXT1"}, {XFMT_DXT3, "DXT3"},
        {XFMT_DXT5, "DXT5"}, {XFMT_LIN_L8, "LIN_L8"}, {XFMT_LIN_A8R8G8B8, "LIN_A8R8G8B8"},
        {XFMT_D24S8, "D24S8"}, {XFMT_F24S8, "F24S8"}, {XFMT_D16, "D16"}, {XFMT_LIN_F16, "LIN_F16"},
        {XFMT_L16, "L16"}, {XFMT_V16U16, "V16U16"}, {XFMT_A8, "A8"}, {XFMT_A8L8, "A8L8"},
    };
    size_t duplicates = 0;
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        for (size_t j = i + 1; j < sizeof named / sizeof named[0]; j++)
            if (named[i].value == named[j].value) duplicates++;
    check_eq(duplicates, 0, "no two format names share a value");

    // Bit depths that the upload path depends on: DXT1 is 4 bits per texel, DXT3/5 are 8. Getting DXT1 wrong
    // halves every compressed upload and looks like a corrupt texture, nothing like a format bug.
    check_eq(format_info(XFMT_DXT1).bpp, 4, "DXT1 is 4 bits per pixel");
    check_eq(format_info(XFMT_DXT3).bpp, 8, "DXT3 is 8");
    check_eq(format_info(XFMT_DXT5).bpp, 8, "DXT5 is 8");
    check(format_info(XFMT_DXT1).compressed, "and the DXT formats are marked compressed");
    check(!format_info(XFMT_A8R8G8B8).compressed, "while A8R8G8B8 is not");

    // LIN_ variants are pitch-linear and sized by the Size field; the non-LIN ones are swizzled.
    check(format_info(XFMT_LIN_A8R8G8B8).linear, "LIN_A8R8G8B8 is pitch-linear");
    check(format_info(XFMT_LIN_F16).linear, "LIN_F16 is pitch-linear");
    check(!format_info(XFMT_A8R8G8B8).linear, "A8R8G8B8 is swizzled");
    check(!format_info(XFMT_D16).linear, "D16 is swizzled");

    // Depth and stencil read back as one 32-bit word on the console, which is why the Vulkan side wanted
    // D32_SFLOAT rather than a 24/8 split.
    check_eq(format_info(XFMT_D24S8).bpp, 32, "D24S8 counts as 32 bits per pixel");
    check_eq(format_info(XFMT_F24S8).bpp, 32, "and so does F24S8");

    // Anything outside the console's value space is reported as unknown rather than guessed at.
    check(std::string(format_info(0xFE).name) == "?", "an unknown format is named '?', not invented");
    check(std::string(format_info(0x1000).name) == "?", "and one past the table is too");
    check_eq(format_info(0xFE).bpp, 32, "with the conservative 32-bit default the Windows port used");

    // Every named format must be findable by its own value, with its own name.
    size_t mismatched = 0;
    for (const Named &n : named)
        if (std::string(format_info(n.value).name) != n.name) mismatched++;
    check_eq(mismatched, 0, "each format reports its own name");
}

static void test_render_state_numbering()
{
    printf("\nthe render state numbering\n");

    // Three sparse blocks. These four anchors are what the shadow array is indexed by; a value shifted by
    // one writes the wrong slot and the symptom is one wrong state somewhere else entirely.
    check_eq(XRS_PSALPHAINPUTS0, 0, "the combiner block starts at 0");
    check_eq(XRS_ZFUNC, 57, "the simple block starts at 57");
    check_eq(XRS_FOGENABLE, 92, "the deferred block starts at 92");
    check_eq(XRS_PSTEXTUREMODES, 136, "the complex block starts at 136");
    check_eq(XRS_MAX, 166, "and the table is 166 entries");

    // Within a block the numbering must be dense -- the enum relies on it, so "XRS_ALPHAFUNC" being 58 is
    // not an assumption, it is what makes the next name's number right.
    check_eq(XRS_ALPHAFUNC, 58, "the simple block is dense after its anchor");
    check_eq(XRS_STIPPLEENABLE, 83, "and ends at 83, as the block comment says");
    check_eq(XRS_FOGTABLEMODE, 93, "the deferred block is dense after its anchor");
    check_eq(XRS_PRESENTATIONINTERVAL, 127, "and ends at 127");
    // The complex block's last entry is DONOTCULLUNCOMPRESSED, not ROPZREAD -- which is the mistake this
    // suite exists to catch: the invoice's comment "// 165" sits after a list of names, and reading it as
    // "ROPZREAD is 165" is off by one. Counting the block from its anchor settles it.
    check_eq(XRS_ROPZREAD, 164, "ROPZREAD is second to last in the complex block");
    check_eq(XRS_DONOTCULLUNCOMPRESSED, 165, "and DONOTCULLUNCOMPRESSED is the last entry");
    check_eq(XRS_DONOTCULLUNCOMPRESSED + 1, XRS_MAX, "so MAX is exactly one past it");

    // The shadow arrays the game writes in place must be big enough for the values it will use.
    check_eq(RENDERSTATE_COUNT, XRS_MAX, "the render state array is sized by MAX");
    check(XRS_MAX > XRS_ROPZREAD, "and every state fits in it");
}

static void test_device_object()
{
    printf("\nthe device object view\n");

    // The offsets the game writes. Each one is inside the object, and none two are the same field.
    struct Field {
        uint32_t off, size;
        const char *name;
    };
    const Field fields[] = {
        {DEV_PUT, 4, "PUT"},           {DEV_THRESHOLD, 4, "THRESHOLD"},
        {DEV_STATEFLAGS, 4, "STATEFLAGS"}, {DEV_GPUTIME, 4, "GPUTIME"},
        {DEV_SCREENOFFSET_X, 4, "screen offset x"}, {DEV_SCREENOFFSET_Y, 4, "screen offset y"},
        {DEV_TEXTURES, 16, "texture array"},      {DEV_VERTEXSHADER, 4, "vertex shader"},
        {DEV_RENDERTARGET, 4, "render target"},   {DEV_ZBUFFER, 4, "z buffer"},
        {DEV_SWAPCALLBACK, 4, "swap callback"},   {DEV_VBLANKCALLBACK, 4, "vblank callback"},
        {DEV_VBLANKEVENT, 4, "vblank event"},     {DEV_SWAPCOUNT, 4, "swap count"},
        {DEV_GAMMA_RAMP, 2 * 0x300, "gamma ramps"}, {DEV_GAMMA_DIRTY, 8, "gamma dirty flags"},
        {DEV_GAMMA_INDEX, 4, "gamma index"},
    };
    size_t overlaps = 0, outside = 0;
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        if (fields[i].off + fields[i].size > DEVICE_SIZE) outside++;
        for (size_t j = i + 1; j < sizeof fields / sizeof fields[0]; j++) {
            const uint32_t a0 = fields[i].off, a1 = fields[i].off + fields[i].size;
            const uint32_t b0 = fields[j].off, b1 = fields[j].off + fields[j].size;
            if (a0 < b1 && b0 < a1) overlaps++;
        }
    }
    check_eq(outside, 0, "every named field fits inside the 0x24A8-byte object");
    check_eq(overlaps, 0, "and no two named fields overlap");

    // Gamma ramp arithmetic: two 256-byte-per-channel buffers, and the dirty flag pair lives 0x200 bytes
    // after the second one starts. These three numbers only make sense together, so they are checked together.
    check_eq(DEV_GAMMA_RAMP + 0x300, DEV_GAMMA_RAMP + 2 * 0x300 - 0x300, "the second ramp starts 0x300 later");
    check_eq(DEV_GAMMA_DIRTY - DEV_GAMMA_RAMP, 0x600, "the dirty flags are two ramps past the first");
    check_eq(DEV_GAMMA_INDEX - DEV_GAMMA_DIRTY, 8, "and the index follows the two dirty flags");

    // Transforms and viewport: ten 0x40-byte matrices, and the viewport starts exactly where they end.
    // SetTransform writes State*0x40 into the object, so a count or a size that is one off lands the ninth
    // transform on top of the viewport -- and the viewport is what the game reads to decide where pixels go.
    check_eq(TRANSFORM_COUNT, 10, "there are ten transforms, world through texture7");
    check_eq(sizeof(Matrix), 0x40, "each one is 0x40 bytes");
    check_eq(DEV_TRANSFORMS + TRANSFORM_COUNT * 0x40, DEV_VIEWPORT,
             "and the viewport begins exactly where the tenth transform ends");
    check_eq(XTS_WORLD, 0, "world is the first transform");
    check_eq(XTS_VIEW, 1, "and view is the second");
    // Eleven named states, ten slots: the console's SetTransform ignores anything past nine, so
    // XTS_TEXTURE7 (10) is a name with nowhere to go. That off-by-one is why the guard is `>= 10` and
    // why the object holds ten matrices rather than eleven.
    check_eq(XTS_TEXTURE7, 10, "texture7 is the eleventh name, past the last slot");
    check_eq(XTS_TEXTURE6 + 1, TRANSFORM_COUNT, "so the last state with a slot is texture6");
    check(XTS_TEXTURE7 >= TRANSFORM_COUNT, "and texture7 is the one that gets ignored");
    check_eq(sizeof(Viewport), 24, "the viewport slot is one D3DVIEWPORT8");

    // A write by "game code" at the measured offset must be visible through the loader's own accessor, and
    // must not disturb the neighbours. This is the property the whole object view exists for.
    static DeviceObject dev; // static: 0x24A8 bytes on the stack would be silly but legal
    memset(dev.bytes, 0, sizeof dev.bytes);
    __builtin_memcpy(dev.bytes + DEV_PUT, "abcd", 4);
    __builtin_memcpy(dev.bytes + DEV_SWAPCOUNT, "\x07\x00\x00\x00", 4);
    dev.set_render_target((Surface *)(uintptr_t)0x00550000);
    dev.set_depth_stencil((Surface *)(uintptr_t)0x00560000);
    check_eq(dev.put(), 0x64636261u, "PUT reads back as the bytes the game wrote");
    check_eq(dev.swap_count(), 7, "and so does the swap count");
    check(dev.render_target() == (Surface *)(uintptr_t)0x00550000, "the render target round-trips");
    check(dev.depth_stencil() == (Surface *)(uintptr_t)0x00560000, "and the depth buffer");
    check_eq(dev.threshold(), 0, "writing those left the neighbouring fields alone");
    check_eq(sizeof(DeviceObject), DEVICE_SIZE, "the view is exactly the console's object size");
}

static void test_enums()
{
    printf("\nsmall enums\n");

    // Clear: the colour mask is the high nibble, which is why XCLEAR_TARGET is not 1 like the others.
    check_eq(XCLEAR_ZBUFFER, 1, "clearing depth is bit 0");
    check_eq(XCLEAR_STENCIL, 2, "stencil is bit 1");
    check_eq(XCLEAR_TARGET, 0xF0, "colour is the high nibble: all four channels");
    check_eq(XCLEAR_TARGET | XCLEAR_ZBUFFER, 0xF1, "so clearing colour and depth is 0xF1");

    // Primitive types equal the NV097_SET_BEGIN_END values; they start at 1 because 0 means "no primitive".
    check_eq(XPT_POINTLIST, 1, "point lists are 1");
    check_eq(XPT_TRIANGLELIST, 5, "triangle lists are 5");
    check_eq(XPT_POLYGON, 10, "polygons are the last one, 10");
}

int main()
{
    printf("BLACK loader -- the Xbox D3D8 ABI\n");

    test_struct_sizes();
    test_present_parameters();
    test_format_word();
    test_format_table();
    test_render_state_numbering();
    test_device_object();
    test_enums();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}