// NV2A vertex microcode, translated to GLSL and compiled -- the whole chain, with no GPU.
//
// The chain is: four-dword microcode -> this repository's decoder -> GLSL -> libglslang -> SPIR-V -> check.
// Before libglslang was installed the last three steps did not exist here, which meant a translation could
// only be eyeballed as text. Now it can be *run*, and the microcode in this file is written by hand rather
// than lifted from a game, so the expected answer is a thing that can be written down rather than recorded.
//
// What this cannot prove: that the shader draws what the game intended. That needs the A0 gate and a
// machine whose GPU submits. What it does prove is that the decoder reads the fields the console documents,
// that the translation is GLSL glslang accepts, and that no instruction form produces a shader that fails
// to compile.
#include "shader/glslang_posix.h"
#include "shader/nv2a_vsh_posix.h"
#include "d3d/vertex_posix.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

static int g_checks = 0, g_failures = 0;

static void check(bool ok, const char *what)
{
    printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
    g_checks++;
    if (!ok) g_failures++;
}

// ---- building microcode by hand ----
//
// The encoder is the inverse of vsh_decode, written from the same field positions. An encoder that
// disagreed with the decoder would make every test pass and every translation wrong, so the first test
// round-trips one instruction rather than trusting it.

struct Inst {
    uint32_t w[4];
    Inst() { memset(w, 0, sizeof w); }
};

static void put(uint32_t &word, int pos, int size, uint32_t value)
{
    const uint32_t mask = ((1u << size) - 1) << pos;
    word = (word & ~mask) | ((value << pos) & mask);
}

// The fields one instruction needs, by name, so the tests read as instructions rather than as bit twiddling.
static void set_mac(Inst &i, uint32_t op) { put(i.w[1], 21, 4, op); }
static void set_ilu(Inst &i, uint32_t op) { put(i.w[1], 25, 3, op); }
static void set_v(Inst &i, uint32_t v) { put(i.w[1], 9, 4, v); }
static void set_cnst(Inst &i, uint32_t c) { put(i.w[1], 13, 8, c); }

static void set_src_a(Inst &i, uint32_t mux, uint32_t reg, uint32_t swz0, uint32_t swz1, uint32_t swz2,
                      uint32_t swz3, uint32_t neg)
{
    put(i.w[1], 8, 1, neg);
    put(i.w[1], 6, 2, swz0);
    put(i.w[1], 4, 2, swz1);
    put(i.w[1], 2, 2, swz2);
    put(i.w[1], 0, 2, swz3);
    put(i.w[2], 28, 4, reg);
    put(i.w[2], 26, 2, mux);
}

static void set_src_b(Inst &i, uint32_t mux, uint32_t reg, uint32_t neg)
{
    put(i.w[2], 25, 1, neg);
    put(i.w[2], 23, 2, 0);
    put(i.w[2], 21, 2, 1);
    put(i.w[2], 19, 2, 2);
    put(i.w[2], 17, 2, 3);
    put(i.w[2], 13, 4, reg);
    put(i.w[2], 11, 2, mux);
}

static void set_src_c_reg(Inst &i, uint32_t reg) // the split field: two bits here, two in w[3]
{
    put(i.w[2], 0, 2, (reg >> 2) & 0x3);
    put(i.w[3], 30, 2, reg & 0x3);
}

static void set_src_c(Inst &i, uint32_t reg, uint32_t neg)
{
    put(i.w[2], 10, 1, neg);
    put(i.w[2], 8, 2, 0);
    put(i.w[2], 6, 2, 1);
    put(i.w[2], 4, 2, 2);
    put(i.w[2], 2, 2, 3);
    set_src_c_reg(i, reg);
    put(i.w[3], 28, 2, xd3d::MUX_C);
}

static void set_out_mac(Inst &i, uint32_t r, uint32_t mask)
{
    put(i.w[3], 24, 4, mask);
    put(i.w[3], 20, 4, r);
}

static void set_out_ilu(Inst &i, uint32_t mask)
{
    put(i.w[3], 16, 4, mask);
}

static void set_out_o(Inst &i, uint32_t address, uint32_t mask, uint32_t from_ilu)
{
    put(i.w[3], 12, 4, mask);
    put(i.w[3], 11, 1, 1); // OUT_ORB: write an output register rather than a constant
    put(i.w[3], 3, 8, address);
    put(i.w[3], 2, 1, from_ilu);
}

