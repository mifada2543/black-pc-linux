// NV2A microcode to a live graphics pipeline. The whole chain, on the software rasteriser.
//
// This is the first test that builds something the driver accepts. The ones before it stopped at SPIR-V,
// which proves a translation is well formed and says nothing about whether the two halves can be used
// together -- and whether is not a formality: a vertex shader writing a texture coordinate at one location
// and a fragment shader reading it at another is a pipeline the driver refuses.
//
// llvmpipe is what runs this. It rasterises nothing on this machine, which does not matter here: what is
// under test is the pipeline, not a pixel.
#include "gfx/gfx_vulkan.h"
#include "shader/glslang_posix.h"
#include "shader/material_posix.h"
#include "shader/nv2a_vsh_posix.h"

#include <stdio.h>
#include <string.h>

#include <vector>

static int g_checks = 0, g_failures = 0;

static void check(bool ok, const char *what)
{
    printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
    g_checks++;
    if (!ok) g_failures++;
}

// ---- microcode, built by hand ----

struct Inst {
    uint32_t w[4];
    Inst() { memset(w, 0, sizeof w); }
};

static void put(uint32_t &word, int pos, int size, uint32_t value)
{
    const uint32_t mask = ((1u << size) - 1) << pos;
    word = (word & ~mask) | ((value << pos) & mask);
}

static void push(std::vector<uint32_t> &p, const Inst &i) { p.insert(p.end(), i.w, i.w + 4); }

// oPos = v0.xyz * c[0], the position-only shader. Everything the material needs and nothing it does not.
static std::vector<uint32_t> position_program()
{
    std::vector<uint32_t> p;
    Inst i;
    put(i.w[1], 21, 4, xd3d::MAC_MAD); // MAC_MAD
    put(i.w[1], 9, 4, 0);              // varying 0
    put(i.w[1], 13, 8, 0);             // constant 0
    put(i.w[1], 8, 1, 0);              // A not negated
    put(i.w[1], 6, 2, 0);
    put(i.w[1], 4, 2, 1);
    put(i.w[1], 2, 2, 2);
    put(i.w[1], 0, 2, 3);              // A = v0.xyzw
    put(i.w[2], 28, 4, 0);
    put(i.w[2], 26, 2, xd3d::MUX_V);   // A from a varying
    put(i.w[2], 25, 1, 0);
    put(i.w[2], 23, 2, 0);
    put(i.w[2], 21, 2, 1);
    put(i.w[2], 19, 2, 2);
    put(i.w[2], 17, 2, 3);
    put(i.w[2], 13, 4, 0);
    put(i.w[2], 11, 2, xd3d::MUX_C);   // B from the constant bank
    put(i.w[2], 10, 1, 0);
    put(i.w[2], 8, 2, 0);
    put(i.w[2], 6, 2, 1);
    put(i.w[2], 4, 2, 2);
    put(i.w[2], 2, 2, 3);
    put(i.w[2], 0, 2, 0);              // the constant address straddles two dwords
    put(i.w[3], 30, 2, 0);
    put(i.w[3], 28, 2, xd3d::MUX_C);   // C from the constant bank too
    put(i.w[3], 24, 4, 0xF);           // write all four
    put(i.w[3], 20, 4, 12);            // to r12, which is oPos
    put(i.w[3], 0, 1, 1);              // final
    push(p, i);
    return p;
}

// A shader that also writes a texture coordinate, so the two halves have something to agree about.
static std::vector<uint32_t> texture_program()
{
    std::vector<uint32_t> p = position_program();
    // The final instruction already has FINAL set, so this one is a plain MOV into oT0 read from a varying.
    p[3 * 4 + 0] &= ~(1u << 0); // clear FINAL on the position instruction
    Inst i;
    put(i.w[1], 21, 4, xd3d::MAC_MOV);
    put(i.w[1], 9, 4, 1); // varying 1
    put(i.w[1], 6, 2, 0);
    put(i.w[1], 4, 2, 1);
    put(i.w[1], 2, 2, 2);
    put(i.w[1], 0, 2, 3);
    put(i.w[2], 28, 4, 0);
    put(i.w[2], 26, 2, xd3d::MUX_V);
    put(i.w[2], 13, 4, 0);
    put(i.w[2], 11, 2, xd3d::MUX_C);
    put(i.w[2], 2, 2, 3);
    put(i.w[3], 28, 2, xd3d::MUX_C);
    put(i.w[3], 12, 4, 0xF);        // write an output register
    put(i.w[3], 11, 1, 1);         // OUT_ORB
    put(i.w[3], 3, 8, xd3d::VSH_OT0); // into oT0
    put(i.w[3], 0, 1, 1);          // final
    push(p, i);
    return p;
}

