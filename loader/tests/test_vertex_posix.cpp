// The vertex-format table and the layout builder, tested on their own.
//
// These are pure functions over the console's own declaration format, which is why they can be tested
// without a device: the question they answer is "what would the GPU be told to read", and the answer is
// arithmetic. What that buys is narrow on purpose -- it does not make anything draw, and the gate that
// would say so (A0) is still waiting on a machine whose GPU submits.
#include "d3d/vertex_posix.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0, g_failures = 0;

static void check(bool ok, const char *what)
{
    printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
    g_checks++;
    if (!ok) g_failures++;
}

static void check_eq(uint64_t got, uint64_t want, const char *what)
{
    printf("  %s   %s (got %llu, want %llu)\n", got == want ? "ok  " : "FAIL", what, (unsigned long long)got,
           (unsigned long long)want);
    g_checks++;
    if (got != want) g_failures++;
}

static void test_widths()
{
    printf("\nthe widths are the console's, not a formula\n");

    // The irregular ones first: FLOAT3 is 12 bytes and PBYTE3 is 3, so nothing here is a function of the
    // component count.
    check_eq(xd3d::vsdt_size(xd3d::VST_FLOAT3), 12, "FLOAT3 is 12 bytes");
    check_eq(xd3d::vsdt_size(xd3d::VST_PBYTE3), 3, "PBYTE3 is 3 bytes");
    check_eq(xd3d::vsdt_size(xd3d::VST_FLOAT4), 16, "FLOAT4 is 16 bytes");
    check_eq(xd3d::vsdt_size(xd3d::VST_PBYTE4), 4, "PBYTE4 is 4 bytes");
    // The one that catches people: three components in four bytes.
    check_eq(xd3d::vsdt_size(xd3d::VST_NORMPACKED3), 4, "NORMPACKED3 is three components in four bytes");
    check_eq(xd3d::vsdt_size(xd3d::VST_FLOAT2H), 4, "FLOAT2H is two halves in four bytes");
    check_eq(xd3d::vsdt_size(xd3d::VST_SHORT4), 8, "SHORT4 is 8 bytes");
    check_eq(xd3d::vsdt_size(xd3d::VST_NORMSHORT4), 8, "NORMSHORT4 is 8 bytes too");
    check_eq(xd3d::vsdt_size(xd3d::VST_NONE), 0, "NONE occupies nothing");
    // An unknown type is zero rather than a guess: that is what makes an unrecognised token in a
    // declaration visible instead of a stride that quietly goes wrong for every element after it.
    check_eq(xd3d::vsdt_size(0x99), 0, "a type this build does not know has no width");
}

static void test_formats()
{
    printf("\nwhat each type becomes\n");

    check_eq(xd3d::vertex_format_code(xd3d::VST_FLOAT4), xd3d::VF_R32G32B32A32_SFLOAT, "FLOAT4 is four floats");
    check_eq(xd3d::vertex_format_code(xd3d::VST_FLOAT3), xd3d::VF_R32G32B32_SFLOAT, "FLOAT3 is three floats");
    check_eq(xd3d::vertex_format_code(xd3d::VST_D3DCOLOR), xd3d::VF_R8G8B8A8_UNORM, "D3DCOLOR is RGBA8");
    check_eq(xd3d::vertex_format_code(xd3d::VST_NORMSHORT2), xd3d::VF_R16G16_SNORM, "NORMSHORT2 is two normalised shorts");
    check_eq(xd3d::vertex_format_code(xd3d::VST_SHORT2), xd3d::VF_R16G16_SINT, "SHORT2 is two signed shorts");

    // The one that decides whether the shader has work to do. Reporting NORMPACKED3 as the nearest RGBA
    // format would put three components roughly in place and fill the fourth with buffer bytes nobody
    // meant to read -- wrong geometry, drawn confidently, instead of a refusal.
    check_eq(xd3d::vertex_format_code(xd3d::VST_NORMPACKED3), xd3d::VF_UNPACKED,
             "NORMPACKED3 has no Vulkan format and says so");
    check(!xd3d::vertex_format_is_native(xd3d::VF_UNPACKED), "and it is not counted as native");
    check(xd3d::vertex_format_is_native(xd3d::VF_R32G32B32_SFLOAT), "an ordinary format is native");
    check(!xd3d::vertex_format_is_native(xd3d::VF_NONE), "reading nothing is not native either");
}

