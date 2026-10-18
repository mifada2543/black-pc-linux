#include "shader/nv2a_vsh_posix.h"

#include "shader/varyings.h"

#include <stdio.h>
#include <string.h>

#include <string>

namespace xd3d {
namespace {

// A field of `size` bits at `pos` in one dword. The positions come from the public NV2A documentation and
// are the whole of the decoder: there is nothing to derive them from, so they are written down.
uint32_t field(const uint32_t *w, int dword, int pos, int size)
{
    return (w[dword] >> pos) & ((1u << size) - 1);
}

const char kComp[] = "xyzw";

// ---- the little textual helpers, shared by the disassembly and the translation ----

// The invoice has a disassembler alongside this translator, and these two helpers are its. They are not
// carried over: the disassembler is not part of what A5 needs, and unused code in a translation unit is a
// thing that later reads as "someone meant to call this". The equivalents below (glsl_mask, and the swizzle
// inside glsl_src) are the ones actually used.

// The console's output register a name. Returning null for an address outside the list is how the
// translator declines to write something rather than inventing a name for it.
const char *out_name(uint8_t a)
{
    switch (a) {
    case VSH_OPOS: return "oPos";
    case VSH_OD0: return "oD0";
    case VSH_OD1: return "oD1";
    case VSH_OFOG: return "oFog";
    case VSH_OPTS: return "oPts";
    case VSH_OB0: return "oB0";
    case VSH_OB1: return "oB1";
    case VSH_OT0: return "oT0";
    default: return nullptr;
    }
}

// The names of the constant registers the translator reads by name. The console's fixed-function code
// writes these, so they are addresses in the bank rather than things this file decides.
const char *const kBank[VSH_CONST_BANK] = {
    "c[0]", "c[1]", "c[2]", "c[3]", "c[4]", "c[5]", "c[6]", "c[7]", "c[8]", "c[9]", "c[10]", "c[11]", "c[12]",
    "c[13]", "c[14]", "c[15]", "c[16]", "c[17]", "c[18]", "c[19]", "c[20]", "c[21]", "c[22]", "c[23]", "c[24]",
    "c[25]", "c[26]", "c[27]", "c[28]", "c[29]", "c[30]", "c[31]", "c[32]", "c[33]", "c[34]", "c[35]", "c[36]",
    "c[37]", "c[38]", "c[39]", "c[40]", "c[41]", "c[42]", "c[43]", "c[44]", "c[45]", "c[46]", "c[47]", "c[48]",
    "c[49]", "c[50]", "c[51]", "c[52]", "c[53]", "c[54]", "c[55]", "c[56]", "c[57]", "c[58]", "c[59]", "c[60]",
    "c[61]", "c[62]", "c[63]", "c[64]", "c[65]", "c[66]", "c[67]", "c[68]", "c[69]", "c[70]", "c[71]", "c[72]",
    "c[73]", "c[74]", "c[75]", "c[76]", "c[77]", "c[78]", "c[79]", "c[80]", "c[81]", "c[82]", "c[83]", "c[84]",
    "c[85]", "c[86]", "c[87]", "c[88]", "c[89]", "c[90]", "c[91]", "c[92]", "c[93]", "c[94]", "c[95]", "c[96]",
    "c[97]", "c[98]", "c[99]", "c[100]", "c[101]", "c[102]", "c[103]", "c[104]", "c[105]", "c[106]", "c[107]",
    "c[108]", "c[109]", "c[110]", "c[111]", "c[112]", "c[113]", "c[114]", "c[115]", "c[116]", "c[117]",
    "c[118]", "c[119]", "c[120]", "c[121]", "c[122]", "c[123]", "c[124]", "c[125]", "c[126]", "c[127]",
    "c[128]", "c[129]", "c[130]", "c[131]", "c[132]", "c[133]", "c[134]", "c[135]", "c[136]", "c[137]",
    "c[138]", "c[139]", "c[140]", "c[141]", "c[142]", "c[143]", "c[144]", "c[145]", "c[146]", "c[147]",
    "c[148]", "c[149]", "c[150]", "c[151]", "c[152]", "c[153]", "c[154]", "c[155]", "c[156]", "c[157]",
    "c[158]", "c[159]", "c[160]", "c[161]", "c[162]", "c[163]", "c[164]", "c[165]", "c[166]", "c[167]",
    "c[168]", "c[169]", "c[170]", "c[171]", "c[172]", "c[173]", "c[174]", "c[175]", "c[176]", "c[177]",
    "c[178]", "c[179]", "c[180]", "c[181]", "c[182]", "c[183]", "c[184]", "c[185]", "c[186]", "c[187]",
    "c[188]", "c[189]", "c[190]", "c[191]",
    // The fixed function's registers. The texture scale is an array of four, so those four entries are
    // written out rather than generated: a loop that builds the text would be shorter and would put a
    // number's spelling in two places.
    "c[192]", "c[193]", "c[194]", "c[195]", "c[196]", "c[197]", "c[198]", "c[199]", "c[200]",
};

// One operand as GLSL. The three multiplexers are the console's: a temporary register, an input varying,
// or the constant bank. `a0x` is the console's indexed-constant mode, where the constant address is
// relative to the address register, and that is why the address register has to be a real variable here
// rather than a compile-time constant.
std::string glsl_src(const VshInst &in, const VshSrc &s)
{
    std::string r;
    switch (s.mux) {
    case MUX_R:
        if (s.reg == 12) r = "oPos"; // r12 is the position output
        else {
            char b[16];
            snprintf(b, sizeof b, "r%u", s.reg);
            r = b;
        }
        break;
    case MUX_V: {
        char b[16];
        snprintf(b, sizeof b, "v%u", in.v);
        r = b;
        break;
    }
    case MUX_C:
        if (in.a0x) {
            char b[24];
            snprintf(b, sizeof b, "c[a0+%u]", in.cnst);
            r = b;
        } else {
            r = in.cnst < VSH_CONST_BANK ? kBank[in.cnst] : "c[0]";
        }
        break;
    default:
        r = "vec4(0,0,0,0)";
        break;
    }
    if (!(s.swz[0] == 0 && s.swz[1] == 1 && s.swz[2] == 2 && s.swz[3] == 3)) {
        r += '.';
        for (int i = 0; i < 4; i++) r += kComp[s.swz[i]];
    }
    return s.neg ? "(-(" + r + "))" : r;
}

// The write mask, as a swizzle. Zero is impossible -- the caller checks the mask before asking -- so there
// is no "what should this be" case to answer here.
std::string glsl_mask(uint8_t m)
{
    std::string r;
    for (int i = 0; i < 4; i++)
        if (m & (8 >> i)) r += kComp[i];
    return r;
}

} // namespace

void vsh_decode(const uint32_t *w, VshInst *o)
{
    o->ilu = (uint8_t)field(w, 1, 25, 3);
    o->mac = (uint8_t)field(w, 1, 21, 4);
    o->cnst = (uint8_t)field(w, 1, 13, 8);
    o->v = (uint8_t)field(w, 1, 9, 4);
    o->a.neg = (uint8_t)field(w, 1, 8, 1);
    o->a.swz[0] = (uint8_t)field(w, 1, 6, 2);
    o->a.swz[1] = (uint8_t)field(w, 1, 4, 2);
    o->a.swz[2] = (uint8_t)field(w, 1, 2, 2);
    o->a.swz[3] = (uint8_t)field(w, 1, 0, 2);
    o->a.reg = (uint8_t)field(w, 2, 28, 4);
    o->a.mux = (uint8_t)field(w, 2, 26, 2);
    o->b.neg = (uint8_t)field(w, 2, 25, 1);
    o->b.swz[0] = (uint8_t)field(w, 2, 23, 2);
    o->b.swz[1] = (uint8_t)field(w, 2, 21, 2);
    o->b.swz[2] = (uint8_t)field(w, 2, 19, 2);
    o->b.swz[3] = (uint8_t)field(w, 2, 17, 2);
    o->b.reg = (uint8_t)field(w, 2, 13, 4);
    o->b.mux = (uint8_t)field(w, 2, 11, 2);
    o->c.neg = (uint8_t)field(w, 2, 10, 1);
    o->c.swz[0] = (uint8_t)field(w, 2, 8, 2);
    o->c.swz[1] = (uint8_t)field(w, 2, 6, 2);
    o->c.swz[2] = (uint8_t)field(w, 2, 4, 2);
    o->c.swz[3] = (uint8_t)field(w, 2, 2, 2);
    // The constant address straddles two dwords: two bits at the bottom of the second and two at the top of
    // the third. Splitting it wrongly gives constants 0-3 for everything, which looks plausible and is not.
    o->c.reg = (uint8_t)((field(w, 2, 0, 2) << 2) | field(w, 3, 30, 2));
    o->c.mux = (uint8_t)field(w, 3, 28, 2);
    o->out_mac_mask = (uint8_t)field(w, 3, 24, 4);
    o->out_r = (uint8_t)field(w, 3, 20, 4);
    o->out_ilu_mask = (uint8_t)field(w, 3, 16, 4);
    o->out_o_mask = (uint8_t)field(w, 3, 12, 4);
    o->out_orb = (uint8_t)field(w, 3, 11, 1);
    o->out_address = (uint8_t)field(w, 3, 3, 8);
    o->out_mux = (uint8_t)field(w, 3, 2, 1);
    o->a0x = (uint8_t)field(w, 3, 1, 1);
    o->final = (uint8_t)field(w, 3, 0, 1);
}

VshOutputs vsh_outputs(const uint32_t *program, uint32_t instruction_count)
{
    VshOutputs o;
    o.t0 = o.t1 = o.t2 = o.t3 = false;
    for (uint32_t i = 0; i < instruction_count; i++) {
        VshInst in;
        vsh_decode(program + i * 4, &in);
        if (in.out_o_mask && in.out_orb) {
            const char *n = out_name(in.out_address);
            if (n) {
                if (n[1] == 'D') {
                    if (in.out_address == VSH_OD0) o.d0 = true;
                    if (in.out_address == VSH_OD1) o.d1 = true;
                } else if (n[1] == 'T') {
                    const uint32_t t = in.out_address - VSH_OT0;
                    if (t < 4) (&o.t0)[t] = true;
                } else if (in.out_address == VSH_OFOG) {
                    o.fog = true;
                } else if (in.out_address == VSH_OPTS) {
                    o.pts = true;
                } else if (in.out_address == VSH_OPOS) {
                    o.pos = true;
                } else if (in.out_address == VSH_OB0 || in.out_address == VSH_OB1) {
                    // Blink and fog colours: the console had them, the game here does not read them, and
                    // carrying them through would mean inventing varyings nothing consumes.
                }
            }
        }
        if (in.final) break;
    }
    return o;
}

std::string vsh_to_glsl(const uint32_t *program, uint32_t instruction_count, uint32_t input_mask, const char **err)
{
    if (err) *err = nullptr;
    if (!program) {
        if (err) *err = "no program to translate";
        return "";
    }
    if (instruction_count == 0) {
        // A program with no instructions is not a vertex shader. The program header's count is where this
        // normally gets caught (see read_program_header), so getting here means the caller bypassed it.
        if (err) *err = "a program with no instructions in it";
        return "";
    }

    const VshOutputs outs = vsh_outputs(program, instruction_count);

    std::string s;
    s += "#version 450\n";
    s += "// Translated from NV2A vertex microcode. See src/shader/nv2a_vsh_posix.cpp for why each step is\n"
         "// written the way it is; the arithmetic is the console's, not a preference.\n";
    s += "layout(std140, binding = 0) uniform Consts {\n";
    // The texture scale is one array of four in the console's bank (194..197), written as four scalars here
    // so the D3D layer's job -- filling c194..c197 -- maps onto plain named members.
    s += "  vec4 c[198];\n";
    s += "  vec4 texScale0, texScale1, texScale2, texScale3;\n";
    s += "  vec4 fogParams;\n";
    s += "  vec4 fogMode;\n";
    s += "  vec4 vmScale;\n";
    s += "};  // no instance name: the members are the global names the operands use\n";

    // Inputs. Only the registers the declaration actually feeds get a location, because a varying with no
    // producer is a pipeline the driver rejects, and one the game never asked for.
    for (uint32_t i = 0; i < 16; i++)
        if (input_mask & (1u << i)) {
            char b[64];
            snprintf(b, sizeof b, "layout(location = %u) in vec4 v%u;\n", i, i);
            s += b;
        }

    // Outputs, numbered in the order the fragment side is told to expect them. gl_Position is *not*
    // declared: it is built into GLSL, and redeclaring it is a compile error about a reserved identifier.
    char b[64];
    if (outs.d0) {
        snprintf(b, sizeof b, "layout(location = %u) out vec4 d0;\n", (unsigned)VARYING_D0);
        s += b;
    }
    if (outs.d1) {
        snprintf(b, sizeof b, "layout(location = %u) out vec4 d1;\n", (unsigned)VARYING_D1);
        s += b;
    }
    // The texture coordinates. These were missing entirely when only the vertex half existed: the console's
    // texel-scale multiply happened, and the result was then dropped, because there was no fragment half to
    // read it. The locations are the shared ones, so the fragment translator can ask for them by name.
    if (outs.t0) {
        snprintf(b, sizeof b, "layout(location = %u) out vec4 t0;\n", (unsigned)varying_texture(0));
        s += b;
    }
    if (outs.t1) {
        snprintf(b, sizeof b, "layout(location = %u) out vec4 t1;\n", (unsigned)varying_texture(1));
        s += b;
    }
    if (outs.t2) {
        snprintf(b, sizeof b, "layout(location = %u) out vec4 t2;\n", (unsigned)varying_texture(2));
        s += b;
    }
    if (outs.t3) {
        snprintf(b, sizeof b, "layout(location = %u) out vec4 t3;\n", (unsigned)varying_texture(3));
        s += b;
    }
    if (outs.fog) {
        snprintf(b, sizeof b, "layout(location = %u) out float fog;\n", (unsigned)VARYING_FOG);
        s += b;
    }
    if (outs.pts) {
        snprintf(b, sizeof b, "layout(location = %u) out float pts;\n", (unsigned)VARYING_PTS);
        s += b;
    }

    s += "void main() {\n";
    for (uint32_t i = 0; i < 16; i++) {
        if (input_mask & (1u << i)) {
            snprintf(b, sizeof b, "  vec4 v%u_ = v%u;\n", i, i);
            s += b;
        }
    }
    s += "  vec4 r0=vec4(0),r1=vec4(0),r2=vec4(0),r3=vec4(0),r4=vec4(0),r5=vec4(0),r6=vec4(0),"
         "r7=vec4(0),r8=vec4(0),r9=vec4(0),r10=vec4(0),r11=vec4(0);\n";
    s += "  vec4 oPos=vec4(0,0,0,1), oD0=vec4(1), oD1=vec4(0), oFog=vec4(0), oPts=vec4(1);\n";
    s += "  vec4 oT0=vec4(0), oT1=vec4(0), oT2=vec4(0), oT3=vec4(0);\n";
    s += "  int a0 = 0;\n";

    bool const_write_noted = false;
    bool writes_fog = false;

    for (uint32_t i = 0; i < instruction_count; i++) {
        VshInst in;
        vsh_decode(program + i * 4, &in);
        if (in.mac == MAC_NOP && in.ilu == ILU_NOP) {
            if (in.final) break;
            continue;
        }

        // The two units run in parallel and both read before either writes. Keeping A, B and C as locals and
        // doing every write after them is what reproduces that ordering; writing the destination straight
        // after the MAC would make a shader that reads its own output, which is a different program.
        s += "  {\n";
        s += "    vec4 A = " + glsl_src(in, in.a) + ";\n";
        s += "    vec4 B = " + glsl_src(in, in.b) + ";\n";
        s += "    vec4 C = " + glsl_src(in, in.c) + ";\n";

        if (in.mac != MAC_NOP) {
            std::string m;
            switch (in.mac) {
            case MAC_MOV: m = "A"; break;
            case MAC_MUL: m = "A * B"; break;
            case MAC_ADD: m = "A + C"; break;
            case MAC_MAD: m = "A * B + C"; break;
            // The console's dot products broadcast their scalar into all four lanes, because the write mask
            // then picks the lane the shader wanted. vec4(...) is GLSL's way of saying the same thing.
            case MAC_DP3: m = "vec4(dot(A.xyz, B.xyz))"; break;
            case MAC_DPH: m = "vec4(dot(A.xyz, B.xyz) + B.w)"; break;
            case MAC_DP4: m = "vec4(dot(A, B))"; break;
            case MAC_DST: m = "vec4(1.0, A.y * B.y, A.z, B.w)"; break;
            case MAC_MIN: m = "min(A, B)"; break;
            case MAC_MAX: m = "max(A, B)"; break;
            // lessThan / greaterThanEqual, and not `<` and `>=`. GLSL's comparison operators on two vectors
            // answer one bool for the whole vector, where the console answers per component -- so `A < B`
            // here would fail to compile rather than quietly do the wrong thing, which is the better of the
            // two failures. The constructor then turns that bvec back into the float vector the console had.
            case MAC_SLT: m = "vec4(lessThan(A, B))"; break;
            case MAC_SGE: m = "vec4(greaterThanEqual(A, B))"; break;
            case MAC_ARL: m = "A"; break;
            default: m = "vec4(0)"; break;
            }
            s += "    vec4 M = " + m + ";\n";
        }

        if (in.ilu != ILU_NOP) {
            std::string l;
            switch (in.ilu) {
            case ILU_MOV: l = "C"; break;
            case ILU_RCP: l = "vec4(1.0 / C.x)"; break;
            // RCC is the reciprocal with the console's own guard: the clamp bounds are the ones its
            // hardware used, and a plain 1.0/x here would produce infinities where the console produced a
            // large finite number.
            case ILU_RCC:
                l = "vec4((C.x < 0.0 ? -1.0 : 1.0) * clamp(1.0 / abs(C.x), 5.42101e-20, 1.884467e19))";
                break;
            case ILU_RSQ: l = "vec4(inversesqrt(abs(C.x)))"; break;
            // EXP and LOG return four values, not one: the fraction and the integer part land in different
            // lanes, which is why they are written out rather than reduced to exp2().
            case ILU_EXP: l = "vec4(exp2(floor(C.x)), C.x - floor(C.x), exp2(C.x), 1.0)"; break;
            case ILU_LOG:
                l = "vec4(floor(log2(abs(C.x))), abs(C.x) / exp2(floor(log2(abs(C.x)))), log2(abs(C.x)), 1.0)";
                break;
            case ILU_LIT:
                l = "vec4(1.0, max(C.x, 0.0), (C.x > 0.0 && C.y > 0.0) ? pow(C.y, clamp(C.w, -127.9961, "
                     "127.9961)) : 0.0, 1.0)";
                break;
            default: l = "vec4(0)"; break;
            }
            s += "    vec4 L = " + l + ";\n";
        }

        if (in.mac == MAC_ARL) {
            // Address register: floored with the console's 0.001 nudge, which is what stops a value that is
            // one ULP under an integer from indexing the previous constant.
            s += "    a0 = int(floor(M.x + 0.001));\n";
        } else if (in.mac != MAC_NOP && in.out_mac_mask) {
            const std::string dst = in.out_r == 12 ? std::string("oPos") : ("r" + std::to_string(in.out_r));
            const std::string mk = glsl_mask(in.out_mac_mask);
            s += "    " + dst + "." + mk + " = M." + mk + ";\n";
        }

        if (in.ilu != ILU_NOP && in.out_ilu_mask) {
            // A paired ILU writes r1 when the MAC took the register, because the two units share the
            // register file and the MAC's write already claimed its destination.
            const uint8_t r = in.mac != MAC_NOP ? 1 : in.out_r;
            const std::string dst = r == 12 ? std::string("oPos") : ("r" + std::to_string(r));
            const std::string mk = glsl_mask(in.out_ilu_mask);
            s += "    " + dst + "." + mk + " = L." + mk + ";\n";
        }

        if (in.out_o_mask) {
            const bool from_ilu = in.out_mux != 0;
            if ((from_ilu && in.ilu != ILU_NOP) || (!from_ilu && in.mac != MAC_NOP && in.mac != MAC_ARL)) {
                const std::string mk = glsl_mask(in.out_o_mask);
                if (in.out_orb) {
                    if (const char *o = out_name(in.out_address)) {
                        const std::string oo = o;
                        s += "    " + oo + "." + mk + " = " + (from_ilu ? "L." : "M.") + mk + ";\n";
                        if (oo == "oFog") writes_fog = true;
                    }
                } else if (!const_write_noted) {
                    // A write to the constant bank. Not implemented, and said once rather than per
                    // instruction: the game does not do it, and a hundred identical comments would bury the
                    // one fact worth reading.
                    s += "    // constant write not supported\n";
                    const_write_noted = true;
                }
            }
        }
        s += "  }\n";
        if (in.final) break;
    }

    // The NV2A rasterises from screen positions with four fractional bits, so separately transformed pieces
    // that meet -- destructible walls are built from chunks, each with its own matrix -- land on the same
    // point. A host with finer precision at a higher render size leaves hairline cracks that flicker along
    // such seams. Snapping to a sub-pixel grid of the console's own closes them.
    s += "  oPos.xy = floor(oPos.xy * 8.0 + 0.5) * 0.125;\n";
    // The NV2A does not divide by w -- x/y/z are already screen coordinates, and w only feeds perspective-
    // correct interpolation -- so a 2D shader often leaves w at zero. Treating that as 1 is the difference
    // between a full-screen quad and a division by zero.
    s += "  float w = (oPos.w == 0.0 || !(abs(oPos.w) < 3.0e38)) ? 1.0 : oPos.w;\n";
    s += "  gl_Position = vec4((oPos.xyz - c[193].xyz) / c[192].xyz * w, w);\n";
    s += "  gl_Position.xy *= vmScale.xy;\n";
    if (outs.d0) s += "  d0 = oD0;\n";
    if (outs.d1) s += "  d1 = oD1;\n";
    // The NV2A's texture coordinates are in texels, so the 1/scale the console would have applied in fixed
    // function is applied here. Without it every texture samples at a multiple of its own size.
    // The console samples in texels, so the 1/scale its fixed function would have applied is applied here.
    // Without it every texture samples at a multiple of its own size, which looks like wrong textures
    // rather than like a missing scale.
    if (outs.t0) s += "  t0 = oT0 * vec4(texScale0.xy, 1, 1);\n";
    if (outs.t1) s += "  t1 = oT1 * vec4(texScale1.xy, 1, 1);\n";
    if (outs.t2) s += "  t2 = oT2 * vec4(texScale2.xy, 1, 1);\n";
    if (outs.t3) s += "  t3 = oT3 * vec4(texScale3.xy, 1, 1);\n";
    if (outs.pts) s += "  pts = oPts.x;\n";

    // The NV2A fog unit turned the oFog coordinate into a factor with the table mode. Vulkan has no fog at
    // all, so the factor is computed here and handed on as a varying.
    if (writes_fog && outs.fog) {
        s += "  float fc = abs(oFog.x);\n";
        s += "  float fden = fogParams.y - fogParams.x;\n";
        s += "  float flin = (fogParams.y - fc) / (abs(fden) > 1e-6 ? fden : 1e-6);\n";
        s += "  float fd = fogParams.z * fc;\n";
        s += "  fog = clamp(dot(fogMode, vec4(flin, exp(-fd), exp(-fd * fd), oFog.x)), 0.0, 1.0);\n";
    } else if (outs.fog) {
        s += "  fog = 1.0;\n";
    }

    s += "}\n";
    return s;
}

} // namespace xd3d