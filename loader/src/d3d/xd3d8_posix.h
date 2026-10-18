// The Xbox D3D8 ABI as the *game* sees it: resource headers it reads in place, the device object it
// pokes fields of directly, the enums it passes, and the shapes of the structures it fills in.
//
// Ported from loader/windows/src/d3d/xd3d.h (the invoice). The values are not invented here: every one of
// them was measured on the real image by the Windows port, and the comments saying *where a number came
// from* and *what breaks without it* are copied across with it, because that is the part a port cannot
// rediscover. Nothing in this file needs a D3D SDK, a Windows header, or the game image -- it is pure
// layout, so it can be built and checked today (tests/test_xd3d8.cpp recomputes every size and offset
// independently and compares).
//
// What this header deliberately does NOT have: anything to do with the host graphics API. The Windows
// port put IDirect3DDevice9 next to the Xbox state; here the host side is Vulkan (loader/src/gfx/), and the
// Xbox-visible half stands alone.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace xd3d {

// ---- resources ----
//
// A D3D8 resource is a header the *game* owns: it allocates it from Xbox contiguous memory, fills the
// fields, and passes the pointer to D3D. So the layout is not negotiable in either direction -- HLE code
// reads these fields, and game code writes them.

constexpr uint32_t COMMON_REFCOUNT_MASK = 0x0000FFFF;
constexpr uint32_t COMMON_TYPE_MASK = 0x00070000;
constexpr uint32_t COMMON_TYPE_SHIFT = 16;
constexpr uint32_t COMMON_INTREFCOUNT_MASK = 0x00780000;
constexpr uint32_t COMMON_VIDEOMEMORY = 0x00800000;
constexpr uint32_t COMMON_D3DCREATED = 0x01000000;

enum ResourceType : uint32_t {
    RTYPE_VERTEXBUFFER = 0,
    RTYPE_INDEXBUFFER = 1,
    RTYPE_PUSHBUFFER = 2,
    RTYPE_PALETTE = 3,
    RTYPE_TEXTURE = 4,
    RTYPE_SURFACE = 5,
    RTYPE_FIXUP = 6,
};

struct Resource {
    uint32_t Common;
    uint32_t Data; // physical address (contiguous memory) of the data
    uint32_t Lock;
};
static_assert(sizeof(Resource) == 12, "D3D8 resource header is three dwords");

struct PixelContainer : Resource {
    uint32_t Format;
    uint32_t Size;
};
static_assert(sizeof(PixelContainer) == 20, "a texture/surface adds Format and Size to the header");

struct Surface : PixelContainer {
    PixelContainer *Parent;
};
static_assert(sizeof(Surface) == 24, "a surface adds one pointer: the container it is a face/level of");

struct Palette : Resource {};
struct VertexBuffer : Resource {};

// PixelContainer::Format is a bitfield, and the HLE code both reads and writes it, so the shifts are part
// of the ABI. Usize/Vsize/Psize are in texels; Mipmap is the level count minus one.
constexpr uint32_t FMT_DMA_MASK = 0x00000003;
constexpr uint32_t FMT_CUBEMAP = 0x00000004;
constexpr uint32_t FMT_BORDERSOURCE_COLOR = 0x00000008;
constexpr uint32_t FMT_DIMENSION_SHIFT = 4;
constexpr uint32_t FMT_FORMAT_SHIFT = 8;
constexpr uint32_t FMT_MIPMAP_SHIFT = 16;
constexpr uint32_t FMT_USIZE_SHIFT = 20;
constexpr uint32_t FMT_VSIZE_SHIFT = 24;
constexpr uint32_t FMT_PSIZE_SHIFT = 28;

