// The HLE state layer, checked twice: once over the table, once over the bytes.
//
// The table check exists because a table row is a string and a function pointer, and nothing about that
// combination fails loudly. A spec with a typo, an implementation that was never written, an address that
// belongs to another build -- each of those is a null thunk and a game that runs the original console code,
// which for a function that pokes NV2A registers is a corrupt frame rather than a message.
//
// The state check is the part that matters more. Every function here has exactly one observable effect:
// bytes move inside the device object, or inside one of the arrays the game writes in place. So each test
// writes through the thunk and then reads the bytes at the offset the console used, exactly as game code
// would. A shadow copy the game cannot see is not state; these checks are what keeps that distinction real.
#include "d3d/device_posix.h"

#include "d3d/hle_table_posix.h"

#include "d3d/thunk_posix.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

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
    char buf[220];
    snprintf(buf, sizeof buf, "%s (got %llu, want %llu)", what.c_str(), (unsigned long long)got,
             (unsigned long long)want);
    check(got == want, buf);
}

// The same caller the thunk suite uses: push `n` words from `stack`, load eax/ecx/edx from `regs`, call
// through `thunk`, and report the registers and the stack afterwards. Six words are enough for every row in
// the table today; the longest spec is three.
enum { IO_STACK = 0, IO_REGS = 32, IO_OUT = 44 };
struct CallIo {
    uint32_t stack[8];
    uint32_t regs[3];
    uint32_t out[5];
};

extern "C" void call_hle(void *thunk, int count, CallIo *io);
__asm__(".text\n"
        ".globl call_hle\n"
        ".type call_hle, @function\n"
        "call_hle:\n"
        "    pushl %ebp\n"
        "    movl %esp, %ebp\n"
        "    pushl %ebx\n"
        "    pushl %esi\n"
        "    pushl %edi\n"
        "    movl 8(%ebp), %ebx\n"
        "    movl 16(%ebp), %edi\n"
        "    movl %esp, %esi\n"
        "    movl %esi, 44(%edi)\n"
        "    movl 12(%ebp), %edx\n"
        "    testl %edx, %edx\n"
        "    je 2f\n"
        "    shll $2, %edx\n"
        "    subl %edx, %esp\n"
        "    movl %esp, %ecx\n"
        "    xorl %eax, %eax\n"
        "1:\n"
        "    movl (%edi,%eax,4), %esi\n"
        "    movl %esi, (%ecx,%eax,4)\n"
        "    incl %eax\n"
        "    cmpl 12(%ebp), %eax\n"
        "    jb 1b\n"
        "2:\n"
        "    movl 32(%edi), %eax\n"
        "    movl 36(%edi), %ecx\n"
        "    movl 40(%edi), %edx\n"
        "    call *%ebx\n"
        "    movl %esp, %esi\n"
        "    movl %esi, 48(%edi)\n"
        "    movl %eax, %esi\n"
        "    movl %esi, 52(%edi)\n"
        "    movl %ecx, %esi\n"
        "    movl %esi, 56(%edi)\n"
        "    movl %edx, %esi\n"
        "    movl %esi, 60(%edi)\n"
        "    leal -12(%ebp), %esp\n"
        "    popl %edi\n"
        "    popl %esi\n"
        "    popl %ebx\n"
        "    popl %ebp\n"
        "    ret\n"
        ".size call_hle, .-call_hle\n");

static CallIo g_io;
static std::vector<void *> g_thunks;

static void reset_io()
{
    memset(&g_io, 0, sizeof g_io);
    for (int i = 0; i < 8; i++) g_io.stack[i] = 0;
    g_io.regs[0] = 0xAAAAAAAAu;
    g_io.regs[1] = 0xBBBBBBBBu;
    g_io.regs[2] = 0xCCCCCCCCu;
}

__attribute__((noinline)) static void call(int row, int count)
{
    call_hle(g_thunks[row], count, &g_io);
}

