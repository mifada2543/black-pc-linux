#include "shader/nv2a_psh_posix.h"

#include <stdio.h>
#include <string.h>

#include <string>

#include "shader/varyings.h"

namespace xd3d {
namespace {

// Where the cube samplers start in the pipeline's set of eight. Has to agree with gfx's descriptor set
// layout, which is eight combined image samplers in one binding.
constexpr uint32_t kCubeBindingBase = 4;


// What one texture mode needs. This exists so that the three things that must agree -- which varyings the
// shader declares, which samplers it declares, and what code it emits -- are answered from one switch.
//
// They were three switches, and they disagreed within a day: PASSTHRU and CLIPPLANE were thought to "sample
// no texture" and so had no coordinate declared, and both of them read one; and DOT_STR_3D emitted a cube
// sample without asking for a cube sampler. Neither failed loudly -- glslang reported an undeclared
// identifier, which is a good failure, but only because the shader was compiled at all.
struct ModeNeeds {
    bool implemented = false; // false = the console had a mode this port does not emulate
    bool reads_coord = false; // reads this stage's own texture coordinate
    bool reads_prev = false;  // reads the stage below's result instead
    bool cube = false;        // samples a cube map
    bool uses_bump = false;   // needs the bump matrix
    bool reads_w_lanes = false; // reads two earlier stages' w lanes, for a specular reflection's eye vector
};

ModeNeeds mode_needs(uint32_t mode, uint32_t tex_cube, uint32_t stage)
{
    ModeNeeds n;
    switch (mode) {
    case 0: break;                                                   // NONE: reads nothing
    case 1: case 2: n.implemented = true; n.reads_coord = true; n.cube = (tex_cube >> stage) & 1; break;
    case 3: n.implemented = true; n.reads_coord = true; n.cube = true; break;
    case 4: n.implemented = true; n.reads_coord = true; break;       // PASSTHRU: the coordinate *is* the colour
    case 5: n.implemented = true; n.reads_coord = true; break;       // CLIPPLANE: tests the coordinate
    case 6: case 7: n.implemented = true; n.reads_coord = true; n.uses_bump = true; break;
    case 9: case 10: n.implemented = true; n.reads_coord = true; n.reads_prev = true; break;
    case 15: case 16: n.implemented = true; n.reads_prev = true; break;
    case 17: n.implemented = true; n.reads_coord = true; n.reads_prev = true; break;
    case 11: case 13: case 14:
        n.implemented = true; n.reads_coord = true; n.reads_prev = true; n.cube = true; break;
    case 12: case 18:
        // The reflections read two *earlier* stages' coordinates as well, for the eye vector. Declaring only
        // this stage's left them undeclared, and glslang reported it.
        n.implemented = true;
        n.reads_coord = true;
        n.reads_prev = true;
        n.cube = true;
        n.reads_w_lanes = true;
        break;
    default: break;                                                  // BRDF and the rest: not emulated
    }
    return n;
}

// A register the combiner can name. Registers 14 (V1R0) and 15 (EF) exist only in the final combiner; a
// general combiner that names one reads zero, which is what returning the zero vector below does rather
// than the register's name -- the alternative compiles and then reads whatever the register held.
const char *reg_name(uint32_t reg)
{
    switch (reg) {
    case 1: return "C0";
    case 2: return "C1";
    case 3: return "FOGR";
    case 4: return "v0";
    case 5: return "v1";
    case 8: return "t0";
    case 9: return "t1";
    case 10: return "t2";
    case 11: return "t3";
    case 12: return "r0";
    case 13: return "r1";
    default: return "vec4(0)";
    }
}

// saturate, spelled out because GLSL has no such function. Clamped rather than wrapped, which is the whole
// point of it: an unclamped value in a colour register is a visible band.
std::string sat(const std::string &x) { return "clamp(" + x + ", 0.0, 1.0)"; }

// One 8-bit combiner input as GLSL. `rgb` selects the RGB portion; the alpha portion picks different
// components of the same register, which is why this cannot be a single expression.
//
// The mapping in the top three bits is the console's, and each is a different remapping rather than a scale:
// 0 is a clamp at zero, 1 is an inversion, 2 and 3 are signed remaps around 1.0, 4 and 5 shift by half, 6
// passes through, 7 negates. Reading them as "a scale" is how a combiner ends up subtly wrong everywhere.
std::string input(uint32_t b, bool rgb)
{
    uint32_t reg = b & 0xF;
    if (reg >= 14) reg = 0; // V1R0 and EF are not readable from a general combiner
    const bool alpha_chan = (b & 0x10) != 0;
    const uint32_t map = (b >> 5) & 7;
    const std::string r = reg_name(reg);
    std::string x;
    if (rgb) x = alpha_chan ? "(" + r + ").aaa" : "(" + r + ").rgb";
    else x = alpha_chan ? "(" + r + ").a" : "(" + r + ").b";
    switch (map) {
    case 0: return "max(" + x + ", 0.0)";
    case 1: return "(1.0 - " + sat(x) + ")";
    case 2: return "(2.0 * max(" + x + ", 0.0) - 1.0)";
    case 3: return "(1.0 - 2.0 * max(" + x + ", 0.0))";
    case 4: return "(max(" + x + ", 0.0) - 0.5)";
    case 5: return "(0.5 - max(" + x + ", 0.0))";
    case 6: return x;
    default: return "(-(" + x + "))";
    }
}

// The output mapping, which scales or shifts what the combiner produced before it is stored. The console
// clamps to [-1,1] afterwards regardless, and so does this.
std::string out_map(const std::string &x, uint32_t flags)
{
    switch (flags & 0x38) {
    case 0x08: return "((" + x + ") - 0.5)";
    case 0x10: return "((" + x + ") * 2.0)";
    case 0x18: return "(((" + x + ") - 0.5) * 2.0)";
    case 0x20: return "((" + x + ") * 4.0)";
    case 0x30: return "((" + x + ") * 0.5)";
    default: return x;
    }
}

// Which registers a combiner stage may write. The console's writable set: the two colour inputs, the two
// texture-coordinate inputs and both accumulators. Anything else named as a destination is dropped, which
// is what the console did -- writing to a constant or to a colour input would be a stage programming error
// the game does not make.
bool writable(uint32_t reg) { return reg == 4 || reg == 5 || (reg >= 8 && reg <= 13); }

// The final combiner's inputs, which are read differently: always saturated, and the mapping field means
// only "invert or not".
std::string final_input(uint32_t b, bool rgb)
{
    const uint32_t reg = b & 0xF;
    const bool alpha_chan = (b & 0x10) != 0;
    const bool invert = ((b >> 5) & 7) == 1;
    const std::string r = reg_name(reg);
    std::string x;
    if (rgb) x = alpha_chan ? "(" + r + ").aaa" : "(" + r + ").rgb";
    else x = alpha_chan ? "(" + r + ").a" : "(" + r + ").b";
    return invert ? "(1.0 - " + sat(x) + ")" : sat(x);
}

const char *sampler_2d(uint32_t stage) { return stage == 0 ? "tex2d0" : stage == 1 ? "tex2d1" : stage == 2 ? "tex2d2" : "tex2d3"; }
const char *sampler_cube(uint32_t stage) { return stage == 0 ? "cube0" : stage == 1 ? "cube1" : stage == 2 ? "cube2" : "cube3"; }

// The coordinate the console's stage samples with, and whether the dot mapping is signed. Decided once so
// every mode that needs it says the same thing -- getting the sign wrong here inverts every normal map.
struct Coord {
    const char *cube = nullptr;
    std::string tc;
    std::string nin; // the dot-mapping applied, ready to sample with
};

Coord coord_for(uint32_t stage, uint32_t in_stage, uint32_t dotmap, uint32_t tex_cube, const char *tc_name)
{
    Coord c;
    (void)stage;
    (void)tex_cube;
    c.tc = tc_name;
    const std::string tin = "t" + std::to_string(in_stage);
    c.nin = dotmap == 0 ? tin + ".rgb" : "((" + tin + ".rgb) * 2.0 - 1.0)";
    return c;
}

} // namespace

PshSamplers psh_samplers_used(const PixelShaderDef &d, uint32_t tex_cube)
{
    PshSamplers u;
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t mode = (d.PSTextureModes >> (5 * i)) & 0x1F;
        const ModeNeeds n = mode_needs(mode, tex_cube, i);
        if (!n.implemented) continue; // nothing is sampled, so nothing is bound
        if (n.cube) u.used_cube |= 1u << i;
        else u.used_2d |= 1u << i;
        if (n.uses_bump) u.needs_bump = true;
    }
    return u;
}

