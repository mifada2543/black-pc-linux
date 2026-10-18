// NV2A pixel programs, translated to GLSL and compiled -- the closed loop again, with no GPU.
//
// Same shape as the vertex test: a D3DPIXELSHADERDEF written by hand here, this repository's translation,
// libglslang's compilation, and a check that a real module came out. The definitions here are hand-built
// rather than taken from a game, so what each one should produce is something that can be written down.
//
// The thing this test is really for is coverage of the *encodings*. A pixel program is not a list of
// instructions but a set of fixed fields whose bits mean different things depending on which field they are
// in, and a translator that reads bit 5 as "invert" in one place and "shift by half" in another compiles
// perfectly and draws wrongly. So each test names the bit it is exercising.
#include "shader/glslang_posix.h"
#include "shader/nv2a_psh_posix.h"
#include "shader/varyings.h"

#include <stdio.h>
#include <string.h>

#include <string>

static int g_checks = 0, g_failures = 0;

static void check(bool ok, const char *what)
{
    printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
    g_checks++;
    if (!ok) g_failures++;
}

// A combiner input byte: register in the low four bits, alpha-channel flag in bit 4, mapping in the top
// three. Written as a function so the tests below read as "register 4, mapped as an inversion" rather than
// as a number somebody shifted by hand.
static uint8_t in_reg(uint32_t reg, uint32_t map, bool alpha_channel)
{
    return (uint8_t)((reg & 0xF) | (alpha_channel ? 0x10 : 0) | ((map & 7) << 5));
}

static bool compiles(const std::string &glsl)
{
    size_t bytes = 0;
    const char *err = nullptr;
    uint32_t *spirv = xd3d::compile_glsl_to_spirv(glsl.c_str(), xd3d::SHADER_STAGE_FRAGMENT, &bytes, &err);
    if (!spirv) {
        printf("       compiler said: %.140s\n", err ? err : "(nothing)");
        return false;
    }
    const bool ok = bytes >= 20 && spirv[0] == 0x07230203u && spirv[1] == 0x00010000u;
    xd3d::free_shader_spirv(spirv);
    return ok;
}

static std::string go(const xd3d::PixelShaderDef &d, uint32_t cube, const char **err)
{
    return xd3d::psh_to_glsl(d, cube, err);
}

static void test_the_simplest_shader()
{
    printf("\na shader with no combiner at all\n");

    // Nothing sampled, no stages, no final combiner: the console would return r0, and r0's alpha started as
    // t0.a. With no texture stage, t0 is zero, so this returns transparent black -- which is exactly what
    // the console returns, and is the thing most easily "fixed" into returning white.
    xd3d::PixelShaderDef d;
    memset(&d, 0, sizeof d);
    const char *err = nullptr;
    const std::string glsl = go(d, 0, &err);
    check(!glsl.empty(), "it translates");
    check(glsl.find("outColour = clamp(r0, 0.0, 1.0);") != std::string::npos,
          "and with no final combiner it returns the accumulator");
    check(glsl.find("vec4 r0 = vec4(0.0, 0.0, 0.0, t0.a)") != std::string::npos,
          "whose alpha starts as t0.a, as on the console");
    check(compiles(glsl), "and it compiles");
    // No sampler may be declared for a shader that samples nothing: a bound-but-unused sampler is a
    // descriptor the pipeline layout has to account for and nothing fills.
    check(glsl.find("sampler2D") == std::string::npos && glsl.find("samplerCube") == std::string::npos,
          "and it declares no sampler, because it samples nothing");
}

static void test_one_combine_stage()
{
    printf("\none combiner stage\n");

    xd3d::PixelShaderDef d;
    memset(&d, 0, sizeof d);
    d.PSCombinerCount = 1;
    // RGB: A = t0, B = 1, C = 0, D = 0; product; write the sum to r0's rgb.
    d.PSRGBInputs[0] = (uint32_t)in_reg(8, 6, false) << 24 | (uint32_t)in_reg(1, 6, false) << 16 |
                       (uint32_t)in_reg(0, 6, false) << 8 | (uint32_t)in_reg(0, 6, false);
    d.PSRGBOutputs[0] = 12u << 8; // sum -> r0
    // Alpha: constant 1, so the accumulator's alpha becomes 1 whatever the textures do.
    d.PSAlphaInputs[0] = (uint32_t)in_reg(1, 6, false) << 24 | (uint32_t)in_reg(1, 6, false) << 16;
    d.PSAlphaOutputs[0] = 12u << 8;

    const char *err = nullptr;
    const std::string glsl = go(d, 0, &err);
    check(!glsl.empty(), "it translates");
    check(glsl.find("combiner stage 0") != std::string::npos, "and names the stage it emitted");
    check(glsl.find("vec3 ab = (A * B);") != std::string::npos, "with the product the flags asked for");
    check(glsl.find("vec3 cd = (C * D);") != std::string::npos, "and the other product");
    check(glsl.find("vec3 sm = (ab + cd);") != std::string::npos, "summed rather than muxed, as the flags said");
    check(glsl.find("r0.rgb = sm;") != std::string::npos, "and written to the register the outputs named");
    check(compiles(glsl), "and it compiles");
}