// The thunk for a row by name, so the tests below read as "SetRenderTarget" rather than as "row 1".
static int row_index(const char *name)
{
    size_t count = 0;
    const Hle *rows = device_rows(&count);
    for (size_t i = 0; i < count; i++)
        if (rows[i].name && strcmp(rows[i].name, name) == 0) return (int)i;
    return -1;
}

// ---- the table ----

static void test_the_table()
{
    printf("\nevery row of the table\n");

    size_t count = 0;
    const Hle *rows = device_rows(&count);
    check(count > 0, "the device table is not empty");
    std::string arena_error;
    check(hooks_prepare_arena(0x1000, (uint8_t *)(uintptr_t)0x00200000, arena_error),
          "an arena to build into" + (arena_error.empty() ? std::string() : ": " + arena_error));

    const TableReport report = build_table(rows, count, &g_thunks);
    check_eq(report.rows, count, "the report counted every row");
    check_eq(report.built, count, "and every one of them built");
    if (!report.refused.empty()) {
        std::string why = report.refused.size() == 1 ? "one was refused: " : "some were refused: ";
        for (const TableReport::Refusal &r : report.refused) {
            char buf[160];
            snprintf(buf, sizeof buf, "%s@0x%08x (%s)", r.name.c_str(), r.address, r.reason.c_str());
            why += buf;
            why += "; ";
        }
        check(false, why);
    }
    check(g_thunks.size() == count && g_thunks[0] != nullptr, "each row has a thunk to call");

    // A row is only usable if the game can actually reach its thunk from the entry it patches: both live in
    // the same arena, so a thunk far from the image would be a rel32 the patch cannot express. The arena is
    // the same one the hooks use, which is the whole reason it is mapped beside the image.
    const uint32_t base = hooks_arena_base();
    size_t outside = 0;
    for (size_t i = 0; i < g_thunks.size(); i++) {
        const uint32_t a = (uint32_t)(uintptr_t)g_thunks[i];
        if (a < base || a >= base + 0x100000) outside++;
    }
    check_eq(outside, 0, "every thunk is inside the arena the entries jump into");

    // The addresses are per-build facts: a row whose address is not in the D3D section is a row that was
    // copied from another build, and it would silently patch the wrong function.
    size_t out_of_section = 0;
    for (size_t i = 0; i < count; i++)
        if (rows[i].addr < 0x00260000 || rows[i].addr > 0x00280000) out_of_section++;
    check_eq(out_of_section, 0, "every address is in the image's D3D section");

    // No two rows may claim the same entry: two thunks at one address means whichever installed last wins,
    // and the other's implementation never runs.
    size_t clashes = 0;
    for (size_t i = 0; i < count; i++)
        for (size_t j = i + 1; j < count; j++)
            if (rows[i].addr == rows[j].addr) clashes++;
    check_eq(clashes, 0, "no two rows claim the same entry point");

    // The tables that are not here yet must say so, rather than being absent from a walk over "the tables".
    size_t n = 1;
    check(resource_rows(&n) == nullptr && n == 0, "the resource table is empty and says so");
    check(draw_rows(&n) == nullptr && n == 0, "and so is the draw table");
}

// ---- what the state functions actually do ----