// PixelContainer::Size (linear formats only; 0 for swizzled power-of-two).
//
// The pitch field is eight bits, which is *not* enough for a 640x480 surface (2560 bytes) -- and that is not
// an oversight here: the console leaves pitch out of this field for anything that wide, and the upload path
// computes it from width x bits-per-pixel itself (that is what texture_level_offset() does in the Windows
// port). The field is documented as it is, and size_pack() masks it rather than letting a wide value leak
// into the next field.
constexpr uint32_t SIZE_WIDTH_MASK = 0x00000FFF;
constexpr uint32_t SIZE_HEIGHT_SHIFT = 12;
constexpr uint32_t SIZE_HEIGHT_MASK = 0x00000FFF;
constexpr uint32_t SIZE_PITCH_SHIFT = 24;
constexpr uint32_t SIZE_PITCH_MASK = 0x000000FF;

// An inline build and unpack of the Format word. Game code writes this field directly, so the HLE side has
// to be able to do the same and mean the same thing -- these two are the only sanctioned way to touch it.
constexpr uint32_t format_pack(uint32_t format, uint32_t dimension, uint32_t mipmaps, uint32_t usize, uint32_t vsize,
                               uint32_t psize, uint32_t flags = 0)
{
    return (flags & (FMT_DMA_MASK | FMT_CUBEMAP | FMT_BORDERSOURCE_COLOR)) |
           (dimension << FMT_DIMENSION_SHIFT) | (format << FMT_FORMAT_SHIFT) | (mipmaps << FMT_MIPMAP_SHIFT) |
           (usize << FMT_USIZE_SHIFT) | (vsize << FMT_VSIZE_SHIFT) | (psize << FMT_PSIZE_SHIFT);
}

constexpr uint32_t format_of(uint32_t packed) { return (packed >> FMT_FORMAT_SHIFT) & 0xFF; }
constexpr uint32_t format_usize(uint32_t packed) { return (packed >> FMT_USIZE_SHIFT) & 0xF; }
constexpr uint32_t format_vsize(uint32_t packed) { return (packed >> FMT_VSIZE_SHIFT) & 0xF; }
constexpr uint32_t format_psize(uint32_t packed) { return (packed >> FMT_PSIZE_SHIFT) & 0xF; }
constexpr uint32_t format_mipmaps(uint32_t packed) { return (packed >> FMT_MIPMAP_SHIFT) & 0xF; }
constexpr bool format_is_cubemap(uint32_t packed) { return (packed & FMT_CUBEMAP) != 0; }

// Size field: width in bits 0-11, height from 12, pitch from 24. Zero width means "swizzled power of two",
// which is the Xbox's normal case -- that is exactly why the field is often unused.
constexpr uint32_t size_pack(uint32_t width, uint32_t height, uint32_t pitch)
{
    return (width & SIZE_WIDTH_MASK) | ((height & SIZE_HEIGHT_MASK) << SIZE_HEIGHT_SHIFT) |
           ((pitch & SIZE_PITCH_MASK) << SIZE_PITCH_SHIFT);
}

constexpr uint32_t size_width(uint32_t size) { return size & SIZE_WIDTH_MASK; }
constexpr uint32_t size_height(uint32_t size) { return (size >> SIZE_HEIGHT_SHIFT) & SIZE_HEIGHT_MASK; }
constexpr uint32_t size_pitch(uint32_t size) { return size >> SIZE_PITCH_SHIFT; }

// Xbox contiguous memory reads back through the top half of the address space. Data is a *physical*
// address, so every dereference goes through here; getting it wrong is an instant access violation.
inline void *phys_to_virt(uint32_t data) { return (void *)(uintptr_t)(data | 0x80000000u); }
inline uint32_t resource_type(const Resource *r) { return (r->Common & COMMON_TYPE_MASK) >> COMMON_TYPE_SHIFT; }