static void test_layout()
{
    printf("\na declaration becomes a layout\n");

    // Position on stream 0, colour on stream 1 -- the shape the invoice's convert_vertices() produces, and
    // the one that proves the two numberings meet in exactly one place.
    const xd3d::VertexElement els[] = {
        {0, 0, xd3d::VST_FLOAT3, 0},
        {1, 1, xd3d::VST_D3DCOLOR, 0},
        {0, 2, xd3d::VST_FLOAT2, 12},
    };
    uint32_t strides[16] = {};
    xd3d::VertexLayout layout;
    const char *err = nullptr;
    const bool ok = xd3d::build_vertex_layout(els, 3, strides, &layout, &err);
    check(ok && err == nullptr, "a two-stream declaration builds");
    check_eq(layout.attribute_count, 3, "with one attribute per element");
    check_eq(layout.binding_count, 2, "and one binding per stream that is used");

    // Registers are what the shader addresses, so they must survive unchanged.
    check_eq(layout.attributes[0].location, 0, "register 0 is attribute 0");
    check_eq(layout.attributes[2].location, 2, "register 2 is attribute 2");
    check_eq(layout.attributes[0].format, xd3d::VF_R32G32B32_SFLOAT, "and keeps the console's type");

    // The attribute rows name the console's stream; the binding rows name Vulkan's. A layout that mixed
    // the two would read a plausible-looking offset from the wrong buffer.
    check_eq(layout.attributes[0].binding, 0, "stream 0 is binding 0");
    check_eq(layout.attributes[1].binding, 1, "stream 1 is binding 1");
    check_eq(layout.attributes[2].binding, 0, "and the third element is back on binding 0");
    check_eq(layout.attributes[2].offset, 12, "at the offset the declaration gave");

    // No stride was given, so it is measured from the declaration: 12 + 8 = 20 for stream 0.
    check_eq(layout.bindings[0].stride, 20, "stream 0's stride is measured from its elements");
    check_eq(layout.bindings[1].stride, 4, "stream 1's is just its one element");

    // A stride the game did give wins over the measured one -- the game knows what it wrote.
    uint32_t given[16] = {};
    given[0] = 64;
    xd3d::VertexLayout layout2;
    const bool ok2 = xd3d::build_vertex_layout(els, 3, given, &layout2, &err);
    check(ok2, "and it builds again with a stride given");
    check_eq(layout2.bindings[0].stride, 64, "the game's stride is the one used");
    check_eq(layout2.bindings[1].stride, 4, "while the stream the game said nothing about is still measured");
}

static void test_refusals()
{
    printf("\nwhat it refuses\n");

    uint32_t strides[16] = {};
    xd3d::VertexLayout layout;
    const char *err = nullptr;

    // A register declared twice: the console took the first, and taking the last would move an offset a
    // shader is already compiled against.
    const xd3d::VertexElement twice[] = {
        {0, 3, xd3d::VST_FLOAT3, 0},
        {0, 3, xd3d::VST_FLOAT4, 12},
    };
    check(!xd3d::build_vertex_layout(twice, 2, strides, &layout, &err) && err != nullptr,
          "a register declared twice is refused");
    check(err && strstr(err, "twice") != nullptr, "and it says why");

    const xd3d::VertexElement unknown[] = {{0, 0, 0x99, 0}};
    check(!xd3d::build_vertex_layout(unknown, 1, strides, &layout, &err) && err != nullptr,
          "an element type this build does not know is refused");

    const xd3d::VertexElement far[] = {{17, 0, xd3d::VST_FLOAT3, 0}};
    check(!xd3d::build_vertex_layout(far, 1, strides, &layout, &err) && err != nullptr,
          "a stream past the sixteenth is refused");

    // An element of type NONE reads nothing, so it contributes no attribute -- but it is not an error
    // either, because a declaration is allowed to mention a register it does not feed.
    const xd3d::VertexElement with_none[] = {
        {0, 0, xd3d::VST_FLOAT3, 0},
        {0, 1, xd3d::VST_NONE, 12},
    };
    err = nullptr;
    check(xd3d::build_vertex_layout(with_none, 2, strides, &layout, &err), "an element of type NONE is not an error");
    check_eq(layout.attribute_count, 1, "but it contributes no attribute");
}

// A declaration written the way the console's own parser reads it: the top three bits are the token kind.
#define TOK_NOP      (0u << 29)
#define TOK_STREAM(n) ((1u << 29) | (uint32_t)(n))
#define TOK_ELEM(reg, type) ((2u << 29) | ((uint32_t)(type) << 16) | (uint32_t)(reg))
#define TOK_END      0xFFFFFFFFu
#define TOK_CONSTMEM(count, addr) ((4u << 29) | ((uint32_t)(count) << 25) | (uint32_t)(addr))

