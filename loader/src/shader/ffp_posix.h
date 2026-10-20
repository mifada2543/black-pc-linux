// The console's fixed-function texture stages, as a fragment shader.
//
// On the Xbox a draw with no pixel program still had a fragment stage: the NV2A's register combiners, which
// D3D8 described as four texture stages with a colour operation and an alpha operation each. The game at
// this point in its life draws with exactly that -- a vertex program and no pixel program -- so the only
// way to show its pictures is to turn those stage descriptions back into shader code.
//
// The numbering here is the console's, not D3D9's. The two differ in the middle of the op list (the Xbox
// puts DOTPRODUCT3 at 22, where D3D9 puts BUMPENVMAP), and the Windows port's table is the evidence for
// this one. That table is also why the sampler state lives at 0..8 and the stage state at 12..21: those are
// the offsets the game writes, so they are the offsets everything that reads them has to use.
//
// An operation this file has not translated is refused with a reason rather than approximated. A wrong
// picture is worse than a logged refusal: it sends whoever debugs it looking at the wrong subsystem.
#pragma once

#include <stdint.h>

#include <string>

namespace xd3d {

// The per-stage state this file reads, at the console's offsets. The array is 32 dwords per stage.
enum TextureStageState : uint32_t {
    XTSS_ADDRESSU = 0, XTSS_ADDRESSV, XTSS_ADDRESSW,
    XTSS_MAGFILTER, XTSS_MINFILTER, XTSS_MIPFILTER, XTSS_MIPMAPLODBIAS, XTSS_MAXMIPLEVEL, XTSS_MAXANISOTROPY,
    XTSS_COLOROP = 12, XTSS_COLORARG0, XTSS_COLORARG1, XTSS_COLORARG2,
    XTSS_ALPHAOP, XTSS_ALPHAARG0, XTSS_ALPHAARG1, XTSS_ALPHAARG2,
    XTSS_RESULTARG, XTSS_TEXTURETRANSFORMFLAGS,
    XTSS_TEXCOORDINDEX = 28, XTSS_BORDERCOLOR = 29,
};

// The console's texture operations, at the numbers it used.
enum TextureOp : uint32_t {
    XTO_DISABLE = 0,
    XTO_SELECTARG1 = 2,
    XTO_SELECTARG2 = 3,
    XTO_MODULATE = 4,
    XTO_MODULATE2X = 5,
    XTO_MODULATE4X = 6,
    XTO_ADD = 7,
    XTO_ADDSIGNED = 8,
    XTO_ADDSIGNED2X = 9,
    XTO_SUBTRACT = 10,
    XTO_ADDSMOOTH = 11,
    XTO_BLENDDIFFUSEALPHA = 12,
    XTO_BLENDCURRENTALPHA = 13,
    XTO_BLENDTEXTUREALPHA = 14,
    XTO_BLENDFACTORALPHA = 15,
    XTO_BLENDTEXTUREALPHAPM = 16,
    XTO_PREMODULATE = 17,
    XTO_MODULATEALPHA_ADDCOLOR = 18,
    XTO_MODULATECOLOR_ADDALPHA = 19,
    XTO_MODULATEINVALPHA_ADDCOLOR = 20,
    XTO_MODULATEINVCOLOR_ADDALPHA = 21,
    XTO_DOTPRODUCT3 = 22,
    XTO_MULTIPLYADD = 23,
    XTO_LERP = 24,
    XTO_BUMPENVMAP = 25,
    XTO_BUMPENVMAPLUMINANCE = 26,
};

// The console's texture-argument bits.
enum TextureArg : uint32_t {
    XTA_SELECTMASK = 0x0F,
    XTA_DIFFUSE = 0x00,
    XTA_CURRENT = 0x01,
    XTA_TEXTURE = 0x02,
    XTA_TFACTOR = 0x03,
    XTA_SPECULAR = 0x04,
    XTA_TEMP = 0x05,
    XTA_COMPLEMENT = 0x10,
    XTA_ALPHAREPLICATE = 0x20,
};

struct FixedFunctionShader {
    std::string glsl;
    // Stages with an operation, which is also how many textures the shader samples. Stage 0 always counts
    // when any later stage is enabled, because a disabled stage ends the chain.
    uint32_t stages = 0;
    bool uses_diffuse = false;
    bool uses_specular = false;

    // The stage state this shader was generated from, so a caller can tell two shaders apart without
    // re-deriving it: the pipeline cache key has to change when any of this does.
    uint32_t color_op[4] = {};
    uint32_t alpha_op[4] = {};
    uint32_t color_arg[4][3] = {};
    uint32_t alpha_arg[4][3] = {};
};

// Translate texture stages into one fragment shader. `stages` is TEXTURESTAGE_PER_STAGE * 4 dwords in the
// device's own layout. Returns false with a reason for anything not translated.
bool build_fixed_function_shader(const uint32_t *stages, FixedFunctionShader *out, const char **err);

} // namespace xd3d