// Xbox D3DFORMAT values (the FMT_FORMAT field of PixelContainer::Format). These are *not* D3D9's numbers:
// the console's value space is the one the game passes, and mapping between them is the texture layer's
// job. LIN_ variants are pitch-linear and sized by the Size field rather than swizzled.
enum XFormat : uint32_t {
    XFMT_L8 = 0x00, XFMT_AL8 = 0x01, XFMT_A1R5G5B5 = 0x02, XFMT_X1R5G5B5 = 0x03, XFMT_A4R4G4B4 = 0x04,
    XFMT_R5G6B5 = 0x05, XFMT_A8R8G8B8 = 0x06, XFMT_X8R8G8B8 = 0x07, XFMT_P8 = 0x0B, XFMT_DXT1 = 0x0C,
    XFMT_DXT3 = 0x0E, XFMT_DXT5 = 0x0F, XFMT_LIN_A1R5G5B5 = 0x10, XFMT_LIN_R5G6B5 = 0x11,
    XFMT_LIN_A8R8G8B8 = 0x12, XFMT_LIN_L8 = 0x13, XFMT_LIN_R8B8 = 0x16, XFMT_LIN_G8B8 = 0x17,
    XFMT_A8 = 0x19, XFMT_A8L8 = 0x1A, XFMT_LIN_AL8 = 0x1B, XFMT_LIN_X1R5G5B5 = 0x1C,
    XFMT_LIN_A4R4G4B4 = 0x1D, XFMT_LIN_X8R8G8B8 = 0x1E, XFMT_LIN_A8 = 0x1F, XFMT_LIN_A8L8 = 0x20,
    XFMT_YUY2 = 0x24, XFMT_UYVY = 0x25, XFMT_L6V5U5 = 0x27, XFMT_V8U8 = 0x28, XFMT_R8B8 = 0x29,
    XFMT_D24S8 = 0x2A, XFMT_F24S8 = 0x2B, XFMT_D16 = 0x2C, XFMT_F16 = 0x2D, XFMT_LIN_D24S8 = 0x2E,
    XFMT_LIN_F24S8 = 0x2F, XFMT_LIN_D16 = 0x30, XFMT_LIN_F16 = 0x31, XFMT_L16 = 0x32, XFMT_V16U16 = 0x33,
    XFMT_LIN_L16 = 0x35, XFMT_LIN_V16U16 = 0x36, XFMT_LIN_L6V5U5 = 0x37, XFMT_R6G5B5 = 0x38,
    XFMT_G8B8 = 0x39, XFMT_A8B8G8R8 = 0x3A, XFMT_B8G8R8A8 = 0x3B, XFMT_R8G8B8A8 = 0x3C,
    XFMT_LIN_R6G5B5 = 0x3D, XFMT_LIN_A8B8G8R8 = 0x3E, XFMT_LIN_B8G8R8A8 = 0x3F, XFMT_LIN_R8G8B8A8 = 0x40,
    XFMT_VERTEXDATA = 0x64, XFMT_INDEX16 = 0x65,
};

// What the texture layer needs to know about a format without a table of its own: bits per pixel (DXT1 is
// 4, DXT3/5 are 8), whether it is pitch-linear (sized by the Size field), and whether it is compressed.
struct FormatInfo {
    uint8_t bpp;
    bool linear;
    bool compressed;
    const char *name;
};
const FormatInfo &format_info(uint32_t xfmt);

// ---- device creation / modes ----

struct DisplayMode { // Xbox D3DDISPLAYMODE
    uint32_t Width, Height, RefreshRate, Flags, Format;
};
static_assert(sizeof(DisplayMode) == 20, "five dwords");