static void test_render_targets()
{
    printf("\nrender targets\n");

    static Surface rt, z;
    memset(&rt, 0, sizeof rt);
    memset(&z, 0, sizeof z);
    const int i_set = row_index("D3DDevice_SetRenderTarget");
    const int i_get = row_index("D3DDevice_GetRenderTarget2");
    const int i_getz = row_index("D3DDevice_GetDepthStencilSurface2");
    check(i_set >= 0 && i_get >= 0 && i_getz >= 0, "the three render target rows are present");

    reset_io();
    g_io.stack[0] = (uint32_t)(uintptr_t)&rt;
    g_io.stack[1] = (uint32_t)(uintptr_t)&z;
    call(i_set, 2);
    check_eq(g_io.out[2], D3D_OK, "SetRenderTarget reports D3D_OK");
    check_eq(g_io.out[1], g_io.out[0], "and the caller's stack is balanced");

    // The bytes the game reads: the render target and the z buffer are pointers inside the device object.
    check_eq(dev_field<uint32_t>(DEV_RENDERTARGET), (uint32_t)(uintptr_t)&rt, "the object holds the colour target");
    check_eq(dev_field<uint32_t>(DEV_ZBUFFER), (uint32_t)(uintptr_t)&z, "and the z buffer");
    check(state().render_target == &rt, "the shadow agrees about the colour target");
    check(state().depth_stencil == &z, "and about the z buffer");

    // GetRenderTarget2 hands back the same surface and adds a reference, because on this console a Get*
    // that returns a resource counts. The game Releases it later; a Get that forgot the increment would
    // make the game release a reference nobody took.
    const uint32_t before = rt.Common;
    reset_io();
    call(i_get, 0);
    check_eq(g_io.out[2], (uint32_t)(uintptr_t)&rt, "GetRenderTarget2 returns it");
    check_eq(rt.Common, before + 1, "and adds a reference");
    check_eq(g_io.out[1], g_io.out[0], "with nothing pushed and nothing dropped");

    reset_io();
    call(i_getz, 0);
    check_eq(g_io.out[2], (uint32_t)(uintptr_t)&z, "GetDepthStencilSurface2 returns the z buffer");
    check_eq(z.Common, 1, "with its reference");

    // A null colour target is not an unbind: the console keeps the previous one and only the z buffer
    // changes. A HLE that treated null as "nothing is bound" would make the game's next draw go nowhere.
    static Surface rt2;
    memset(&rt2, 0, sizeof rt2);
    reset_io();
    g_io.stack[0] = 0;
    g_io.stack[1] = 0;
    call(i_set, 2);
    check(state().render_target == &rt, "a null colour target keeps the previous one");
    check(state().depth_stencil == nullptr, "while a null z buffer clears the z buffer");
}

static void test_transforms()
{
    printf("\ntransforms and viewport\n");

    const int i = row_index("D3DDevice_SetTransform");
    const int i_vp = row_index("D3DDevice_SetViewport");
    check(i >= 0 && i_vp >= 0, "the SetTransform and SetViewport rows are present");

    static Matrix m;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) m.m[r][c] = (float)(r * 4 + c) + 0.5f;

    reset_io();
    g_io.stack[0] = XTS_VIEW;
    g_io.stack[1] = (uint32_t)(uintptr_t)&m;
    call(i, 2);
    check(state().transforms[XTS_VIEW].m[3][3] == m.m[3][3], "the shadow kept the matrix");

    // And the bytes: the console's ten matrices sit at 0x0C60, one every 0x40. Game code reads them
    // straight out of the device object, so a correct shadow and a stale object are both bugs.
    const uint8_t *bytes = device_object()->bytes + DEV_TRANSFORMS + XTS_VIEW * sizeof(Matrix);
    check(memcmp(bytes, &m, sizeof m) == 0, "the object holds the same matrix at DEV_TRANSFORMS + view * 0x40");

    // One past the last slot is not clamped: the console drops it, and clamping would write over the
    // viewport that follows in the object. The viewport is poisoned first, so "untouched" means something.
    static Viewport vp = {11, 22, 640, 480, 0.0f, 1.0f};
    for (int w = 0; w < 6; w++) dev_field<uint32_t>(DEV_VIEWPORT + 4 * w) = 0x0B0B0B0Bu;
    reset_io();
    g_io.stack[0] = (uint32_t)(uintptr_t)&vp;
    call(i_vp, 1);
    check(memcmp(device_object()->bytes + DEV_VIEWPORT, &vp, sizeof vp) == 0, "SetViewport put it there");

    static Matrix m2 = {};
    for (int w = 0; w < 6; w++) dev_field<uint32_t>(DEV_VIEWPORT + 4 * w) = 0x0B0B0B0Bu;
    reset_io();
    g_io.stack[0] = XTS_COUNT; // the tenth slot plus one: texture7, which has nowhere to go
    g_io.stack[1] = (uint32_t)(uintptr_t)&m2;
    call(i, 2);
    check(memcmp(device_object()->bytes + DEV_TRANSFORMS + XTS_COUNT * sizeof(Matrix), &m2, sizeof m2) != 0,
          "a transform past the tenth is ignored, not written past the end");
    check_eq(dev_field<uint32_t>(DEV_VIEWPORT), 0x0B0B0B0Bu, "and the viewport is untouched");
}