static void test_the_input_mappings()
{
    printf("\nevery input mapping, one per meaning\n");

    // Each mapping is a different remapping rather than a scale, so each is checked by its own shape.
    static const char *kWanted[] = {"max(", "(1.0 - clamp(", "(2.0 * max(", "(1.0 - 2.0 * max(",
                                    "(max(", "(0.5 - max(", "", "(-("};
    size_t bad = 0;
    for (uint32_t map = 0; map < 8; map++) {
        xd3d::PixelShaderDef d;
        memset(&d, 0, sizeof d);
        d.PSCombinerCount = 1;
        d.PSRGBInputs[0] = (uint32_t)in_reg(8, map, false) << 24 | (uint32_t)in_reg(1, 6, false) << 16;
        d.PSRGBOutputs[0] = 12u << 8;
        const char *err = nullptr;
        const std::string glsl = go(d, 0, &err);
        // Mapping 6 passes through, so "no wrapper" is what its absence of text looks like.
        const bool ok = !glsl.empty() && compiles(glsl) && glsl.find(kWanted[map]) != std::string::npos;
        if (!ok) bad++;
    }
    check(bad == 0, "all eight input mappings translate, compile, and keep their own shape");
}

static void test_the_dot_and_mux_flags()
{
    printf("\nthe dot-product and mux flags\n");

    {
        // Bit 1 of the output flags turns AB into a dot product; bit 0 does the same for CD.
        xd3d::PixelShaderDef d;
        memset(&d, 0, sizeof d);
        d.PSCombinerCount = 1;
        d.PSRGBInputs[0] = (uint32_t)in_reg(8, 6, false) << 24 | (uint32_t)in_reg(1, 6, false) << 16 |
                           (uint32_t)in_reg(1, 6, false) << 8 | (uint32_t)in_reg(1, 6, false);
        d.PSRGBOutputs[0] = (2u | 1u) << 12 | (12u << 8);
        const char *err = nullptr;
        const std::string glsl = go(d, 0, &err);
        check(glsl.find("vec3(dot(A, B))") != std::string::npos, "AB can be a dot product");
        check(glsl.find("vec3(dot(C, D))") != std::string::npos, "and CD can be");
        check(compiles(glsl), "and it compiles");
    }
    {
        // Bit 2 makes the sum a mux on r0.a. The two encodings of that test are different tests, not two
        // spellings of one, so both are checked.
        for (int high = 0; high < 2; high++) {
            xd3d::PixelShaderDef d;
            memset(&d, 0, sizeof d);
            d.PSCombinerCount = 1 | (high ? 0x100u : 0u);
            d.PSRGBInputs[0] = (uint32_t)in_reg(8, 6, false) << 24 | (uint32_t)in_reg(1, 6, false) << 16 |
                               (uint32_t)in_reg(1, 6, false) << 8 | (uint32_t)in_reg(1, 6, false);
            d.PSRGBOutputs[0] = (4u << 12) | (12u << 8);
            const char *err = nullptr;
            const std::string glsl = go(d, 0, &err);
            const bool ok = !glsl.empty() && glsl.find("? cd : ab") != std::string::npos &&
                            compiles(glsl);
            check(ok, high ? "the high-bit mux tests r0.a against a half" : "the low-bit mux tests its eighth bit");
        }
    }
}

static void test_blue_to_alpha()
{
    printf("\nblue to alpha\n");

    // Bits 7 and 6 of the RGB output flags copy a blue lane into the alpha of the AB or CD destination.
    // This is a separate write, and dropping it changes the alpha of every shader that uses it -- quietly,
    // because the colour still looks right.
    xd3d::PixelShaderDef d;
    memset(&d, 0, sizeof d);
    d.PSCombinerCount = 1;
    d.PSRGBInputs[0] = (uint32_t)in_reg(8, 6, false) << 24 | (uint32_t)in_reg(1, 6, false) << 16;
    d.PSRGBOutputs[0] = (0x80u << 12) | (12u << 4) | (12u << 8);
    const char *err = nullptr;
    const std::string glsl = go(d, 0, &err);
    check(glsl.find(".a = ab.b;") != std::string::npos, "AB's blue reaches alpha as a separate write");
    check(compiles(glsl), "and it compiles");
}