// Xbox D3DPRESENT_PARAMETERS. The game fills this in and CreateDevice reads it, so both sides must agree on
// the size: the last four members are pointers, which is what makes it 0x44 rather than 0x40.
struct PresentParameters {
    uint32_t BackBufferWidth;
    uint32_t BackBufferHeight;
    uint32_t BackBufferFormat;
    uint32_t BackBufferCount;
    uint32_t MultiSampleType;
    uint32_t SwapEffect;
    uint32_t hDeviceWindow;
    uint32_t Windowed;
    uint32_t EnableAutoDepthStencil;
    uint32_t AutoDepthStencilFormat;
    uint32_t Flags;
    uint32_t FullScreen_RefreshRateInHz;
    uint32_t FullScreen_PresentationInterval;
    Surface *BufferSurfaces[3];
    Surface *DepthStencilSurface;
};
static_assert(sizeof(PresentParameters) == 0x44, "13 dwords then 4 pointers");
static_assert(offsetof(PresentParameters, BufferSurfaces) == 13 * 4, "the pointers start after the dwords");
static_assert(offsetof(PresentParameters, DepthStencilSurface) == 13 * 4 + 3 * 4, "BufferSurfaces[3] first");

struct Viewport { // D3DVIEWPORT8
    uint32_t X, Y, Width, Height;
    float MinZ, MaxZ;
};
static_assert(sizeof(Viewport) == 24, "four dwords and two floats");

// D3DRECT, the console's rectangle. Four longs in this order, which is not recalled from a Windows header:
// the invoice indexes it as pRects[i].x1/.y1/.x2/.y2 and hands x1 and y1 to a scale together as a pair, which
// is only consistent with the four longs being consecutive in that order.
//
// It is a separate type from RECT because it is a separate type in D3D8's own headers, and the two are not
// interchangeable even at the same width.
struct D3DRect {
    int32_t x1, y1, x2, y2;
};
static_assert(sizeof(D3DRect) == 16, "four longs");

// A 4x4 matrix in row-major order, which is how the Xbox keeps it and how the game fills it: 16 floats, no
// padding. (The Windows port used D3DMATRIX from d3d9.h, which happens to have the same layout -- but not
// because d3d9.h exists here.)
struct Matrix {
    float m[4][4];
};
static_assert(sizeof(Matrix) == 64, "16 floats");
static_assert(offsetof(Matrix, m) == 0, "no hidden padding before the rows");

// D3DTRANSFORMSTATETYPE on the console: ten of them, world through view, and SetTransform indexes the
// device object's ten 0x40-byte slots with these. Anything past the last is ignored rather than clamped,
// which is what the Windows port did -- a game that asks for an index it invented would otherwise have its
// matrix written over whatever followed in the object.
enum XTransform : uint32_t {
    XTS_WORLD = 0, XTS_VIEW, XTS_PROJECTION, XTS_TEXTURE0, XTS_TEXTURE1,
    XTS_TEXTURE2, XTS_TEXTURE3, XTS_TEXTURE4, XTS_TEXTURE5, XTS_TEXTURE6, XTS_TEXTURE7,
    XTS_COUNT = 10,
};
constexpr uint32_t TRANSFORM_COUNT = XTS_COUNT;