static void test_viewport_and_offset()
{
    printf("\nviewport and screen space offset\n");

    const int i_vp = row_index("D3DDevice_SetViewport");
    const int i_off = row_index("D3DDevice_SetScreenSpaceOffset");
    check(i_vp >= 0 && i_off >= 0, "both rows are present");

    static Viewport vp = {5, 6, 320, 240, 0.25f, 0.75f};
    reset_io();
    g_io.stack[0] = (uint32_t)(uintptr_t)&vp;
    call(i_vp, 1);
    check(memcmp(device_object()->bytes + DEV_VIEWPORT, &vp, sizeof vp) == 0, "the viewport reached the object");
    check(state().viewport.Width == 320 && state().viewport.MinZ == 0.25f, "and the shadow kept it");

    // Two floats, not two integers: the fields at 0xEF8/0xEFC hold an offset the screen transform is built
    // from, and this game uses fractional offsets. Calling through the thunk is what proves the ABI: a
    // signature of two integers would push the wrong bytes and the test would see 0.
    reset_io();
    const float fx = 0.5f, fy = -0.25f;
    uint32_t bits_x, bits_y;
    memcpy(&bits_x, &fx, 4);
    memcpy(&bits_y, &fy, 4);
    g_io.stack[0] = bits_x;
    g_io.stack[1] = bits_y;
    call(i_off, 2);
    check_eq(dev_field<uint32_t>(DEV_SCREENOFFSET_X), bits_x, "the x offset is the float the game passed");
    check_eq(dev_field<uint32_t>(DEV_SCREENOFFSET_Y), bits_y, "and so is y");
    check(state().screen_offset[0] == 0.5f && state().screen_offset[1] == -0.25f, "the shadow holds them as floats");
}

