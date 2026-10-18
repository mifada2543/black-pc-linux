// The device rows that touch a graphics device, checked against the bytes they leave behind.
//
// Every other device test in this project reads the device object or the state arrays and asks "did the
// console's bytes move". These rows have no bytes to read: a Clear's whole observable effect is the contents
// of a render target, so the only honest check is to clear one, read it back, and compare. That is possible
// here because llvmpipe rasterises -- badly, and in the A0 gate with zero fragments -- but a clear does not
// need rasterisation, and a clear that came out wrong is a clear that came out wrong.
#include "gfx/gfx_vulkan.h"

#include "d3d/device_posix.h"
#include "d3d/hle_table_posix.h"
#include "d3d/resource_posix.h"

#include <stdio.h>
#include <string.h>

#include <vector>

namespace xd3d {
XD3D_STDCALL uint32_t D3DDevice_Clear(uint32_t Count, const D3DRect *pRects, uint32_t Flags, uint32_t Color, float Z,
                                      uint32_t Stencil);
XD3D_STDCALL uint32_t D3DDevice_SetRenderTarget(Surface *pRenderTarget, Surface *pNewZStencil);
XD3D_STDCALL Surface *D3D_CreateSurface2(uint32_t Width, uint32_t Height, uint32_t Usage, uint32_t Format);
} // namespace xd3d

using namespace xd3d;

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

static const uint32_t kW = 64, kH = 64;

static gfx::Image g_colour, g_depth;

// Read one pixel and hand it back as the console names its channels: argb[0] alpha, [1] red, [2] green,
// [3] blue.
//
// read_color returns the image's own byte order, which for these targets is B, G, R, A, and that is
// deliberate -- the game asked for A8R8G8B8 and reads those same bytes back through LockRect. So the swap
// belongs here, in the test, where the question "which channel is which" is explicit. Putting it in read_color
// would be a change of behaviour for every caller, to fix a mistake that was only ever in this test.
static bool pixel(uint32_t x, uint32_t y, uint8_t *argb)
{
    std::vector<uint8_t> px(kW * kH * 4);
    if (!gfx::read_color(&g_colour, px.data())) return false;
    const size_t o = ((size_t)y * kW + x) * 4;
    argb[0] = px[o + 3]; // A
    argb[1] = px[o + 2]; // R
    argb[2] = px[o + 1]; // G
    argb[3] = px[o + 0]; // B
    return true;
}

static void test_a_whole_target_clear()
{
    printf("\nclearing the whole target\n");

    // 0xFF204060: alpha 0xFF, red 0x20, green 0x40, blue 0x60. The byte order is the console's A8R8G8B8, and
    // getting it backwards swaps red and blue on every clear -- which reads as a wrong background rather
    // than as an error, so it is the first thing worth checking.
    const uint32_t hr = D3DDevice_Clear(0, nullptr, XCLEAR_TARGET, 0xFF204060u, 1.0f, 0);
    check_eq(hr, D3D_OK, "a whole-target colour clear succeeds");
    check(gfx::end_frame(), "and the frame submits");
    gfx::begin_frame();

    uint8_t p[4] = {0, 0, 0, 0};
    check(pixel(32, 32, p), "the pixels come back");
    check_eq(p[1], 0x20, "red landed in red");
    check_eq(p[2], 0x40, "green in green");
    check_eq(p[3], 0x60, "and blue in blue");
    check_eq(p[0], 0xFF, "with the alpha the game asked for");
}

static void test_the_console_flag_mask()
{
    printf("\nthe console's flag mask, not D3D9's\n");

    // XCLEAR_TARGET is 0xF0, a colour mask and not a single bit. A translation that mapped it onto "the
    // colour bit" would clear nothing and say nothing, which is why this is its own check rather than part of
    // the one above: there, a wrong flag constant would look exactly like a clear that worked.
    D3DDevice_Clear(0, nullptr, XCLEAR_TARGET, 0xFF112233u, 1.0f, 0);
    gfx::end_frame();
    gfx::begin_frame();
    uint8_t p[4];
    pixel(32, 32, p);
    const bool cleared = p[1] == 0x11 && p[2] == 0x22 && p[3] == 0x33;
    check(cleared, "XCLEAR_TARGET on its own -- 0xF0, not a bit -- clears the colour");
}

static void test_a_rectangle_clear()
{
    printf("\nclearing a rectangle\n");

    D3DDevice_Clear(0, nullptr, XCLEAR_TARGET, 0xFFFFFFFFu, 1.0f, 0);
    gfx::end_frame();
    gfx::begin_frame();

    D3DRect r;
    r.x1 = 16;
    r.y1 = 8;
    r.x2 = 48;
    r.y2 = 40;
    D3DDevice_Clear(1, &r, XCLEAR_TARGET, 0xFF000000u, 1.0f, 0);
    check(gfx::end_frame(), "a rectangle clear submits");
    gfx::begin_frame();

    uint8_t inside[4], outside[4];
    pixel(32, 24, inside); // squarely inside
    pixel(4, 4, outside);   // squarely outside
    const bool is_black = inside[1] == 0 && inside[2] == 0 && inside[3] == 0;
    const bool is_white = outside[1] == 0xFF && outside[2] == 0xFF && outside[3] == 0xFF;
    check(is_black, "the rectangle is the colour that was cleared into it");
    check(is_white, "and the rest of the target kept what it had");

    // The corners are the half-open boundary: (16,8) is inside, (48,40) is not. An off-by-one here shows up
    // as a one-pixel frame around every cleared rectangle, which is easy to mistake for a rendering bug.
    uint8_t corner[4];
    pixel(47, 39, corner);
    check(corner[1] == 0, "the rectangle ends where it says, not one pixel later");
}