// ---- device object fields read or written directly by game code ----
//
// Offsets into the console's D3D device object, measured by the Windows port (see NOTES.md). Game code
// writes several of these itself, so the HLE side has to keep the object the same size and the fields at
// the same offsets; the comment on each is what it is for.
constexpr uint32_t DEV_PUT = 0x0000;           // push buffer write pointer
constexpr uint32_t DEV_THRESHOLD = 0x0004;     // MakeRequestedSpace when PUT reaches this
constexpr uint32_t DEV_STATEFLAGS = 0x0008;
constexpr uint32_t DEV_GPUTIME = 0x002C;       // fence / GPU time
constexpr uint32_t DEV_SCREENOFFSET_X = 0x0EF8;
constexpr uint32_t DEV_SCREENOFFSET_Y = 0x0EFC;
constexpr uint32_t DEV_TEXTURES = 0x0F98;      // Resource* [4]
constexpr uint32_t DEV_TRANSFORMS = 0x0C60;   // Matrix[10]: D3DTS_WORLD .. D3DTS_VIEW, 0x40 bytes each
constexpr uint32_t DEV_VIEWPORT = 0x0EE0;     // Viewport: starts exactly where the ten transforms end
constexpr uint32_t DEV_VERTEXSHADER = 0x0794;  // m_VertexShader (XbSymbolDatabase)
constexpr uint32_t DEV_RENDERTARGET = 0x1A04;  // Surface*
constexpr uint32_t DEV_ZBUFFER = 0x1A08;       // Surface*
constexpr uint32_t DEV_SWAPCALLBACK = 0x1DB4;  // m_SwapCallback
constexpr uint32_t DEV_VBLANKCALLBACK = 0x1DB8; // m_VBlankCallback (set by game code)
constexpr uint32_t DEV_VBLANKEVENT = 0x1DBC;   // m_VerticalBlankEvent
constexpr uint32_t DEV_SWAPCOUNT = 0x2478;
// Gamma ramp (D3DGAMMARAMP: red[256], green[256], blue[256] bytes), double-buffered: the game's ramp builder
// FUN_001762B0 writes buffer (swap count & 1) at +0x1E04 + i*0x300, makes it current (+0x240C) and sets its
// dirty flag (+0x2404 + i*4) for the vblank upload to the DAC palette.
constexpr uint32_t DEV_GAMMA_RAMP = 0x1E04;
constexpr uint32_t DEV_GAMMA_DIRTY = 0x2404;
constexpr uint32_t DEV_GAMMA_INDEX = 0x240C;
constexpr uint32_t ADDR_DEVICE_OBJECT = 0x00279B50; // static g_Device
constexpr uint32_t DEVICE_SIZE = 0x24A8;            // up to D3D_g_pDevice (0x27BFF8)
static_assert(DEV_GAMMA_RAMP + 2 * 0x300 <= DEVICE_SIZE, "both gamma ramp buffers must fit in the object");
static_assert(DEV_TRANSFORMS + TRANSFORM_COUNT * sizeof(Matrix) == DEV_VIEWPORT,
              "the ten transforms end exactly where the viewport starts -- three numbers that only make "
              "sense together, which is why they are one assertion");
static_assert(DEV_SWAPCOUNT < DEVICE_SIZE, "the swap count is inside the object");
static_assert(DEVICE_SIZE == ADDR_DEVICE_OBJECT + 0x24A8 - ADDR_DEVICE_OBJECT, "the object is 0x24A8 bytes");

// The device object as a sparse byte view. The HLE keeps the object the console had, so game writes land
// where game writes always landed; this is how the loader reads a field without declaring all 0x24A8
// bytes. Only the fields above are named -- the rest is the console's internals, which the HLE replaces.
struct DeviceObject {
    uint8_t bytes[DEVICE_SIZE];
    uint32_t put() const { uint32_t v; __builtin_memcpy(&v, bytes + DEV_PUT, 4); return v; }
    uint32_t threshold() const { uint32_t v; __builtin_memcpy(&v, bytes + DEV_THRESHOLD, 4); return v; }
    uint32_t swap_count() const { uint32_t v; __builtin_memcpy(&v, bytes + DEV_SWAPCOUNT, 4); return v; }
    uint32_t vertex_shader() const { uint32_t v; __builtin_memcpy(&v, bytes + DEV_VERTEXSHADER, 4); return v; }
    Surface *render_target() const { Surface *v; __builtin_memcpy(&v, bytes + DEV_RENDERTARGET, 4); return v; }
    Surface *depth_stencil() const { Surface *v; __builtin_memcpy(&v, bytes + DEV_ZBUFFER, 4); return v; }
    void set_render_target(Surface *s) { __builtin_memcpy(bytes + DEV_RENDERTARGET, &s, 4); }
    void set_depth_stencil(Surface *s) { __builtin_memcpy(bytes + DEV_ZBUFFER, &s, 4); }
};
static_assert(sizeof(DeviceObject) == DEVICE_SIZE, "the device object view is exactly the console's size");