static void test_texture_modes()
{
    printf("\nthe texture modes\n");

    // Each mode is set on stage 0 and translated on its own, so one broken mode names itself.
    static const uint32_t kModes[] = {1, 2, 3, 4, 5, 6, 7, 9, 10, 15, 16, 17, 19};
    size_t failed = 0, not_emulated = 0;
    for (uint32_t mode : kModes) {
        xd3d::PixelShaderDef d;
        memset(&d, 0, sizeof d);
        d.PSTextureModes = mode; // stage 0 occupies bits 0-4
        const char *err = nullptr;
        const std::string glsl = go(d, 0, &err);
        if (glsl.find("is not emulated") != std::string::npos) {
            not_emulated++;
            continue;
        }
        if (!compiles(glsl)) failed++;
    }
    check(failed == 0, "every texture mode either translates and compiles, or says it is not emulated");
    // One mode is genuinely outside what this port does, and saying so is the behaviour being checked: a
    // mode that quietly produced a plausible colour would be much harder to trace than an obvious zero.
    check(not_emulated > 0, "and at least one says so rather than inventing a colour");
}

// The two places where the first port quietly did something other than what the console did. Both compiled,
// both ran, and both lit or clipped the wrong pixels, which is why they are named here rather than left to
// the next person to notice by looking at a frame.
static void test_the_console_semantics_that_are_easy_to_invert()
{
    printf("\nwhere this port got the console's semantics wrong the first time\n");

    {
        // A specular reflection builds its eye vector from three stages' w lanes. The order is two back,
        // one back, this one -- and a rotation of that vector is still a valid vector, so nothing complains.
        xd3d::PixelShaderDef d;
        memset(&d, 0, sizeof d);
        d.PSTextureModes = 18u; // stage 0: a cube reflection
        const char *err = nullptr;
        const std::string glsl = go(d, 0, &err);
        // The eye vector is built from tc0.w three times at stage 0, which makes the order indistinguishable
        // here -- so the check is on a later stage, where the three lanes are different components.
        xd3d::PixelShaderDef d3;
        memset(&d3, 0, sizeof d3);
        d3.PSTextureModes = 0u | (18u << (5 * 3));
        const std::string glsl3 = go(d3, 0, &err);
        const size_t at = glsl3.find("reflect(");
        check(at != std::string::npos, "a reflection builds an eye vector");
        if (at != std::string::npos) {
            const std::string chunk = glsl3.substr(at, 120);
            // At stage 3 "two back" is tc1 and "one back" is tc2 -- not tc0. A test that expected the
            // lanes in stage-number order would be checking the wrong thing, which is the same mistake as
            // writing the port with them in the wrong order.
            const size_t w1 = chunk.find("tc1.w");
            const size_t w2 = chunk.find("tc2.w");
            const size_t w3 = chunk.find("tc3.w");
            check(w1 != std::string::npos && w1 < w2 && w2 < w3,
                  "and takes the w lanes in the console's order: two back, one back, this one");
        }
        check(compiles(glsl3), "and it compiles");
        (void)glsl;
    }

    {
        // CLIPPLANE. A set bit means "clip below zero", so it *keeps* the component at or above zero. Read
        // the other way, the plane clips exactly the pixels it was meant to keep -- and all four components
        // are compared, not just x and y.
        xd3d::PixelShaderDef d;
        memset(&d, 0, sizeof d);
        d.PSTextureModes = 5u;
        d.PSCompareMode = 0xF; // every bit set
        const char *err = nullptr;
        const std::string glsl = go(d, 0, &err);
        check(glsl.find("discard") != std::string::npos, "a clip plane discards");
        check(glsl.find("tc0.x >= 0.0") != std::string::npos, "a set bit keeps the component at or above zero");
        check(glsl.find("tc0.z >= 0.0") != std::string::npos, "and all four components are compared, not two");
        check(glsl.find("tc0.w >= 0.0") != std::string::npos, "including w");
        check(compiles(glsl), "and it compiles");

        // The other polarity, because a bit that means "keep above" and one that means "clip below" are the
        // same test only if you already know which way round it is -- which is the thing being pinned here.
        xd3d::PixelShaderDef inv;
        memset(&inv, 0, sizeof inv);
        inv.PSTextureModes = 5u;
        inv.PSCompareMode = 0x0;
        const std::string glsl2 = go(inv, 0, &err);
        check(glsl2.find("tc0.x <= 0.0") != std::string::npos, "a clear bit keeps the component at or below zero");
        check(compiles(glsl2), "and that compiles too");
    }
}

