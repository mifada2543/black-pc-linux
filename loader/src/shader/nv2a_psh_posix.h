// NV2A pixel shaders: the register combiners, and the Xbox's D3DPIXELSHADERDEF.
//
// This is the other half of the shader pair, and it is a different shape of problem from the vertex half.
// The vertex translator decodes instructions; this one reads a fixed structure -- eight combiner stages,
// each with its own inputs, outputs and flags, plus a final combiner -- and emits straight-line arithmetic.
// There is no instruction stream to walk, so the work is in getting the *field encodings* right: an input
// byte is a register in four bits, an alpha-channel flag in the next, and a mapping in the top three.
//
// Two things are worth knowing before reading the code:
//
//   * The console computes RGB and alpha separately, and a register combiner writes into chosen component
//     lanes of its output registers. That is reproduced literally, including the blue-to-alpha copies, which
//     are separate writes rather than part of the RGB one.
//   * Some texture modes are not emulated. The invoice listed them as "not emulated" and so does this; a
//     mode that silently produced a wrong colour would be worse than one that produces zero, because the
//     game's own art would be visibly wrong and nobody would know where to look.
#pragma once

#include <stdint.h>

#include <string>

namespace xd3d {

// D3DPIXELSHADERDEF as the console has it. The first 57 dwords of a pixel program are these, and the
// comment on that is not decoration: the game patches the program in place through render states 0-56, so
// this layout is an address the game writes to, not just something we read.
struct PixelShaderDef {
    uint32_t PSAlphaInputs[8];
    uint32_t PSFinalCombinerInputsABCD;
    uint32_t PSFinalCombinerInputsEFG;
    uint32_t PSConstant0[8];
    uint32_t PSConstant1[8];
    uint32_t PSAlphaOutputs[8];
    uint32_t PSRGBInputs[8];
    uint32_t PSCompareMode;
    uint32_t PSFinalCombinerConstant0;
    uint32_t PSFinalCombinerConstant1;
    uint32_t PSRGBOutputs[8];
    uint32_t PSCombinerCount;
    uint32_t PSTextureModes;
    uint32_t PSDotMapping;
    uint32_t PSInputTexture;
    uint32_t PSC0Mapping;
    uint32_t PSC1Mapping;
    uint32_t PSFinalCombinerConstants;
};

// The constant bank the combiner reads, and the fixed-function registers around it. The layout is the
// console's: c0-7 stage C0, c8-15 stage C1, c16/c17 the final combiner's two.
enum : uint32_t {
    PSH_CONST_COUNT = 18,
    PSH_C_FOG_COLOR = 18,
    PSH_C_BUMP_MAT = 19, // four vec4
    PSH_CONST_BANK = 23,
};

// Which combined image samplers the shader needs, so the descriptor set can be built for exactly those.
// A sampler declared in the GLSL but never bound is a pipeline that reads undefined; one bound but never
// declared is a descriptor the driver expects and cannot get.
struct PshSamplers {
    uint32_t used_2d = 0; // one bit per stage
    uint32_t used_cube = 0;
    bool needs_bump = false;
};

// GLSL for the shader. `tex_cube` has one bit per stage saying the console's stage samples a cube map,
// which changes both the sampler type and the coordinate the sample takes.
std::string psh_to_glsl(const PixelShaderDef &def, uint32_t tex_cube, const char **err);

// What the shader needs from the vertex half, so the two can be checked against each other without
// compiling a pipeline.
uint32_t psh_varyings_used(const PixelShaderDef &def, uint32_t tex_cube);

PshSamplers psh_samplers_used(const PixelShaderDef &def, uint32_t tex_cube);

} // namespace xd3d