// Xbox D3DRENDERSTATETYPE for XDK 5849, verified against the XbSymbolDatabase variable addresses.
//
// The numbering is not one dense list: the console's table has three blocks with gaps, and the gaps are
// load-bearing -- the render-state shadow array is XRS_MAX entries and every SetRenderState indexes it
// with these values, so a value that is shifted by one would write the wrong slot. The blocks are
// pixel-shader combiner (0..53), simple pipeline states (57..91), fog/wrap/lighting (92..127), and the
// complex ones (136..165).
enum XRS : uint32_t {
    XRS_PSALPHAINPUTS0 = 0, XRS_PSFINALCOMBINERINPUTSABCD = 8, XRS_PSFINALCOMBINERINPUTSEFG = 9,
    XRS_PSCONSTANT0_0 = 10, XRS_PSCONSTANT1_0 = 18, XRS_PSALPHAOUTPUTS0 = 26, XRS_PSRGBINPUTS0 = 34,
    XRS_PSCOMPAREMODE = 42, XRS_PSFINALCOMBINERCONSTANT0 = 43, XRS_PSFINALCOMBINERCONSTANT1 = 44,
    XRS_PSRGBOUTPUTS0 = 45, XRS_PSCOMBINERCOUNT = 53,
    // simple
    XRS_ZFUNC = 57, XRS_ALPHAFUNC, XRS_ALPHABLENDENABLE, XRS_ALPHATESTENABLE, XRS_ALPHAREF, XRS_SRCBLEND,
    XRS_DESTBLEND, XRS_ZWRITEENABLE, XRS_DITHERENABLE, XRS_SHADEMODE, XRS_COLORWRITEENABLE, XRS_STENCILZFAIL,
    XRS_STENCILPASS, XRS_STENCILFUNC, XRS_STENCILREF, XRS_STENCILMASK, XRS_STENCILWRITEMASK, XRS_BLENDOP,
    XRS_BLENDCOLOR, XRS_SWATHWIDTH, XRS_POLYGONOFFSETZSLOPESCALE, XRS_POLYGONOFFSETZOFFSET, XRS_POINTOFFSETENABLE,
    XRS_WIREFRAMEOFFSETENABLE, XRS_SOLIDOFFSETENABLE, XRS_DEPTHCLIPCONTROL, XRS_STIPPLEENABLE, // 83
    // deferred
    XRS_FOGENABLE = 92, XRS_FOGTABLEMODE, XRS_FOGSTART, XRS_FOGEND, XRS_FOGDENSITY, XRS_RANGEFOGENABLE,
    XRS_WRAP0, XRS_WRAP1, XRS_WRAP2, XRS_WRAP3, XRS_LIGHTING, XRS_SPECULARENABLE, XRS_LOCALVIEWER, XRS_COLORVERTEX,
    XRS_BACKSPECULARMATERIALSOURCE, XRS_BACKDIFFUSEMATERIALSOURCE, XRS_BACKAMBIENTMATERIALSOURCE,
    XRS_BACKEMISSIVEMATERIALSOURCE, XRS_SPECULARMATERIALSOURCE, XRS_DIFFUSEMATERIALSOURCE, XRS_AMBIENTMATERIALSOURCE,
    XRS_EMISSIVEMATERIALSOURCE, XRS_BACKAMBIENT, XRS_AMBIENT, XRS_POINTSIZE, XRS_POINTSIZE_MIN, XRS_POINTSPRITEENABLE,
    XRS_POINTSCALEENABLE, XRS_POINTSCALE_A, XRS_POINTSCALE_B, XRS_POINTSCALE_C, XRS_POINTSIZE_MAX, XRS_PATCHEDGESTYLE,
    XRS_PATCHSEGMENTS, XRS_SWAPFILTER, XRS_PRESENTATIONINTERVAL, // 127
    // complex
    XRS_PSTEXTUREMODES = 136, XRS_VERTEXBLEND, XRS_FOGCOLOR, XRS_FILLMODE, XRS_BACKFILLMODE, XRS_TWOSIDEDLIGHTING,
    XRS_NORMALIZENORMALS, XRS_ZENABLE, XRS_STENCILENABLE, XRS_STENCILFAIL, XRS_FRONTFACE, XRS_CULLMODE,
    XRS_TEXTUREFACTOR, XRS_ZBIAS, XRS_LOGICOP, XRS_EDGEANTIALIAS, XRS_MULTISAMPLEANTIALIAS, XRS_MULTISAMPLEMASK,
    XRS_MULTISAMPLEMODE, XRS_MULTISAMPLERENDERTARGETMODE, XRS_SHADOWFUNC, XRS_LINEWIDTH, XRS_SAMPLEALPHA,
    XRS_DXT1NOISEENABLE, XRS_YUVENABLE, XRS_OCCLUSIONCULLENABLE, XRS_STENCILCULLENABLE, XRS_ROPZCMPALWAYSREAD,
    XRS_ROPZREAD, XRS_DONOTCULLUNCOMPRESSED, // 165
    XRS_MAX = 166,
};
static_assert(XRS_PSALPHAINPUTS0 == 0, "the combiner block starts at zero");
static_assert(XRS_ZFUNC == 57, "the simple block starts at 57 -- SetRenderState indexes the array by these");
static_assert(XRS_FOGENABLE == 92, "the deferred block starts at 92");
static_assert(XRS_PSTEXTUREMODES == 136, "the complex block starts at 136");
static_assert(XRS_DONOTCULLUNCOMPRESSED == XRS_MAX - 1, "MAX is one past the last render state");