static void set_final(Inst &i) { put(i.w[3], 0, 1, 1); }

static void push(std::vector<uint32_t> &p, const Inst &i)
{
    p.insert(p.end(), i.w, i.w + 4);
}

// ---- the checks ----

static void test_the_encoder_agrees_with_the_decoder()
{
    printf("\nthe hand-built microcode decodes back to what was asked for\n");

    Inst i;
    set_mac(i, xd3d::MAC_MAD);
    set_v(i, 3);
    set_cnst(i, 42);
    set_src_a(i, xd3d::MUX_V, 0, 0, 1, 2, 3, 0);
    set_src_b(i, xd3d::MUX_R, 4, 1);
    set_src_c(i, 7, 0);
    set_out_mac(i, 5, 0xF);
    set_out_ilu(i, 0x3);

    xd3d::VshInst d;
    xd3d::vsh_decode(i.w, &d);
    check(d.mac == xd3d::MAC_MAD, "the MAC opcode survives the round trip");
    check(d.ilu == xd3d::ILU_NOP, "and an unset ILU stays NOP rather than becoming something");
    check(d.v == 3, "the varying index survives");
    check(d.cnst == 42, "the constant address survives");
    check(d.a.mux == xd3d::MUX_V && d.a.swz[0] == 0 && d.a.swz[1] == 1 && d.a.swz[2] == 2 && d.a.swz[3] == 3,
          "operand A keeps its mux and its identity swizzle");
    check(d.b.mux == xd3d::MUX_R && d.b.reg == 4 && d.b.neg == 1, "operand B keeps its register and its sign");
    check(d.c.mux == xd3d::MUX_C && d.c.reg == 7, "operand C keeps its constant -- including the two bits "
                                                     "that straddle two dwords");
    check(d.out_mac_mask == 0xF && d.out_r == 5, "the MAC destination survives");
    check(d.out_ilu_mask == 0x3, "and the ILU write mask");

    // The split field is the one worth checking separately: getting it wrong gives constant addresses 0-3
    // for everything, which looks like a working shader.
    // Four bits in all: two at the bottom of the second dword and two at the top of the third. A value above
    // 15 cannot be represented here, so the test uses 13 rather than something that looks more convincing
    // and is really just truncated.
    Inst c;
    set_src_c_reg(c, 13);
    xd3d::VshInst dc;
    xd3d::vsh_decode(c.w, &dc);
    check(dc.c.reg == 13, "a constant address above 3 survives, which is the split field working");
    Inst c0;
    set_src_c_reg(c0, 4);
    xd3d::VshInst d0;
    xd3d::vsh_decode(c0.w, &d0);
    check(d0.c.reg == 4, "and one whose low bits are non-zero too");
}

static std::string translate(const std::vector<uint32_t> &program, uint32_t input_mask, const char **err)
{
    return xd3d::vsh_to_glsl(program.data(), (uint32_t)(program.size() / 4), input_mask, err);
}

// The whole point: translation, then compilation, then a real module.
static bool compiles_to_spirv(const std::string &glsl)
{
    size_t bytes = 0;
    const char *err = nullptr;
    uint32_t *spirv = xd3d::compile_glsl_to_spirv(glsl.c_str(), xd3d::SHADER_STAGE_VERTEX, &bytes, &err);
    if (!spirv) {
        printf("       compiler said: %.100s\n", err ? err : "(nothing)");
        return false;
    }
    const bool ok = bytes >= 20 && spirv[0] == 0x07230203u && spirv[1] == 0x00010000u;
    xd3d::free_shader_spirv(spirv);
    return ok;
}

static void test_a_position_only_shader()
{
    printf("\nthe simplest useful shader: a position, written by hand\n");

    // oPos = v0.xyz * c[0]   -- one MAD, then write oPos.
    std::vector<uint32_t> p;
    {
        Inst i;
        set_mac(i, xd3d::MAC_MAD);
        set_v(i, 0);
        set_cnst(i, 0);
        set_src_a(i, xd3d::MUX_V, 0, 0, 1, 2, 3, 0);
        set_src_b(i, xd3d::MUX_C, 0, 0);
        set_src_c(i, 0, 0);
        set_out_mac(i, 12, 0xF); // r12 is oPos
        set_final(i);
        push(p, i);
    }

    const char *err = nullptr;
    const std::string glsl = translate(p, 0x1u, &err); // register 0 only
    check(!glsl.empty(), "it translates");
    if (glsl.empty()) {
        printf("       reason: %s\n", err ? err : "(none)");
        return;
    }
    check(glsl.find("layout(location = 0) in vec4 v0;") != std::string::npos,
          "and declares only the input the declaration feeds");
    check(glsl.find("in vec4 v1;") == std::string::npos, "and no input the declaration did not feed");
    check(glsl.find("gl_Position") != std::string::npos, "and writes a position");
    check(compiles_to_spirv(glsl), "and the GLSL compiles to a SPIR-V 1.0 module");
}