static void test_a_rectangle_that_is_empty()
{
    printf("\nan empty rectangle\n");

    D3DDevice_Clear(0, nullptr, XCLEAR_TARGET, 0xFF804020u, 1.0f, 0);
    gfx::end_frame();
    gfx::begin_frame();

    D3DRect r;
    r.x1 = 20;
    r.y1 = 20;
    r.x2 = 20; // zero width
    r.y2 = 30;
    check_eq(D3DDevice_Clear(1, &r, XCLEAR_TARGET, 0xFF000000u, 1.0f, 0), D3D_OK, "a zero-width rectangle is not an error");
    check(gfx::end_frame(), "and submits");
    gfx::begin_frame();
    uint8_t p[4];
    pixel(20, 25, p);
    check(p[1] == 0x80 && p[2] == 0x40 && p[3] == 0x20, "and cleared nothing, as the console did");
}

static void test_depth()
{
    printf("\nclearing depth\n");

    check_eq(D3DDevice_Clear(0, nullptr, XCLEAR_ZBUFFER, 0, 0.5f, 0), D3D_OK, "a depth clear succeeds");
    check(gfx::end_frame(), "and submits");
    gfx::begin_frame();
    // The depth value itself is not readable through this path; what is checkable is that a depth clear
    // does not disturb the colour, which is what a wrong aspect mask would do.
    uint8_t p[4];
    D3DDevice_Clear(0, nullptr, XCLEAR_TARGET, 0xFF336699u, 1.0f, 0);
    check(gfx::end_frame(), "and a colour clear after it still works");
    gfx::begin_frame();
    pixel(10, 10, p);
    check(p[1] == 0x33 && p[2] == 0x66 && p[3] == 0x99, "which means the depth clear left the colour alone");
}

static void test_nothing_asked_for()
{
    printf("\nasking for nothing\n");

    D3DDevice_Clear(0, nullptr, XCLEAR_TARGET, 0xFFABCDEFu, 1.0f, 0);
    gfx::end_frame();
    gfx::begin_frame();

    // Flags of zero is legal: a game that ORs several flags and clears none of them is not broken, and
    // refusing would be. What it must not do is clear anyway.
    check_eq(D3DDevice_Clear(0, nullptr, 0, 0x00000000u, 0.0f, 0), D3D_OK, "a clear with no flags is not an error");
    check(gfx::end_frame(), "and submits");
    gfx::begin_frame();
    uint8_t p[4];
    pixel(10, 10, p);
    check(p[1] == 0xAB && p[2] == 0xCD && p[3] == 0xEF, "and cleared nothing");
}

static void test_the_row_is_in_the_table()
{
    printf("\nthe row\n");

    size_t n = 0;
    const Hle *rows = device_rows(&n);
    int found = -1;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(rows[i].name, "D3DDevice_Clear")) found = (int)i;
    check(found >= 0, "D3DDevice_Clear is in the device table");
    if (found < 0) return;
    check_eq(rows[found].addr, 0x00268880u, "at the address the invoice recorded");
    check(rows[found].returns, "and it returns a value -- the game needs to know it failed");

    size_t clashes = 0;
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (rows[i].addr == rows[j].addr) clashes++;
    check_eq(clashes, 0, "and no two rows claim that entry");

    TableReport report = build_table(rows, n);
    check_eq(report.built, n, "every row in the table still builds a thunk");
    for (size_t i = 0; i < report.refused.size(); i++)
        printf("  ....  %s: %s\n", report.refused[i].name.c_str(), report.refused[i].reason.c_str());
}

static void test_a_device_owns_no_surface_yet()
{
    printf("\nwhat Clear does without a game surface\n");

    // The D3D rows keep a Surface* in the state; a Clear goes straight to the graphics device and does not
    // look at it, because the console's Clear did not either. So it works with nothing set up, which is what
    // makes it testable before the game image exists.
    check_eq(D3DDevice_Clear(0, nullptr, XCLEAR_TARGET, 0xFF445566u, 1.0f, 0), D3D_OK,
             "a clear does not need the state shadow to be filled in");
    check(gfx::end_frame(), "and submits");
    gfx::begin_frame();
    uint8_t p[4];
    pixel(5, 5, p);
    check(p[1] == 0x44 && p[2] == 0x55 && p[3] == 0x66, "with the colour the caller gave");
}

int main()
{
    printf("BLACK loader -- the device rows that touch the GPU\n");
    if (!gfx::init()) {
        printf("  --     no Vulkan on this machine: %s (skipped)\n", gfx::last_error());
        printf("\n%d checks, 0 failures\n", g_checks);
        return 77;
    }
    if (!gfx::create_color_target(&g_colour, kW, kH, VK_FORMAT_B8G8R8A8_UNORM) ||
        !gfx::create_depth_target(&g_depth, kW, kH, VK_FORMAT_D16_UNORM)) {
        printf("  --     no render targets: %s (skipped)\n", gfx::last_error());
        gfx::shutdown();
        return 77;
    }
    gfx::set_render_target(&g_colour, &g_depth);

    test_a_whole_target_clear();
    test_the_console_flag_mask();
    test_a_rectangle_clear();
    test_a_rectangle_that_is_empty();
    test_depth();
    test_nothing_asked_for();
    test_a_device_owns_no_surface_yet();
    test_the_row_is_in_the_table();

    gfx::set_render_target(nullptr, nullptr);
    gfx::shutdown();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}