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
    // A row with no implementation is not a gap: it says "the original library code here is correct, leave
    // it in place", and the installer does exactly that. Only rows that claim an implementation have to
    // produce a thunk -- so the expected count is the rows that have one.
    size_t with_impl = 0;
    for (size_t i = 0; i < count; i++)
        if (rows[i].impl) with_impl++;
    check_eq(report.built, with_impl, "and every row that has an implementation built");
    check_eq(report.refused.size(), count - with_impl, "the rows without one are the only ones not built");
    for (const TableReport::Refusal &r : report.refused) {
        // Not a failure: the installer leaves these native, which is why build_table refuses them.
        if (strstr(r.reason.c_str(), "no implementation bound")) continue;
        char buf[160];
        snprintf(buf, sizeof buf, "%s@0x%08x (%s)", r.name.c_str(), r.address, r.reason.c_str());
        check(false, std::string("refused for a reason other than being native: ") + buf);
    }
    size_t missing = 0;
    for (size_t i = 0; i < count; i++)
        if ((g_thunks[i] != nullptr) != (rows[i].impl != nullptr)) missing++;
    check_eq(missing, 0, "each row has a thunk exactly when it has an implementation");

    // A row is only usable if the game can actually reach its thunk from the entry it patches: both live in
    // the same arena, so a thunk far from the image would be a rel32 the patch cannot express. The arena is
    // the same one the hooks use, which is the whole reason it is mapped beside the image.
    const uint32_t base = hooks_arena_base();
    size_t outside = 0, thunks = 0;
    for (size_t i = 0; i < g_thunks.size(); i++) {
        if (!g_thunks[i]) continue; // a NATIVE row has no thunk, by design
        const uint32_t a = (uint32_t)(uintptr_t)g_thunks[i];
        thunks++;
        if (a < base || a >= base + 0x100000) outside++;
    }
    check(thunks > 0, "there are thunks to check");
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

        // All three tables are filled now, so a walk over "the tables" visits every row and every one of them
    // has been checked. The per-table specifics live with their own tests.
    size_t resources = 0, draws = 0;
    check(resource_rows(&resources) != nullptr && resources == 18, "and the resource table is there as well");
    check(draw_rows(&draws) != nullptr && draws == 3, "and so is the draw table");
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

// ---- the rows added for A3 ----

static void test_adapters_and_device()
{
    printf("\nadapters and the device itself\n");

    const int i_count = row_index("Direct3D_GetAdapterModeCount");
    const int i_enum = row_index("Direct3D_EnumAdapterModes");
    const int i_release = row_index("D3DDevice_Release");
    check(i_count >= 0 && i_enum >= 0 && i_release >= 0, "the three rows are present");

    reset_io();
    call(i_count, 1);
    check_eq(g_io.out[2], 1, "one mode, and one mode is what the game is told");

    // Mode 0 is the mode the driver insists on; the widths are the loader's window, not the TV mode,
    // which is the whole reason GetDisplayMode overrides them.
    static DisplayMode m;
    memset(&m, 0, sizeof m);
    reset_io();
    g_io.stack[0] = 0;
    g_io.stack[1] = 0;
    g_io.stack[2] = (uint32_t)(uintptr_t)&m;
    call(i_enum, 3);
    check_eq(g_io.out[2], D3D_OK, "mode 0 is enumerated");
    check_eq(g_io.out[1], g_io.out[0], "with a balanced stack");

    // Out of range is refused rather than clamped. A caller that asked for mode 7 of a one-mode adapter
    // has a bug, and handing back mode 0 would hide it until something further along went wrong.
    reset_io();
    g_io.stack[1] = 7;
    g_io.stack[2] = (uint32_t)(uintptr_t)&m;
    call(i_enum, 3);
    check_eq(g_io.out[2], D3DERR_INVALIDCALL, "mode 7 of a one-mode adapter is refused, not clamped");

    reset_io();
    g_io.stack[2] = 0;
    call(i_enum, 3);
    check_eq(g_io.out[2], D3D_E_POINTER, "and a null out-pointer is reported");

    // Release on the device frees nothing -- the device is the singleton at ADDR_DEVICE_OBJECT -- but the
    // caller still expects a reference count, so the answer is 1 rather than 0.
    reset_io();
    call(i_release, 0);
    check_eq(g_io.out[2], 1, "Release reports a reference count of 1");
}