static void test_textures_and_shader()
{
    printf("\ntextures, vertex shader, render state\n");

    const int i_tex = row_index("D3DDevice_SetTexture");
    const int i_vs = row_index("D3DDevice_SetVertexShader");
    const int i_sel = row_index("D3D_SelectVertexShader");
    const int i_rs = row_index("D3DDevice_SetRenderStateNotInline");
    const int i_rsi = row_index("D3DDevice_SetRenderState_Simple");
    check(i_tex >= 0 && i_vs >= 0 && i_sel >= 0 && i_rs >= 0 && i_rsi >= 0, "all five rows are present");

    // Four texture stages, four pointers at 0x0F98.
    static Resource t3;
    memset(&t3, 0, sizeof t3);
    reset_io();
    g_io.stack[0] = 3;
    g_io.stack[1] = (uint32_t)(uintptr_t)&t3;
    call(i_tex, 2);
    check_eq(dev_field<uint32_t>(DEV_TEXTURES + 3 * 4), (uint32_t)(uintptr_t)&t3, "stage 3 is in the object");
    check(state().textures[3] == &t3, "and in the shadow");

    reset_io();
    g_io.stack[0] = TEXTURESTAGE_COUNT;
    g_io.stack[1] = 0;
    call(i_tex, 2);
    check(state().textures[TEXTURESTAGE_COUNT - 1] == &t3, "a stage past the fourth is ignored, not clamped");

    // The vertex shader is a handle the game chose, not an index into anything of ours. The inline variant
    // masks bit 4 off: that bit is part of the tag the game carries, not part of the handle.
    reset_io();
    // 0x1200, not 0x1234 or 0x1230: both of those have bit 4 already set, so masking them would clear a
    // bit the caller meant and the check could not tell "masked the tag" from "lost a real bit". Two
    // attempts at a cleverer handle ended here.
    g_io.stack[0] = 0x1200;
    call(i_vs, 1);
    check_eq(state().vertex_shader, 0x1200, "SetVertexShader stores the handle as given");
    reset_io();
    g_io.regs[0] = 0x1200 | 0x10u;
    call(i_sel, 0);
    check_eq(state().vertex_shader, 0x1200, "and the inline variant masks the tag bit off");
    check(state().vertex_shader != (0x1200u | 0x10u), "which is a different value from the one it was given");

    // Render state: the array is the game's own, so the value has to land there and stay there.
    reset_io();
    g_io.stack[0] = XRS_ZFUNC;
    g_io.stack[1] = 7;
    call(i_rs, 2);
    check_eq(render_state()[XRS_ZFUNC], 7, "the value landed in the game's render state array");

    reset_io();
    g_io.stack[0] = XRS_MAX; // one past the last state: the console drops it
    g_io.stack[1] = 99;
    call(i_rs, 2);
    check_eq(render_state()[XRS_ZFUNC], 7, "and a state past the last is ignored rather than clamped");

    // The inline path is called after the game's own code has already written the array; storing again
    // would undo the store the game made, which is why this row does nothing but flush. Its spec is the
    // LTCG shape -- the method in ecx and the value in edx -- so ecx and edx are both arguments here, and
    // "the caller's registers came back" means they came back holding what the caller put in them.
    render_state()[XRS_ALPHABLENDENABLE] = 1;
    reset_io();
    g_io.regs[0] = 0x50; // eax is not in the spec: the thunk leaves it alone
    g_io.regs[1] = 0x1D; // method
    g_io.regs[2] = 42;  // value
    call(i_rsi, 0);
    check_eq(render_state()[XRS_ALPHABLENDENABLE], 1, "the inline path leaves the array the game wrote");
    check_eq(g_io.out[1], g_io.out[0], "with nothing pushed on the stack, nothing dropped");
    check_eq(g_io.out[3], 0x1D, "ecx comes back holding what the caller put in it, which was the method");
    check_eq(g_io.out[4], 42, "and edx the value");
    check_eq(g_io.out[2], 0x50, "while eax, being no one's argument, is untouched");
}

static void test_display_mode()
{
    printf("\ndisplay mode\n");

    const int i = row_index("D3DDevice_GetDisplayMode");
    check(i >= 0, "the GetDisplayMode row is present");

    DisplayMode mode;
    memset(&mode, 0, sizeof mode);
    state().display_width = 1280;
    state().display_height = 720;
    reset_io();
    g_io.stack[0] = (uint32_t)(uintptr_t)&mode;
    call(i, 1);
    check_eq(g_io.out[2], D3D_OK, "it reports D3D_OK");
    check_eq(mode.Width, 1280, "and reports the window's width, not the TV mode's");
    check_eq(mode.Height, 720, "and its height");
    check_eq(mode.RefreshRate, 60, "keeping the refresh rate of the mode it matched");
    check_eq(mode.Format, XFMT_LIN_A8R8G8B8, "and the format the driver insists on (0x12)");
    check_eq(mode.Flags, 0x120, "with the flags the driver insists on as well");

    // Null in, error out: the console does not write through a null pointer and does not pretend it did.
    reset_io();
    call(i, 1);
    check_eq(g_io.out[2], D3D_E_POINTER, "a null argument is reported, not ignored");
}

int main()
{
    printf("BLACK loader -- the HLE state layer\n");

    test_the_table();
    test_render_targets();
    test_transforms();
    test_viewport_and_offset();
    test_textures_and_shader();
    test_display_mode();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}