// The parts of the Xbox D3D8 ABI that need code rather than layout: the format table, and the shadow state
// the HLE keeps instead of a host device.
//
// Ported from loader/windows/src/d3d/formats.cpp and state.h. One thing changed on purpose: the Windows
// port filled its format table lazily on first use (`static bool init`), which is a data race the moment two
// threads ask for a format at the same time -- and the texture layer is called from the game's thread while
// the loader's own logging can run on another. Here the table is constexpr, so it is built before main()
// and cannot race. The values are the same table, measured on the real image.
#include "d3d/xd3d8_posix.h"

namespace xd3d {
namespace {

// Index 0..0xFF by XFormat value; "?" for anything not in the console's value space, which is what the
// Windows port returned too. A game that passes a format we have never heard of is a fact worth logging, not
// a reason to guess at a size.
constexpr FormatInfo kUnknown = {32, false, false, "?"};

struct FormatTable {
    FormatInfo e[0x100];
};

constexpr FormatTable make_format_table()
{
    FormatTable t{};
    for (auto &f : t.e) f = kUnknown;
    auto set = [](FormatInfo &slot, uint8_t bpp, bool lin, bool comp, const char *n) {
        slot = FormatInfo{bpp, lin, comp, n};
    };
    set(t.e[XFMT_L8], 8, false, false, "L8");
    set(t.e[XFMT_AL8], 8, false, false, "AL8");
    set(t.e[XFMT_A1R5G5B5], 16, false, false, "A1R5G5B5");
    set(t.e[XFMT_X1R5G5B5], 16, false, false, "X1R5G5B5");
    set(t.e[XFMT_A4R4G4B4], 16, false, false, "A4R4G4B4");
    set(t.e[XFMT_R5G6B5], 16, false, false, "R5G6B5");
    set(t.e[XFMT_A8R8G8B8], 32, false, false, "A8R8G8B8");
    set(t.e[XFMT_X8R8G8B8], 32, false, false, "X8R8G8B8");
    set(t.e[XFMT_P8], 8, false, false, "P8");
    set(t.e[XFMT_DXT1], 4, false, true, "DXT1"); // 4 bits per pixel: the only compressed format that is
    set(t.e[XFMT_DXT3], 8, false, true, "DXT3"); // 8, and the difference matters to the upload path
    set(t.e[XFMT_DXT5], 8, false, true, "DXT5");
    set(t.e[XFMT_LIN_A1R5G5B5], 16, true, false, "LIN_A1R5G5B5");
    set(t.e[XFMT_LIN_R5G6B5], 16, true, false, "LIN_R5G6B5");
    set(t.e[XFMT_LIN_A8R8G8B8], 32, true, false, "LIN_A8R8G8B8");
    set(t.e[XFMT_LIN_L8], 8, true, false, "LIN_L8");
    set(t.e[XFMT_LIN_R8B8], 16, true, false, "LIN_R8B8");
    set(t.e[XFMT_LIN_G8B8], 16, true, false, "LIN_G8B8");
    set(t.e[XFMT_A8], 8, false, false, "A8");
    set(t.e[XFMT_A8L8], 16, false, false, "A8L8");
    set(t.e[XFMT_LIN_AL8], 8, true, false, "LIN_AL8");
    set(t.e[XFMT_LIN_X1R5G5B5], 16, true, false, "LIN_X1R5G5B5");
    set(t.e[XFMT_LIN_A4R4G4B4], 16, true, false, "LIN_A4R4G4B4");
    set(t.e[XFMT_LIN_X8R8G8B8], 32, true, false, "LIN_X8R8G8B8");
    set(t.e[XFMT_LIN_A8], 8, true, false, "LIN_A8");
    set(t.e[XFMT_LIN_A8L8], 16, true, false, "LIN_A8L8");
    set(t.e[XFMT_YUY2], 16, true, false, "YUY2");
    set(t.e[XFMT_UYVY], 16, true, false, "UYVY");
    set(t.e[XFMT_L6V5U5], 16, false, false, "L6V5U5");
    set(t.e[XFMT_V8U8], 16, false, false, "V8U8");
    set(t.e[XFMT_R8B8], 16, false, false, "R8B8");
    set(t.e[XFMT_D24S8], 32, false, false, "D24S8"); // 32, not 24: the depth buffer is read back as one
    set(t.e[XFMT_F24S8], 32, false, false, "F24S8"); // word, which is why the Vulkan side uses D32_SFLOAT
    set(t.e[XFMT_D16], 16, false, false, "D16");
    set(t.e[XFMT_F16], 16, false, false, "F16");
    set(t.e[XFMT_LIN_D24S8], 32, true, false, "LIN_D24S8");
    set(t.e[XFMT_LIN_F24S8], 32, true, false, "LIN_F24S8");
    set(t.e[XFMT_LIN_D16], 16, true, false, "LIN_D16");
    set(t.e[XFMT_LIN_F16], 16, true, false, "LIN_F16");
    set(t.e[XFMT_L16], 16, false, false, "L16");
    set(t.e[XFMT_V16U16], 32, false, false, "V16U16");
    set(t.e[XFMT_LIN_L16], 16, true, false, "LIN_L16");
    set(t.e[XFMT_LIN_V16U16], 32, true, false, "LIN_V16U16");
    set(t.e[XFMT_LIN_L6V5U5], 16, true, false, "LIN_L6V5U5");
    set(t.e[XFMT_R6G5B5], 16, false, false, "R6G5B5");
    set(t.e[XFMT_G8B8], 16, false, false, "G8B8");
    set(t.e[XFMT_A8B8G8R8], 32, false, false, "A8B8G8R8");
    set(t.e[XFMT_B8G8R8A8], 32, false, false, "B8G8R8A8");
    set(t.e[XFMT_R8G8B8A8], 32, false, false, "R8G8B8A8");
    set(t.e[XFMT_LIN_R6G5B5], 16, true, false, "LIN_R6G5B5");
    set(t.e[XFMT_LIN_A8B8G8R8], 32, true, false, "LIN_A8B8G8R8");
    set(t.e[XFMT_LIN_B8G8R8A8], 32, true, false, "LIN_B8G8R8A8");
    set(t.e[XFMT_LIN_R8G8B8A8], 32, true, false, "LIN_R8G8B8A8");
    return t;
}

constexpr FormatTable kFormats = make_format_table();

} // namespace

const FormatInfo &format_info(uint32_t xfmt)
{
    return xfmt < 0x100 ? kFormats.e[xfmt] : kUnknown;
}

} // namespace xd3d