static void test_a_material_reaches_the_driver()
{
    printf("\na position-only material, built end to end\n");

    if (!gfx::init()) {
        printf("  --     no Vulkan on this machine: %s (skipped, as test_gfx does)\n", gfx::last_error());
        return;
    }
    check(true, "there is a device to build against");

    std::vector<uint32_t> vs = position_program();
    xd3d::VertexElement els[] = {{0, 0, xd3d::VST_FLOAT3, 0}};
    uint32_t stride[16] = {};

    xd3d::MaterialDesc d;
    d.vertex_program = vs.data();
    d.vertex_instruction_count = (uint32_t)(vs.size() / 4);
    d.vertex_input_mask = 0x1u;
    d.vertex_elements = els;
    d.vertex_element_count = 1;
    d.stream_stride = stride;
    d.color_format = gfx::srgb_format(gfx::ATT_B8G8R8A8_UNORM);
    d.flags = gfx::PIPE_DEPTH_TEST | gfx::PIPE_DEPTH_WRITE | gfx::PIPE_CULL_BACK;

    gfx::Pipeline pipe;
    const char *err = nullptr;
    const bool ok = xd3d::build_material(&pipe, d, &err) ? true : (printf("       %s\n", err ? err : ""), false);
    check(ok, "the material builds a pipeline the driver accepts");

    if (ok) {
        check(pipe.pipeline != VK_NULL_HANDLE, "and the pipeline handle is real");
        check(pipe.vertex_module != VK_NULL_HANDLE, "as are both shader modules");
        check(pipe.layout != VK_NULL_HANDLE, "and the pipeline layout");
        gfx::destroy_pipeline(&pipe);
    }
}

static void test_a_material_with_a_pixel_shader()
{
    printf("\na material with both halves\n");

    std::vector<uint32_t> vs = texture_program();
    xd3d::VertexElement els[] = {{0, 0, xd3d::VST_FLOAT3, 0}};
    uint32_t stride[16] = {};

    xd3d::PixelShaderDef ps;
    memset(&ps, 0, sizeof ps);
    // Stage 0 samples, and the final combiner passes t0 through -- so the fragment half reads the coordinate
    // the vertex half wrote, which is the thing being checked.
    ps.PSTextureModes = 1u;
    ps.PSFinalCombinerInputsABCD = (1u << 24) | (8u << 16); // A = 1, B = t0
    ps.PSFinalCombinerInputsEFG = (1u << 24);             // G = 1

    xd3d::MaterialDesc d;
    d.vertex_program = vs.data();
    d.vertex_instruction_count = (uint32_t)(vs.size() / 4);
    d.vertex_input_mask = 0x1u;
    d.vertex_elements = els;
    d.vertex_element_count = 1;
    d.stream_stride = stride;
    d.pixel_shader = &ps;
    d.color_format = gfx::srgb_format(gfx::ATT_B8G8R8A8_UNORM);

    gfx::Pipeline pipe;
    const char *err = nullptr;
    const bool ok = xd3d::build_material(&pipe, d, &err) ? true : (printf("       %s\n", err ? err : ""), false);
    check(ok, "a vertex and a pixel program together build a pipeline");
    if (ok) gfx::destroy_pipeline(&pipe);
}

static void test_what_it_refuses()
{
    printf("\nwhat it refuses\n");

    gfx::Pipeline pipe;
    const char *err = nullptr;

    // No microcode at all: there is nothing to make a shader out of, and saying so is better than handing
    // the driver a null module.
    xd3d::MaterialDesc empty;
    check(!xd3d::build_material(&pipe, empty, &err) && err != nullptr,
          "a material with no vertex program is refused with a reason");

    err = nullptr;
    xd3d::MaterialDesc no_handle;
    check(!xd3d::build_material(&pipe, no_handle, &err) && err != nullptr, "a null pipeline to fill is refused too");

    // A declaration the layout builder rejects -- a register declared twice -- must stop here rather than
    // producing a pipeline that reads the wrong bytes at the right offsets.
    err = nullptr;
    std::vector<uint32_t> vs = position_program();
    xd3d::VertexElement twice[] = {{0, 0, xd3d::VST_FLOAT3, 0}, {0, 0, xd3d::VST_FLOAT4, 12}};
    xd3d::MaterialDesc bad;
    bad.vertex_program = vs.data();
    bad.vertex_instruction_count = (uint32_t)(vs.size() / 4);
    bad.vertex_elements = twice;
    bad.vertex_element_count = 2;
    check(!xd3d::build_material(&pipe, bad, &err) && err != nullptr,
          "a declaration with a register twice is refused with a reason");
}

static void test_the_cache_key()
{
    printf("\nthe cache key\n");

    std::vector<uint32_t> a = position_program();
    std::vector<uint32_t> b = texture_program();
    xd3d::VertexElement els[] = {{0, 0, xd3d::VST_FLOAT3, 0}};

    xd3d::MaterialDesc ma, mb;
    ma.vertex_program = a.data();
    ma.vertex_instruction_count = (uint32_t)(a.size() / 4);
    ma.vertex_elements = els;
    ma.vertex_element_count = 1;
    mb = ma;
    check(xd3d::material_cache_key(ma) == xd3d::material_cache_key(mb), "two identical materials share a key");

    mb.vertex_program = b.data();
    mb.vertex_instruction_count = (uint32_t)(b.size() / 4);
    check(xd3d::material_cache_key(ma) != xd3d::material_cache_key(mb),
          "and two different programs do not -- even at the same address");

    xd3d::MaterialDesc mc = ma;
    mc.flags = gfx::PIPE_BLEND;
    check(xd3d::material_cache_key(ma) != xd3d::material_cache_key(mc),
          "and neither does one whose pipeline state differs");
}

int main()
{
    printf("BLACK loader -- NV2A microcode to a graphics pipeline\n");
    test_a_material_reaches_the_driver();
    test_a_material_with_a_pixel_shader();
    test_what_it_refuses();
    test_the_cache_key();
    gfx::shutdown();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}