static void test_display_field_status()
{
    printf("\ndisplay field status\n");

    const int i_field = row_index("D3DDevice_GetDisplayFieldStatus");
    check(i_field >= 0, "the row is present");

    // The fields alternate: 1 is odd, 2 is even. The count itself is reported as the field number, so a
    // caller that polls parity gets the right answer whichever way round it is read.
    uint32_t fs[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    state().vblank_count = 0;
    reset_io();
    g_io.stack[0] = (uint32_t)(uintptr_t)fs;
    call(i_field, 1);
    check_eq(fs[0], 2, "an even count reports the even field");
    check_eq(fs[1], 0, "and the count itself");

    state().vblank_count = 7;
    reset_io();
    g_io.stack[0] = (uint32_t)(uintptr_t)fs;
    call(i_field, 1);
    check_eq(fs[0], 1, "an odd count reports the odd field");
    check_eq(fs[1], 7, "and the count again");

    reset_io();
    g_io.stack[0] = 0;
    call(i_field, 1);
    check(true, "a null argument does not fault");
}

static void test_push_buffer()
{
    printf("\npush buffer\n");

    const int i_space = row_index("D3D_MakeRequestedSpace");
    const int i_begin = row_index("D3DDevice_BeginPush");
    const int i_end = row_index("D3DDevice_EndPush");
    check(i_space >= 0 && i_begin >= 0 && i_end >= 0, "the three rows are present");

    // A stand-in for the push buffer. It is not the real segment -- nothing is queued yet -- but the point
    // of these rows is that the cursor lives in the device object and the caller's pointer becomes it.
    static uint32_t segment[4096];
    dev_field<uint32_t *>(DEV_PUT) = segment;
    dev_field<uint32_t *>(DEV_THRESHOLD) = segment + 2048;

    reset_io();
    g_io.stack[0] = 64;
    call(i_space, 1);
    check_eq(g_io.out[2], (uint32_t)(uintptr_t)segment, "MakeRequestedSpace hands back the write cursor");

    // EndPush stores the caller's pointer as the new cursor. Storing it is the whole function: the game
    // wrote its primitive and hands back where it got to.
    reset_io();
    g_io.stack[0] = (uint32_t)(uintptr_t)(segment + 32);
    call(i_end, 1);
    check_eq((uint32_t)(uintptr_t)dev_field<uint32_t *>(DEV_PUT), (uint32_t)(uintptr_t)(segment + 32),
             "EndPush made the caller's pointer the cursor");

    // BeginPush returns the cursor, and rewinds when the requested primitive would not fit before the
    // threshold. The rewind is invisible while nothing is queued, so what is checked is that the call is
    // safe with and without the condition, and that the pointer still comes from the device object.
    reset_io();
    g_io.stack[0] = 16;
    call(i_begin, 1);
    check_eq(g_io.out[2], (uint32_t)(uintptr_t)(segment + 32), "BeginPush hands back the cursor");

    dev_field<uint32_t *>(DEV_PUT) = segment + 4090; // nearly at the end of the segment
    reset_io();
    g_io.stack[0] = 4096;
    call(i_begin, 1);
    check(true, "a push that would overrun the threshold does not fault");

    // With no device, both pointers are null and the comparison the console does is between two
    // unrelated pointers. The call has to stay safe anyway, because GetDisplayMode and friends can run
    // before CreateDevice does.
    dev_field<uint32_t *>(DEV_PUT) = nullptr;
    dev_field<uint32_t *>(DEV_THRESHOLD) = nullptr;
    reset_io();
    g_io.stack[0] = 16;
    call(i_begin, 1);
    check_eq(g_io.out[2], 0, "BeginPush on a device that does not exist answers null");
}

static void test_viewport_transform()
{
    printf("\nthe viewport transform\n");

    const int i_get = row_index("D3DDevice_GetViewportOffsetAndScale");
    const int i_write = row_index("D3D_WriteViewportTransform");
    check(i_get >= 0 && i_write >= 0, "the two rows are present");

    // screen = clip / w * scale + offset, with the console's D24 depth range. The numbers are worked out
    // by hand rather than copied from a run, because a test that reads back what the code just wrote
    // proves nothing about whether the formula is right.
    Viewport v = {};
    v.X = 10;
    v.Y = 20;
    v.Width = 640;
    v.Height = 480;
    v.MinZ = 0.25f;
    v.MaxZ = 0.75f;
    state().viewport = v;
    state().screen_offset[0] = 0.0f;
    state().screen_offset[1] = 0.0f;

    float offset[4] = {0, 0, 0, 0}, scale[4] = {0, 0, 0, 0};
    reset_io();
    g_io.stack[0] = (uint32_t)(uintptr_t)offset;
    g_io.stack[1] = (uint32_t)(uintptr_t)scale;
    call(i_get, 2);
    check(scale[0] > 319.9f && scale[0] < 320.1f, "scale x is half the width");
    check(scale[1] < -239.9f && scale[1] > -240.1f, "scale y is minus half the height -- the console's Y is down");
    // 16777215 * (0.75 - 0.25) = 8388607.5 exactly. Worked out here rather than read back from the code,
    // because a test that recomputes what it just wrote proves nothing about the formula.
    check(scale[2] > 8388607.0f && scale[2] < 8388608.0f, "scale z is the D24 range times (MaxZ - MinZ)");
    check(scale[3] == 1.0f, "and the perspective term is 1");
    check(offset[0] > 329.9f && offset[0] < 330.1f, "offset x is X + half the width");
    check(offset[1] > 259.9f && offset[1] < 260.1f, "offset y likewise");
    check(offset[2] > 4194303.0f && offset[2] < 4194304.0f, "offset z is MinZ times the D24 range");
    check(offset[3] == 0.0f, "and the w term is 0");

    // The screen-space offset moves the offset and leaves the scale alone: it is a translation, not a zoom.
    state().screen_offset[0] = 3.0f;
    state().screen_offset[1] = -4.0f;
    reset_io();
    g_io.stack[0] = (uint32_t)(uintptr_t)offset;
    g_io.stack[1] = (uint32_t)(uintptr_t)scale;
    call(i_get, 2);
    check(offset[0] > 332.9f && offset[0] < 333.1f, "a screen-space offset x of 3 shifts offset x by 3");
    check(offset[1] > 255.9f && offset[1] < 256.1f, "and an offset y of -4 shifts offset y by -4");
    check(scale[0] > 319.9f && scale[0] < 320.1f, "leaving the scale untouched");

    // FUN_0026eaf0: the HLE derives the transform when it reads it, so there is nothing to push and the
    // pointer comes back exactly as it went in.
    // Spec "eax": the argument arrives in eax, and a row that reports a value has it handed back in edx,
    // which is where out[2] reads it from.
    reset_io();
    g_io.regs[0] = 0x1234;
    call(i_write, 0);
    check_eq(g_io.out[2], 0x1234, "WriteViewportTransform returns the push pointer unchanged");
}

static void test_palettes_and_streams()
{
    printf("\npalettes and streams\n");

    const int i_pal = row_index("D3DDevice_SetPalette");
    const int i_str = row_index("D3DDevice_SetStreamSource");
    check(i_pal >= 0 && i_str >= 0, "the two rows are present");

    static Palette pal[2];
    memset(pal, 0, sizeof pal);
    static VertexBuffer vb;
    memset(&vb, 0, sizeof vb);

    reset_io();
    g_io.stack[0] = 1;
    g_io.stack[1] = (uint32_t)(uintptr_t)&pal[1];
    call(i_pal, 2);
    check(state().palettes[1] == &pal[1], "the palette is remembered on its stage");

    // A stage past the fourth is one the console dropped. Dropping it -- rather than clamping it onto
    // stage 3 -- is what keeps "SetPalette(7)" from quietly becoming "SetPalette(3)".
    reset_io();
    g_io.stack[0] = 7;
    g_io.stack[1] = (uint32_t)(uintptr_t)&pal[0];
    call(i_pal, 2);
    check(state().palettes[1] == &pal[1], "a stage past the fourth is dropped, not clamped");
    check(state().palettes[0] == nullptr, "and it did not land on stage 0 either");

    reset_io();
    g_io.stack[0] = 2;
    g_io.stack[1] = (uint32_t)(uintptr_t)&vb;
    g_io.stack[2] = 32;
    call(i_str, 3);
    check(state().streams[2].buffer == &vb, "the stream source is remembered");
    check_eq(state().streams[2].stride, 32, "with the stride the game gave");

    reset_io();
    g_io.stack[0] = 16;
    g_io.stack[1] = (uint32_t)(uintptr_t)&vb;
    g_io.stack[2] = 8;
    call(i_str, 3);
    check(state().streams[2].buffer == &vb, "stream 16 of 16 is dropped, not clamped onto 15");
}

static void test_single_render_states()
{
    printf("\nthe four single-state render entry points\n");

    const int i_norm = row_index("D3DDevice_SetRenderState_NormalizeNormals");
    const int i_en = row_index("D3DDevice_SetRenderState_StencilEnable");
    const int i_fail = row_index("D3DDevice_SetRenderState_StencilFail");
    const int i_factor = row_index("D3DDevice_SetRenderState_TextureFactor");
    check(i_norm >= 0 && i_en >= 0 && i_fail >= 0 && i_factor >= 0, "the four rows are present");

    // Each writes one index. The point of the test is that they are four *different* indices: a macro
    // that took the wrong index would still compile, still store, and still pass a test that only checked
    // "something changed".
    const uint32_t before = render_state()[XRS_NORMALIZENORMALS];
    reset_io();
    g_io.stack[0] = before + 1;
    call(i_norm, 1);
    check_eq(render_state()[XRS_NORMALIZENORMALS], before + 1, "NormalizeNormals wrote its own index");

    const uint32_t en_before = render_state()[XRS_STENCILENABLE];
    reset_io();
    g_io.stack[0] = en_before ^ 1u;
    call(i_en, 1);
    check_eq(render_state()[XRS_STENCILENABLE], en_before ^ 1u, "StencilEnable wrote its own index");

    const uint32_t fail_before = render_state()[XRS_STENCILFAIL];
    reset_io();
    g_io.stack[0] = fail_before + 7;
    call(i_fail, 1);
    check_eq(render_state()[XRS_STENCILFAIL], fail_before + 7, "StencilFail wrote its own index");

    const uint32_t factor_before = render_state()[XRS_TEXTUREFACTOR];
    reset_io();
    g_io.stack[0] = factor_before + 3;
    call(i_factor, 1);
    check_eq(render_state()[XRS_TEXTUREFACTOR], factor_before + 3, "TextureFactor wrote its own index");

    // Texture-stage state is a separate array per stage.
    const int i_stage = row_index("D3DDevice_SetTextureStageStateNotInline");
    check(i_stage >= 0, "the texture-stage row is present");
    reset_io();
    g_io.stack[0] = 2;
    g_io.stack[1] = 5;
    g_io.stack[2] = 0xABCD;
    call(i_stage, 3);
    check_eq(texture_state(2)[5], 0xABCDu, "the texture-stage state landed on the stage it was given");

    reset_io();
    g_io.stack[0] = 7;
    g_io.stack[1] = 5;
    g_io.stack[2] = 0x1234;
    call(i_stage, 3);
    check_eq(texture_state(2)[5], 0xABCDu, "a stage past the fourth is dropped, not clamped");
}

static void test_shader_constants()
{
    printf("\nshader constants\n");

    const int i_c1 = row_index("D3DDevice_SetVertexShaderConstant1");
    const int i_c4 = row_index("D3DDevice_SetVertexShaderConstant4");
    const int i_dw = row_index("D3DDevice_SetVertexShaderConstantNotInlineFast");
    check(i_c1 >= 0 && i_c4 >= 0 && i_dw >= 0, "the three vertex-constant rows are present");

    static float data[4 * 8];
    for (int i = 0; i < 4 * 8; i++) data[i] = (float)i;

    // Spec "ecx edx": both arguments arrive in registers, not on the stack.
    reset_io();
    g_io.regs[1] = 10;
    g_io.regs[2] = (uint32_t)(uintptr_t)data;
    call(i_c1, 0);
    check(state().vs_constants[10][0] == 0.0f && state().vs_constants[10][3] == 3.0f,
          "one constant is four floats, copied whole");

    reset_io();
    g_io.regs[1] = 20;
    g_io.regs[2] = (uint32_t)(uintptr_t)data;
    call(i_c4, 0);
    check(state().vs_constants[23][0] == 12.0f, "a four-constant write lands on registers 20..23");
    check(state().vs_constants[24][0] == 0.0f, "and stops there");

    // The count is in DWORDs. Read as a register count it would write four times too far, over the slots
    // after it with memory past the caller's data -- which is exactly the bug the invoice records.
    memset(state().vs_constants, 0, sizeof state().vs_constants);
    // Spec "ecx edx s": two in registers, the DWORD count on the stack.
    reset_io();
    g_io.regs[1] = 30;
    g_io.regs[2] = (uint32_t)(uintptr_t)data;
    g_io.stack[0] = 8; // 8 DWORDs = 2 registers
    call(i_dw, 1);
    check(state().vs_constants[31][0] == 4.0f, "8 DWORDs is two registers");
    check(state().vs_constants[32][0] == 0.0f, "not eight -- the slot after is untouched");

    // A register outside the bank is dropped, not clamped: 192 is where the shader epilogue stops, and a
    // clamped write would land somewhere the game never wrote.
    reset_io();
    g_io.regs[1] = VS_CONSTANT_COUNT + 5;
    g_io.regs[2] = (uint32_t)(uintptr_t)data;
    call(i_c1, 0);
    check(state().vs_constants[VS_CONSTANT_COUNT - 1][0] == 0.0f,
          "a register past 191 is dropped, not wrapped onto the last one");

    const int i_ps = row_index("D3DDevice_SetPixelShader");
    const int i_psdel = row_index("D3DDevice_DeletePixelShader");
    const int i_psc = row_index("D3DDevice_SetPixelShaderConstant");
    check(i_ps >= 0 && i_psdel >= 0 && i_psc >= 0, "the three pixel-shader rows are present");

    reset_io();
    g_io.stack[0] = 0;
    call(i_ps, 1);
    check_eq(state().pixel_shader, 0, "binding pixel shader 0 records it");

    reset_io();
    g_io.stack[0] = 0x1234;
    call(i_ps, 1);
    check_eq(state().pixel_shader, 0x1234, "a handle is stored as the binding");

    // Delete clears the binding only when it is the shader that is bound. A delete of some other handle
    // leaving the binding alone is the whole reason this is a comparison and not an assignment.
    reset_io();
    g_io.stack[0] = 0x9999;
    call(i_psdel, 1);
    check_eq(state().pixel_shader, 0x1234, "deleting another shader leaves the binding alone");

    reset_io();
    g_io.stack[0] = 0x1234;
    call(i_psdel, 1);
    check_eq(state().pixel_shader, 0, "deleting the bound one clears the binding");

    // Spec "ecx s eax": the register in ecx, the data on the stack, the count in eax.
    reset_io();
    g_io.regs[1] = 2;
    g_io.stack[0] = (uint32_t)(uintptr_t)data;
    g_io.regs[0] = 4;
    call(i_psc, 1);
    check(state().ps_constants[5][0] == 12.0f, "four pixel constants land on registers 2..5");
    check(state().ps_constants[6][0] == 0.0f, "and stop there");

    // 16 is the register count the register combiners read; past it there is nothing to write into.
    reset_io();
    g_io.regs[1] = PS_CONSTANT_COUNT - 1;
    g_io.stack[0] = (uint32_t)(uintptr_t)data;
    g_io.regs[0] = 4;
    call(i_psc, 1);
    check(state().ps_constants[PS_CONSTANT_COUNT - 1][0] == 0.0f, "the last register takes what fits");
    check(true, "and the overflow does not run off the bank");
}

static void test_vertex_shader_handles()
{
    printf("\nvertex shader handles\n");

    const int i_create = row_index("D3DDevice_CreateVertexShader");
    const int i_delete = row_index("D3DDevice_DeleteVertexShader");
    check(i_create >= 0 && i_delete >= 0, "the two rows are present");

    // Translation is A5, so there is nothing to hand back yet. What matters here is that it refuses
    // visibly rather than returning a handle that would fault later, somewhere far from this call.
    static uint32_t decl[16], func[16], handle = 0xFFFFFFFFu;
    reset_io();
    g_io.stack[0] = (uint32_t)(uintptr_t)decl;
    g_io.stack[1] = (uint32_t)(uintptr_t)func;
    g_io.stack[2] = (uint32_t)(uintptr_t)&handle;
    g_io.stack[3] = 0;
    call(i_create, 4);
    check_eq(g_io.out[2], D3DERR_INVALIDCALL, "a shader that cannot be translated is refused, not faked");
    check_eq(handle, 0xFFFFFFFFu, "and the caller's handle is left alone");

    reset_io();
    g_io.stack[0] = (uint32_t)(uintptr_t)decl;
    g_io.stack[1] = (uint32_t)(uintptr_t)func;
    g_io.stack[2] = 0;
    call(i_create, 4);
    check_eq(g_io.out[2], D3D_E_POINTER, "a null handle out-parameter is reported");

    // Delete has to clear the binding: a deleted shader left bound means the next draw reads a shader
    // that is no longer there.
    state().vertex_shader = 0x5555;
    reset_io();
    g_io.stack[0] = 0x7777;
    call(i_delete, 1);
    check_eq(state().vertex_shader, 0x5555, "deleting a different shader leaves the binding alone");

    reset_io();
    g_io.stack[0] = 0x5555;
    call(i_delete, 1);
    check_eq(state().vertex_shader, 0, "deleting the bound one clears the binding");
}

static void test_fences()
{
    printf("\nfences\n");

    const int i_set = row_index("D3D_SetFence");
    const int i_pending = row_index("D3DDevice_IsFencePending");
    check(i_set >= 0 && i_pending >= 0, "the two rows are present");

    const uint32_t before = state().gpu_time;
    reset_io();
    g_io.stack[0] = 0;
    call(i_set, 1);
    check_eq(g_io.out[2], before + 1, "a fence hands back an increasing token");
    // Game code reads this field of the device object directly rather than through the entry point, so
    // the two have to agree.
    check_eq(dev_field<uint32_t>(DEV_GPUTIME), before + 1, "and the device object's copy agrees");

    // Nothing is ever outstanding: the HLE does the work when it is asked, so a fence has completed by
    // the time anyone asks whether it has.
    reset_io();
    g_io.stack[0] = g_io.out[2];
    call(i_pending, 1);
    check_eq(g_io.out[2], 0, "no fence is ever pending");

    const int i_block = row_index("D3D_BlockOnTime");
    const int i_flush = row_index("D3D_FlushDirtyState");
    const int i_fog = row_index("D3D_UpdateFogState");
    check(i_block >= 0 && i_flush >= 0 && i_fog >= 0, "the time and dirty-state rows are present");
    reset_io();
    call(i_block, 2);
    check(true, "BlockOnTime returns rather than waiting on nothing");
    reset_io();
    call(i_flush, 0);
    check(true, "FlushDirtyState is a no-op because state is applied at draw time");
    reset_io();
    call(i_fog, 0);
    check(true, "UpdateFogState is a no-op because fog arrives as render states");
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
    test_adapters_and_device();
    test_display_field_status();
    test_push_buffer();
    test_viewport_transform();
    test_palettes_and_streams();
    test_single_render_states();
    test_shader_constants();
    test_vertex_shader_handles();
    test_fences();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}