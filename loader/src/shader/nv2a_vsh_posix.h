// NV2A vertex program microcode: decoding, and translation to GLSL.
//
// This is the half of A5 that is ours rather than glslang's. glslang has never heard of NV2A -- it compiles
// GLSL and emits SPIR-V, and nothing else -- so the job here is to turn the console's four-dword
// instructions into GLSL text that glslang can then compile at run time. The two halves are deliberately
// kept apart: this file decides *what the shader means*, and the wrapper in glslang_posix.cpp decides how
// to compile it.
//
// The decoding is a transcription, not a derivation: field positions are the public NV2A documentation's,
// and a field in the wrong place produces a shader that compiles cleanly and draws nonsense. The
// arithmetic is the console's, down to the sub-pixel snapping at the end and the handling of a zero `w`.
//
// No `Vk*` type appears here, and no Vulkan type of any kind: this layer's output is GLSL text.
#pragma once

#include <stdint.h>

#include <string>

namespace xd3d {

// ---- the console's instruction set ----

enum VshMac : uint8_t {
    MAC_NOP = 0, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4, MAC_DST, MAC_MIN, MAC_MAX,
    MAC_SLT, MAC_SGE, MAC_ARL
};
enum VshIlu : uint8_t { ILU_NOP = 0, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum VshMux : uint8_t { MUX_R = 1, MUX_V = 2, MUX_C = 3 };

struct VshSrc {
    uint8_t neg;
    uint8_t swz[4];
    uint8_t reg;
    uint8_t mux;
};

struct VshInst {
    uint8_t ilu, mac, cnst, v;
    VshSrc a, b, c;
    uint8_t out_mac_mask, out_r, out_ilu_mask, out_o_mask, out_orb, out_address, out_mux, a0x, final;
};

// One instruction is four dwords; the first is unused. `words` points at the first dword of the instruction,
// not at the start of the program.
void vsh_decode(const uint32_t *words, VshInst *out);

// Output register addresses (OUT_ADDRESS when OUT_ORB is 1).
enum : uint8_t { VSH_OPOS = 0, VSH_OD0 = 3, VSH_OD1 = 4, VSH_OFOG = 5, VSH_OPTS = 6, VSH_OB0 = 7, VSH_OB1 = 8,
                 VSH_OT0 = 9 };

// The constant bank, in registers. c0-c191 are the shader's own; the rest are what the console's fixed
// function writes there, and the translator reads them by name. The layout is the console's, so the numbers
// here are facts about the hardware rather than a choice.
enum : uint32_t {
    VSH_CONST_COUNT = 192,     // c0..c191: the game's own constants
    VSH_C_VP_SCALE = 192,      // the screen-space transform, undone at the end
    VSH_C_VP_OFFSET = 193,
    VSH_C_TEX_SCALE = 194,     // four vec4: 1/size for a linear texture, since NV2A samples in texels
    VSH_C_FOG_PARAMS = 198,    // start, end, density
    VSH_C_FOG_MODE = 199,      // weights: linear, exp, exp2, and the raw coordinate
    VSH_C_VM_SCALE = 200,      // clip x/y factor, for the first-person gun at the game's field of view
    VSH_CONST_BANK = 201,
};

// GLSL for the program. `input_mask` is the set of vertex registers the declaration feeds, which is what
// decides which varyings exist -- the same mask the D3D layer already has from parsing the declaration.
//
// `err` receives a reason and the function returns an empty string when the program cannot be translated at
// all. An empty shader is not a small failure: glslang would refuse it anyway, several calls later and with
// a message about the wrong thing.
std::string vsh_to_glsl(const uint32_t *program, uint32_t instruction_count, uint32_t input_mask,
                        const char **err);

// The varyings this translator writes, in the order it writes them. A fragment translator has to agree with
// this list, so it is one place rather than two that have to be kept in step.
struct VshOutputs {
    uint32_t used = 0; // a bit per output below
    bool pos = true, d0 = true, d1 = true;
    bool t0 = true, t1 = true, t2 = true, t3 = true;
    bool fog = false, pts = true;
};
VshOutputs vsh_outputs(const uint32_t *program, uint32_t instruction_count);

} // namespace xd3d