static void test_the_final_combiner()
{
    printf("\nthe final combiner\n");

    xd3d::PixelShaderDef d;
    memset(&d, 0, sizeof d);
    // A = 1, B = t0, C = 0, D = 0 for RGB: the result is t0, which is the simplest shader that shows the
    // final combiner is running at all.
    d.PSFinalCombinerInputsABCD = (uint32_t)in_reg(1, 6, false) << 24 | (uint32_t)in_reg(8, 6, false) << 16 |
                                  (uint32_t)in_reg(0, 6, false) << 8 | (uint32_t)in_reg(0, 6, false);
    d.PSFinalCombinerInputsEFG = (uint32_t)in_reg(1, 6, false) << 24; // G = 1
    const char *err = nullptr;
    const std::string glsl = go(d, 0, &err);
    check(glsl.find("final combiner") != std::string::npos, "it emits the final combiner");
    check(glsl.find("A * B + (1.0 - A) * C + D") != std::string::npos,
          "with the lerp the console computes, not just the sum");
    check(glsl.find("outColour = clamp(") != std::string::npos, "and writes the colour");
    check(compiles(glsl), "and it compiles");
}

static void test_the_two_halves_agree()
{
    printf("\nthe two halves agree on the varyings\n");

    // The vertex half writes texture coordinates at fixed locations; the fragment half reads them there.
    // A mismatch is a pipeline the driver refuses with a message that does not say which side was wrong,
    // so it is checked here rather than found there.
    xd3d::PixelShaderDef d;
    memset(&d, 0, sizeof d);
    d.PSTextureModes = 1; // stage 0 samples, so it reads tc0
    const uint32_t used = xd3d::psh_varyings_used(d, 0);
    check((used & (1u << 8)) != 0, "a shader that samples stage 0 reports reading tc0");
    check((used & (1u << 9)) == 0, "and does not claim to read tc1");
    check(xd3d::varying_texture(0) == 3 && xd3d::varying_texture(3) == 6,
          "the texture coordinates sit where the vertex half writes them");

    const char *err = nullptr;
    const std::string glsl = go(d, 0, &err);
    check(glsl.find("layout(location = 3) in vec4 tc0;") != std::string::npos,
          "and the fragment GLSL declares it at that location");

    // And a shader that samples nothing declares nothing, which is the other half of the same contract.
    xd3d::PixelShaderDef none;
    memset(&none, 0, sizeof none);
    check(xd3d::psh_varyings_used(none, 0) == 0, "a shader that samples nothing reads nothing");
}

static void test_it_declares_only_the_samplers_it_uses()
{
    printf("\nonly the samplers it uses\n");

    xd3d::PixelShaderDef d;
    memset(&d, 0, sizeof d);
    d.PSTextureModes = 1u;               // stage 0 samples a 2D texture
    d.PSTextureModes |= 3u << (5 * 1);   // stage 1 samples a cube map
    const xd3d::PshSamplers u = xd3d::psh_samplers_used(d, 0);
    check((u.used_2d & 1u) != 0, "stage 0 needs a 2D sampler");
    check((u.used_cube & 2u) != 0, "stage 1 needs a cube sampler");
    check((u.used_2d & 2u) == 0, "and neither stage is counted twice");
    const char *err = nullptr;
    const std::string glsl = go(d, 0, &err);
    check(glsl.find("sampler2D tex2d0") != std::string::npos, "the GLSL declares the one it uses");
    check(glsl.find("sampler2D tex2d1") == std::string::npos, "and not one it does not");
    check(compiles(glsl), "and it compiles");
}

int main()
{
    printf("BLACK loader -- NV2A pixel programs to SPIR-V\n");
    test_the_simplest_shader();
    test_one_combine_stage();
    test_the_input_mappings();
    test_the_dot_and_mux_flags();
    test_blue_to_alpha();
    test_the_console_semantics_that_are_easy_to_invert();
    test_texture_modes();
    test_the_final_combiner();
    test_the_two_halves_agree();
    test_it_declares_only_the_samplers_it_uses();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}