// Xbox data in the D3D section that game code reads or writes
constexpr uint32_t ADDR_G_PDEVICE = 0x0027BFF8;    // D3D_g_pDevice
constexpr uint32_t ADDR_PUSH_SEGMENT = 0x0027CEB0; // size the game passes to MakeRequestedSpace
constexpr uint32_t ADDR_RENDERSTATE = 0x0027C008;  // D3D_g_RenderState[166]
constexpr uint32_t ADDR_TEXTURESTATE = 0x00279948; // D3D_g_DeferredTextureState[4][32]
constexpr uint32_t ADDR_STREAMS = 0x00279758;      // D3D_g_Stream
constexpr uint32_t ADDR_DIRTYFLAGS = 0x0027C000;

// The sizes of those arrays. Game code writes the originals in place, so the HLE reads them where they are
// rather than intercepting the write -- which is why the accessors that reach them live in device_posix.h
// rather than here: the addresses are a fact about the image, and the storage is a fact about the loader.
constexpr uint32_t RENDERSTATE_COUNT = XRS_MAX;
constexpr uint32_t TEXTURESTAGE_COUNT = 4;
constexpr uint32_t TEXTURESTATE_PER_STAGE = 32;

// ---- enums ----

// Xbox D3DCLEAR. TARGET is a high nibble (it is the colour mask: 0xF0 = all four channels).
enum : uint32_t {
    XCLEAR_ZBUFFER = 0x01,
    XCLEAR_STENCIL = 0x02,
    XCLEAR_TARGET = 0xF0,
};

// D3DPRIMITIVETYPE on the console, which equals the NV097_SET_BEGIN_END values -- the draw path reads
// these to decide the primitive restart/boundary command.
enum : uint32_t {
    XPT_POINTLIST = 1, XPT_LINELIST = 2, XPT_LINELOOP = 3, XPT_LINESTRIP = 4, XPT_TRIANGLELIST = 5,
    XPT_TRIANGLESTRIP = 6, XPT_TRIANGLEFAN = 7, XPT_QUADLIST = 8, XPT_QUADSTRIP = 9, XPT_POLYGON = 10,
};

} // namespace xd3d