uint32_t psh_varyings_used(const PixelShaderDef &d, uint32_t tex_cube)
{
    // Which of the vertex half's outputs this shader actually reads. A stage that samples a texture reads
    // that stage's coordinate; a combiner that names v0 or v1 reads those.
    uint32_t used = 0;
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t mode = (d.PSTextureModes >> (5 * i)) & 0x1F;
        const ModeNeeds n = mode_needs(mode, tex_cube, i);
        if (n.reads_coord) used |= 1u << (8 + i);
        if (n.reads_w_lanes) {
            // The eye vector reaches two stages back. The console clamps those to stage 0, and so does this:
            // a reflection on the first or second stage reads its own coordinate twice rather than reading
            // coordinate minus one.
            if (i > 0) used |= 1u << (8 + (i - 1));
            if (i > 1) used |= 1u << (8 + (i - 2));
        }
    }
    // Every stage's inputs, not just the first: a combiner four stages in can still name v0 or v1, and the
    // first version of this scan looked only at stage 0, so a shader using them later declared nothing and
    // glslang reported an undeclared identifier.
    uint32_t stages = d.PSCombinerCount & 0xFF;
    if (stages > 8) stages = 8;
    for (uint32_t i = 0; i < stages; i++) {
        const uint32_t words[2] = {d.PSRGBInputs[i], d.PSAlphaInputs[i]};
        for (uint32_t k = 0; k < 2; k++)
            for (uint32_t b = 0; b < 4; b++) {
                const uint32_t reg = (words[k] >> (24 - 8 * b)) & 0xF;
                if (reg == 4) used |= 1u << 4; // v0
                if (reg == 5) used |= 1u << 5; // v1
            }
    }
    // The final combiner reads v1 unconditionally: V1R0 is built from v1 and r0 whichever of the two
    // invert flags is set, because both halves are expressions that mention it.
    if (d.PSFinalCombinerInputsABCD || d.PSFinalCombinerInputsEFG) used |= 1u << 5;
    (void)tex_cube;
    return used;
}