static void test_declaration()
{
    printf("\na declaration token stream\n");

    // Stream 0: position (reg 0, FLOAT3) then texture 0 (reg 1, FLOAT2). Stream 1: colour (reg 3).
    const uint32_t decl[] = {
        TOK_STREAM(0),
        TOK_ELEM(0, xd3d::VST_FLOAT3),
        TOK_ELEM(1, xd3d::VST_FLOAT2),
        TOK_STREAM(1),
        TOK_ELEM(3, xd3d::VST_D3DCOLOR),
        TOK_END,
    };
    xd3d::VertexDeclaration d;
    const char *err = nullptr;
    check(xd3d::parse_vertex_declaration(decl, 6, &d, &err), "a well-formed declaration parses");
    check_eq(d.count, 3, "with three elements");
    check(d.ended, "and it said where it ends");

    // Offsets accumulate per stream, and that accumulation is the whole reason the parser tracks them: an
    // element whose offset is wrong reads the right bytes of the wrong vertex.
    check_eq(d.elements[0].offset, 0, "the first element is at the start of its stream");
    check_eq(d.elements[0].type, xd3d::VST_FLOAT3, "and is a FLOAT3");
    check_eq(d.elements[1].offset, 12, "the second follows it at 12 bytes");
    check_eq(d.elements[1].type, xd3d::VST_FLOAT2, "and is a FLOAT2");
    check_eq(d.elements[2].stream, 1, "the third is on stream 1");
    check_eq(d.elements[2].reg, 3, "register 3, as the token said");
    check_eq(d.stream_stride[0], 20, "stream 0 measures 20 bytes: 12 plus 8");
    check_eq(d.stream_stride[1], 4, "stream 1 measures its one element");

    // And the layout builder takes it from there without any further information.
    xd3d::VertexLayout layout;
    check(xd3d::build_vertex_layout(d.elements, d.count, d.stream_stride, &layout, &err),
          "and the parsed declaration builds a layout");
    check_eq(layout.bindings[0].stride, 20, "with the measured stride");
    check_eq(layout.attribute_count, 3, "and one attribute per element");
}

static void test_declaration_refusals()
{
    printf("\ndeclarations it refuses\n");

    xd3d::VertexDeclaration d;
    const char *err = nullptr;

    // A CONSTMEM token claiming fifteen constants is sixty dwords. Against a six-token budget that walks off
    // the end of the buffer -- which is the bug the obvious loop guard does not stop, because the skip
    // happens inside one iteration.
    const uint32_t runaway[] = {TOK_CONSTMEM(15, 0), TOK_END};
    check(!xd3d::parse_vertex_declaration(runaway, 2, &d, &err) && err != nullptr,
          "a constant block that runs past the end is refused");

    // And the same token with a budget that *does* fit, so the refusal is about the walk and not the shape.
    uint32_t big[64];
    big[0] = TOK_CONSTMEM(15, 0);
    for (int i = 1; i <= 60; i++) big[i] = TOK_NOP;
    big[61] = TOK_END;
    err = nullptr;
    check(xd3d::parse_vertex_declaration(big, 64, &d, &err), "the same token parses when the room is there");
    check_eq(d.constant_count, 15, "and its fifteen constants are recorded");
    check_eq(d.constant_address, 0, "at the address the token gave");

    // No end token inside the budget: half a declaration is not a declaration.
    const uint32_t unterminated[] = {TOK_STREAM(0), TOK_ELEM(0, xd3d::VST_FLOAT3)};
    err = nullptr;
    check(!xd3d::parse_vertex_declaration(unterminated, 2, &d, &err) && err != nullptr,
          "a declaration that never ends is refused");

    // An element type this build does not know. Laying it out anyway would leave every later offset short
    // by that element's width, which draws something plausible.
    const uint32_t alien[] = {TOK_ELEM(0, 0x99), TOK_END};
    err = nullptr;
    check(!xd3d::parse_vertex_declaration(alien, 2, &d, &err) && err != nullptr,
          "an unknown element type is refused");

    // A stated gap still occupies its space.
    const uint32_t with_gap[] = {
        TOK_ELEM(0, xd3d::VST_FLOAT3),
        (2u << 29) | 0x10000000u | (2u << 16) | 0u, // STREAMDATA skip of two dwords
        TOK_ELEM(1, xd3d::VST_FLOAT2),
        TOK_END,
    };
    err = nullptr;
    check(xd3d::parse_vertex_declaration(with_gap, 4, &d, &err), "a stated gap parses");
    check_eq(d.elements[1].offset, 20, "and the element after it moves past the gap");
}

static void test_program_header()
{
    printf("\nthe program header\n");

    uint32_t type = 0, insts = 0;
    const char *err = nullptr;
    const uint32_t program[] = {(5u << 16) | 0x2078u};
    check(xd3d::read_program_header(program, &type, &insts, &err), "a program header reads");
    check_eq(type, 0x2078, "the low half is the program type");
    check_eq(insts, 5, "and the high half is the instruction count");

    // Zero instructions is not a program, and handing it on would build a pipeline from nothing.
    const uint32_t empty[] = {0x2078u};
    check(!xd3d::read_program_header(empty, &type, &insts, &err), "a program with no instructions is refused");
}

int main()
{
    printf("BLACK loader -- vertex formats and layout\n");
    test_widths();
    test_formats();
    test_layout();
    test_refusals();
    test_declaration();
    test_declaration_refusals();
    test_program_header();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}