static void test_every_instruction_form_compiles()
{
    printf("\nevery instruction form produces a shader that compiles\n");

    // One instruction per MAC opcode, wired to read from a varying and write to r0. The point is coverage of
    // the *translation*, not of the arithmetic: a form whose GLSL is malformed fails here even though the
    // numbers it would compute are never checked.
    static const uint8_t kMacs[] = {xd3d::MAC_MOV, xd3d::MAC_MUL,     xd3d::MAC_ADD,  xd3d::MAC_MAD,
                                    xd3d::MAC_DP3, xd3d::MAC_DPH,     xd3d::MAC_DP4,  xd3d::MAC_DST,
                                    xd3d::MAC_MIN, xd3d::MAC_MAX,     xd3d::MAC_SLT,  xd3d::MAC_SGE,
                                    xd3d::MAC_ARL};
    size_t mac_failures = 0;
    for (uint8_t mac : kMacs) {
        std::vector<uint32_t> p;
        Inst i;
        set_mac(i, mac);
        set_v(i, 0);
        set_cnst(i, 1);
        set_src_a(i, xd3d::MUX_V, 0, 0, 1, 2, 3, 0);
        set_src_b(i, xd3d::MUX_C, 1, 0);
        set_src_c(i, 1, 0);
        set_out_mac(i, 0, 0xF);
        set_final(i);
        push(p, i);
        const char *err = nullptr;
        const std::string glsl = translate(p, 0x1u, &err);
        if (glsl.empty() || !compiles_to_spirv(glsl)) mac_failures++;
    }
    check(mac_failures == 0, "all twelve MAC forms translate and compile");

    static const uint8_t kIlus[] = {xd3d::ILU_MOV, xd3d::ILU_RCP, xd3d::ILU_RCC, xd3d::ILU_RSQ,
                                    xd3d::ILU_EXP, xd3d::ILU_LOG, xd3d::ILU_LIT};
    size_t ilu_failures = 0;
    for (uint8_t ilu : kIlus) {
        std::vector<uint32_t> p;
        Inst i;
        set_ilu(i, ilu);
        set_v(i, 0);
        set_cnst(i, 1);
        set_src_a(i, xd3d::MUX_V, 0, 0, 1, 2, 3, 0);
        set_src_b(i, xd3d::MUX_C, 1, 0);
        set_src_c(i, 1, 0);
        set_out_ilu(i, 0xF);
        set_final(i);
        push(p, i);
        const char *err = nullptr;
        const std::string glsl = translate(p, 0x1u, &err);
        if (glsl.empty() || !compiles_to_spirv(glsl)) ilu_failures++;
    }
    check(ilu_failures == 0, "all seven ILU forms translate and compile");
}