std::string psh_to_glsl(const PixelShaderDef &d, uint32_t tex_cube, const char **err)
{
    if (err) *err = nullptr;

    const PshSamplers sam = psh_samplers_used(d, tex_cube);

    std::string s;
    char b[512];
    s += "#version 450\n";
    s += "// Translated from an Xbox NV2A pixel program (D3DPIXELSHADERDEF). The register combiners are\n"
         "// reproduced literally, including the separate RGB and alpha paths, because that is what the\n"
         "// hardware did -- see src/shader/nv2a_psh_posix.cpp.\n";
    s += "layout(std140, set = 0, binding = 9) uniform Consts {\n";
    s += "  vec4 pc[18];\n";
    s += "  vec4 fogColor;\n";
    s += "  vec4 bumpMat[4];\n";
    s += "};  // no instance name: the operands below name these directly\n";
    // Only the samplers this program reads. A sampler declared and never written is a descriptor that reads
    // undefined, and one bound and never declared is a descriptor the driver cannot account for.
    // The 2D and cube samplers for one stage must not claim the same binding. The pipeline layout has eight
    // combined image samplers in one set, so the two kinds get disjoint halves: 0-3 for 2D and 4-7 for
    // cube. Sharing a number compiles when only one kind is used and fails at pipeline creation when both
    // are -- which is to say, it fails late.
    for (uint32_t i = 0; i < 4; i++) {
        if (sam.used_2d & (1u << i)) {
            snprintf(b, sizeof b, "layout(binding = %u) uniform sampler2D %s;\n", i, sampler_2d(i));
            s += b;
        }
    }
    for (uint32_t i = 0; i < 4; i++) {
        if (sam.used_cube & (1u << i)) {
            snprintf(b, sizeof b, "layout(binding = %u) uniform samplerCube %s;\n",
                     kCubeBindingBase + i, sampler_cube(i));
            s += b;
        }
    }

    // Inputs, at the shared locations. Only what is read.
    const uint32_t vars = psh_varyings_used(d, tex_cube);
    if (vars & (1u << 4)) s += "layout(location = 1) in vec4 v0;\n";
    if (vars & (1u << 5)) s += "layout(location = 2) in vec4 v1;\n";
    for (uint32_t i = 0; i < 4; i++)
        if (vars & (1u << (8 + i))) {
            snprintf(b, sizeof b, "layout(location = %u) in vec4 tc%u;\n", (unsigned)varying_texture(i), i);
            s += b;
        }
    s += "layout(location = 0) out vec4 outColour;\n";
    s += "void main() {\n";
    // A stage that reads nothing still needs the name to exist if a later stage samples its result.
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t mode = (d.PSTextureModes >> (5 * i)) & 0x1F;
        snprintf(b, sizeof b, "  vec4 t%u = vec4(0);\n", i);
        s += b;
        (void)mode;
    }
    s += "  float d0 = 0.0, d1 = 0.0, d2 = 0.0, d3 = 0.0;\n";
    s += "  vec4 FOGR = vec4(fogColor.rgb, 1.0);\n";

    // ---- texture stages ----
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t mode = (d.PSTextureModes >> (5 * i)) & 0x1F;
        // Which earlier stage's result this one samples. The console's rule differs per mode, and clamping
        // it to the stage below is what stops stage 0 reading a register nothing has written.
        uint32_t in_stage = 0;
        if (i == 2) in_stage = (d.PSInputTexture >> 16) & 1;
        if (i == 3) in_stage = (d.PSInputTexture >> 20) & 3;
        if (i == 1) in_stage = 0;
        if (i > 0 && in_stage >= i) in_stage = i - 1;
        const uint32_t dotmap = i ? (d.PSDotMapping >> (4 * (i - 1))) & 7 : 0;
        char tc_name[16];
        snprintf(tc_name, sizeof tc_name, "tc%u", i);
        const Coord co = coord_for(i, in_stage, dotmap, tex_cube, tc_name);
        const std::string tin = "t" + std::to_string(in_stage);
        const bool cube = (tex_cube >> i) & 1;

        switch (mode) {
        case 0:
            break; // NONE: the console samples nothing and the register holds what it held
        case 1: case 2: { // PROJECT2D / PROJECT3D
            // The w guard is the console's: a projected sample with w of zero samples the whole texture, so
            // the guard turns that into a single point rather than a smear.
            if (cube) {
                snprintf(b, sizeof b, "  t%u = texture(%s, %s.xyz);\n", i, sampler_cube(i), tc_name);
            } else {
                snprintf(b, sizeof b,
                         "  t%u = textureProj(%s, vec4(%s.xy, 0.0, %s.w == 0.0 ? 1.0 : %s.w));\n", i,
                         sampler_2d(i), tc_name, tc_name, tc_name);
            }
            s += b;
            break;
        }
        case 3: // CUBEMAP
            snprintf(b, sizeof b, "  t%u = texture(%s, %s.xyz);\n", i, sampler_cube(i), tc_name);
            s += b;
            break;
        case 4: // PASSTHRU: the coordinate is the colour
            // sat() returns an *expression*, so it is not called here: writing sat(tc)(tc) would compile to
            // a call on the result of the clamp, which is what the first version of this line did.
            snprintf(b, sizeof b, "  t%u = %s;\n", i, sat(tc_name).c_str());
            s += b;
            break;
        case 5: { // CLIPPLANE: discard the pixel when a coordinate fails the compare
            const uint32_t cmp = (d.PSCompareMode >> (4 * i)) & 0xF;
            std::string cond;
            // Four bits, one per component, and all four are compared -- the first port tested only x and y,
            // on the reasoning that the others were padding. They are not, and the console emitted all four.
            //
            // The polarity is the console's too, and it is the other way round from what one expects: a set
            // bit means "clip when the component goes below zero", which is `clip(x)` rather than `clip(-x)`,
            // so a set bit *keeps* x at or above zero. Reading it the other way clips exactly the pixels
            // the plane was meant to keep.
            for (uint32_t c = 0; c < 4; c++) {
                const char comps[5] = "xyzw";
                const std::string term = std::string("(") + tc_name + "." + comps[c] +
                                         (((cmp >> c) & 1) ? " >= 0.0" : " <= 0.0") + ")";
                cond += cond.empty() ? term : " && " + term;
            }
            if (!cond.empty()) {
                snprintf(b, sizeof b, "  if (!(%s)) discard;\n", cond.c_str());
                s += b;
            }
            break;
        }
        case 6: case 7: { // BUMPENVMAP / _LUM
            snprintf(b, sizeof b,
                     "  { vec2 du = %s.rg * 2.0 - 1.0; vec2 o = vec2(dot(du, bumpMat[%u].xz), dot(du, "
                     "bumpMat[%u].yw));\n    t%u = texture(%s, %s.xy + o).xxxx; }\n",
                     tin.c_str(), i, i, i, sampler_2d(i), tc_name);
            s += b;
            break;
        }
        case 15: // DPNDNT_AR
            snprintf(b, sizeof b, "  t%u = texture(%s, %s.ar).xxxx;\n", i, sampler_2d(i), tin.c_str());
            s += b;
            break;
        case 16: // DPNDNT_GB
            snprintf(b, sizeof b, "  t%u = texture(%s, %s.gb).xxxx;\n", i, sampler_2d(i), tin.c_str());
            s += b;
            break;
        case 17: // DOTPRODUCT: keep the dot for the next stage
            snprintf(b, sizeof b, "  d%u = dot(%s.xyz, %s); t%u = vec4(d%u);\n", i, tc_name, co.nin.c_str(), i, i);
            s += b;
            break;
        case 9: // DOT_ST: this stage's dot, looked up through the previous stage's texture
            snprintf(b, sizeof b, "  d%u = dot(%s.xyz, %s); t%u = texture(%s, vec2(d%u, d%u)).xxxx;\n", i,
                     tc_name, co.nin.c_str(), i, sampler_2d(i), i, i > 0 ? i - 1 : 0);
            s += b;
            break;
        case 10: // DOT_ZW: depth replace, which this port does not emulate; the dot is passed through
            snprintf(b, sizeof b, "  d%u = dot(%s.xyz, %s); t%u = vec4(d%u);\n", i, tc_name, co.nin.c_str(), i, i);
            s += b;
            break;
        case 13: case 14: case 11: case 12: case 18: { // three dots form a vector, optionally reflected
            snprintf(b, sizeof b, "  d%u = dot(%s.xyz, %s);\n", i, tc_name, co.nin.c_str());
            s += b;
            std::string n = "vec3(d" + std::to_string(i > 1 ? i - 2 : 0) + ", d" + std::to_string(i > 0 ? i - 1 : 0) +
                            ", d" + std::to_string(i) + ")";
            if (mode == 12 || mode == 18) { // a specular reflection: the eye vector comes from the w lanes
                // In this order: two stages back, one back, then this one. The first port wrote it as this
                // one, two back, one back -- a rotation of the eye vector, which still compiles and still
                // runs, and lights every reflective surface from the wrong direction.
                const std::string e = "vec3(tc" + std::to_string(i > 1 ? i - 2 : 0) + ".w, tc" +
                                      std::to_string(i > 0 ? i - 1 : 0) + ".w, " + tc_name + ".w)";
                n = "reflect(-(" + e + "), normalize(" + n + "))";
            }
            snprintf(b, sizeof b, "  t%u = texture(%s, %s).xxxx;\n", i, sampler_cube(i), n.c_str());
            s += b;
            break;
        }
        default:
            // Said rather than silently wrong. A mode that produced a plausible colour would be much harder
            // to trace than one that is obviously not emulated.
            snprintf(b, sizeof b, "  t%u = vec4(0); // texture mode %u is not emulated\n", i, mode);
            s += b;
            break;
        }
    }

    // r0.a starts as t0.a on the console. That is not a detail: it is where the very first combiner's
    // alpha comes from for any shader that does not set it explicitly.
    s += "  vec4 r0 = vec4(0.0, 0.0, 0.0, t0.a), r1 = vec4(0);\n";

    // ---- general combiners ----
    uint32_t stages = d.PSCombinerCount & 0xFF;
    if (stages > 8) stages = 8;
    const bool unique_c0 = (d.PSCombinerCount & 0x1000) != 0;
    const bool unique_c1 = (d.PSCombinerCount & 0x10000) != 0;
    // The AB/CD mux. The two encodings are different tests, not different spellings of one: the high bit
    // tests the value against half, the low bit tests the eighth bit of the 8-bit alpha the console kept.
    const std::string mux = (d.PSCombinerCount & 0x100)
                                ? "(r0.a >= 0.5)"
                                : "(mod(floor(" + sat("r0.a") + " * 255.0 + 0.5), 2.0) >= 1.0)";

    for (uint32_t i = 0; i < stages; i++) {
        snprintf(b, sizeof b, "  { // combiner stage %u\n    vec4 C0 = pc[%u], C1 = pc[%u];\n", i, unique_c0 ? i : 0,
                 8 + (unique_c1 ? i : 0));
        s += b;
        const uint32_t ri = d.PSRGBInputs[i], ro = d.PSRGBOutputs[i];
        const uint32_t ai = d.PSAlphaInputs[i], ao = d.PSAlphaOutputs[i];
        const uint32_t rflags = ro >> 12, aflags = ao >> 12;

        s += "    vec3 A = " + input(ri >> 24, true) + ", B = " + input((ri >> 16) & 0xFF, true) + ", C = " +
             input((ri >> 8) & 0xFF, true) + ", D = " + input(ri & 0xFF, true) + ";\n";
        s += std::string("    vec3 ab = ") + ((rflags & 2) ? "vec3(dot(A, B))" : "(A * B)") + ";\n";
        s += std::string("    vec3 cd = ") + ((rflags & 1) ? "vec3(dot(C, D))" : "(C * D)") + ";\n";
        s += std::string("    vec3 sm = ") +
             ((rflags & 4) ? ("(" + mux + " ? cd : ab)") : std::string("(ab + cd)")) + ";\n";
        s += "    ab = clamp(" + out_map("ab", rflags) + ", -1.0, 1.0); cd = clamp(" + out_map("cd", rflags) +
             ", -1.0, 1.0); sm = clamp(" + out_map("sm", rflags) + ", -1.0, 1.0);\n";

        s += "    float Aa = " + input(ai >> 24, false) + ", Ba = " + input((ai >> 16) & 0xFF, false) +
             ", Ca = " + input((ai >> 8) & 0xFF, false) + ", Da = " + input(ai & 0xFF, false) + ";\n";
        s += "    float aba = Aa * Ba, cda = Ca * Da;\n";
        s += std::string("    float sma = ") +
             ((aflags & 4) ? ("(" + mux + " ? cda : aba)") : std::string("(aba + cda)")) + ";\n";
        s += "    aba = clamp(" + out_map("aba", aflags) + ", -1.0, 1.0); cda = clamp(" + out_map("cda", aflags) +
             ", -1.0, 1.0); sma = clamp(" + out_map("sma", aflags) + ", -1.0, 1.0);\n";

        // Every read is done; every write follows. Writing as each value was produced would let a stage read
        // back a register an earlier write in the same stage had just changed.
        const uint32_t r_cd = ro & 0xF, r_ab = (ro >> 4) & 0xF, r_sum = (ro >> 8) & 0xF;
        const uint32_t a_cd = ao & 0xF, a_ab = (ao >> 4) & 0xF, a_sum = (ao >> 8) & 0xF;
        if (writable(r_ab)) s += std::string("    ") + reg_name(r_ab) + ".rgb = ab;\n";
        if (writable(r_cd)) s += std::string("    ") + reg_name(r_cd) + ".rgb = cd;\n";
        if (writable(r_sum)) s += std::string("    ") + reg_name(r_sum) + ".rgb = sm;\n";
        if (writable(a_ab)) s += std::string("    ") + reg_name(a_ab) + ".a = aba;\n";
        if (writable(a_cd)) s += std::string("    ") + reg_name(a_cd) + ".a = cda;\n";
        if (writable(a_sum)) s += std::string("    ") + reg_name(a_sum) + ".a = sma;\n";
        // Blue to alpha is a separate write, not part of the RGB one. Dropping it changes the alpha of
        // every shader that uses it, quietly.
        if ((rflags & 0x80) && writable(r_ab)) s += std::string("    ") + reg_name(r_ab) + ".a = ab.b;\n";
        if ((rflags & 0x40) && writable(r_cd)) s += std::string("    ") + reg_name(r_cd) + ".a = cd.b;\n";
        s += "  }\n";
    }

    // ---- final combiner ----
    if (d.PSFinalCombinerInputsABCD || d.PSFinalCombinerInputsEFG) {
        const uint32_t abcd = d.PSFinalCombinerInputsABCD, efg = d.PSFinalCombinerInputsEFG;
        const uint32_t flags = efg & 0xFF;
        s += "  { // final combiner\n    vec4 C0 = pc[16], C1 = pc[17];\n";
        snprintf(b, sizeof b, "    vec4 V1R0 = vec4(%s + %s, 0.0);\n", (flags & 0x40) ? "(1.0 - v1.rgb)" : "v1.rgb",
                 (flags & 0x20) ? "(1.0 - r0.rgb)" : "r0.rgb");
        s += b;
        if (flags & 0x80) s += "    V1R0 = " + sat("V1R0") + ";\n";
        s += "    vec4 EF = vec4(" + final_input(efg >> 24, true) + " * " + final_input((efg >> 16) & 0xFF, true) +
             ", 0.0);\n";
        s += "    vec3 A = " + final_input(abcd >> 24, true) + ", B = " + final_input((abcd >> 16) & 0xFF, true) +
             ", C = " + final_input((abcd >> 8) & 0xFF, true) + ", D = " + final_input(abcd & 0xFF, true) + ";\n";
        s += "    float G = " + final_input((efg >> 8) & 0xFF, false) + ";\n";
        // A * B + (1 - A) * C + D. The lerp between B and C by A is the whole of what the final combiner
        // does to colour; the alpha lane is computed separately by G.
        s += "    outColour = " + sat("vec4((A * B + (1.0 - A) * C + D), G)") + ";\n";
        s += "    return;\n";
        s += "  }\n";
    }
    s += "  outColour = " + sat("r0") + ";\n";
    s += "}\n";
    return s;
}

} // namespace xd3d