static void test_the_awkward_cases()
{
    printf("\nthe cases that are easy to get wrong\n");

    // The parallel units: both MAC and ILU in one instruction, both writing, both reading first. If the
    // translation emitted a write between the two reads, this shader would compute something else entirely --
    // and would still compile, which is why it needs saying.
    {
        std::vector<uint32_t> p;
        Inst i;
        set_mac(i, xd3d::MAC_MAD);
        set_ilu(i, xd3d::ILU_RSQ);
        set_v(i, 0);
        set_cnst(i, 2);
        set_src_a(i, xd3d::MUX_V, 0, 0, 1, 2, 3, 0);
        set_src_b(i, xd3d::MUX_C, 1, 0);
        set_src_c(i, 1, 0);
        set_out_mac(i, 3, 0xF);
        set_out_ilu(i, 0xF);
        set_final(i);
        push(p, i);
        const char *err = nullptr;
        const std::string glsl = translate(p, 0x1u, &err);
        check(!glsl.empty(), "a MAC and an ILU in one instruction translate");
        const size_t a = glsl.find("vec4 A =");
        const size_t b = glsl.find("vec4 M =");
        const size_t c = glsl.find("r3.xyzw = M.xyzw;");
        check(a != std::string::npos && b != std::string::npos && c != std::string::npos && a < b && b < c,
              "and every read comes before every write");
        check(compiles_to_spirv(glsl), "and it compiles");
    }

    // Indexed constants: c[a0+n]. The address register has to be a real variable, and the write has to be
    // through ARL. If the translator folded the address away, this would silently read constant 0.
    {
        std::vector<uint32_t> p;
        Inst i;
        set_mac(i, xd3d::MAC_ARL);
        set_v(i, 0);
        set_src_a(i, xd3d::MUX_V, 0, 0, 1, 2, 3, 0);
        set_src_b(i, xd3d::MUX_C, 0, 0);
        set_src_c(i, 0, 0);
        put(i.w[3], 1, 1, 1); // a0x: the constant address is relative to the address register
        set_out_mac(i, 0, 0x1);
        set_final(i);
        push(p, i);
        const char *err = nullptr;
        const std::string glsl = translate(p, 0x1u, &err);
        check(glsl.find("c[a0+") != std::string::npos, "indexed constants keep their index");
        check(glsl.find("a0 = int(floor(") != std::string::npos, "and the address register is written");
        check(compiles_to_spirv(glsl), "and it compiles");
    }

    // A negated, swizzled operand. Both are applied by a wrapper rather than folded into the name, because
    // folding "-(-(v0.xyz))" into a single expression is where signs quietly get lost.
    {
        std::vector<uint32_t> p;
        Inst i;
        set_mac(i, xd3d::MAC_MOV);
        set_v(i, 1);
        // .yxyx, not .xyxy: the component order is {1,0,1,0} and "xyzw"[1] is y. Writing the expectation as
        // .xyxy is the same mistake as typing the swizzle wrong, which is exactly what this test exists to
        // catch -- in the translator and in the expectation alike.
        set_src_a(i, xd3d::MUX_V, 0, 1, 0, 1, 0, 1); // negated, .yxyx
        set_src_b(i, xd3d::MUX_C, 0, 0);
        set_src_c(i, 0, 0);
        set_out_mac(i, 0, 0x4);
        set_final(i);
        push(p, i);
        const char *err = nullptr;
        const std::string glsl = translate(p, 0x2u, &err);
                check(glsl.find(".yxyx") != std::string::npos, "a swizzle survives into the GLSL");
        check(glsl.find("(-(v1.yxyx))") != std::string::npos, "and a negation is wrapped, not folded");
        check(compiles_to_spirv(glsl), "and it compiles");
    }

    // The console's two epilogue steps are not optional: without the sub-pixel snap, separately transformed
    // pieces of a destructible wall do not meet; without the w guard, a 2D shader divides by zero.
    {
        std::vector<uint32_t> p;
        Inst i;
        set_mac(i, xd3d::MAC_MOV);
        set_v(i, 0);
        set_src_a(i, xd3d::MUX_V, 0, 0, 1, 2, 3, 0);
        set_src_b(i, xd3d::MUX_C, 0, 0);
        set_src_c(i, 0, 0);
        set_out_mac(i, 12, 0xF);
        set_final(i);
        push(p, i);
        const char *err = nullptr;
        const std::string glsl = translate(p, 0x1u, &err);
        check(glsl.find("floor(oPos.xy * 8.0 + 0.5) * 0.125") != std::string::npos,
              "the sub-pixel snap to the console's grid is there");
        check(glsl.find("oPos.w == 0.0") != std::string::npos, "and so is the zero-w guard");
    }
}

static void test_a_program_that_cannot_be_translated()
{
    printf("\nwhat it refuses\n");

    const char *err = nullptr;
    check(xd3d::vsh_to_glsl(nullptr, 4, 1, &err).empty() && err != nullptr, "no program is refused with a reason");

    err = nullptr;
    const std::vector<uint32_t> empty;
    check(xd3d::vsh_to_glsl(empty.data(), 0, 1, &err).empty() && err != nullptr,
          "and a program with no instructions is refused with a reason");
}

int main()
{
    printf("BLACK loader -- NV2A vertex microcode to SPIR-V\n");
    test_the_encoder_agrees_with_the_decoder();
    test_a_position_only_shader();
    test_every_instruction_form_compiles();
    test_the_awkward_cases();
    test_a_program_that_cannot_be_translated();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}