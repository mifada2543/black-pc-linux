// One material: a vertex program and a pixel program, turned into a graphics pipeline.
//
// This is where the two translator halves stop being text and become something the driver can run. Until it
// existed the chain ended at SPIR-V in memory, which proved the translation was well formed and said nothing
// about whether the two halves could be used together -- and whether is not a formality. A vertex shader
// writing a texture coordinate at one location and a fragment shader reading it at another is a pipeline the
// driver refuses, and the two sides are written by hand in different files.
//
// The chain, end to end:
//
//     NV2A microcode -> GLSL -> SPIR-V -> VkShaderModule -> VkGraphicsPipeline
//
// Nothing here decides what the shaders *mean*. That is nv2a_vsh_posix and nv2a_psh_posix. This decides what
// they are attached to: the colour and depth targets, the blend and depth state, and the vertex layout --
// all of which the console also described with state fields, and all of which therefore have to be passed
// through rather than guessed.
#pragma once

#include <stdint.h>

#include "d3d/vertex_posix.h"
#include "gfx/gfx_vulkan.h"
#include "shader/nv2a_psh_posix.h"

namespace xd3d {

// The console's pixel program, which the game hands over as a block of dwords rather than as a structure:
// the first 57 are the D3DPIXELSHADERDEF and the rest is the program. Taking the definition by pointer here
// and letting the caller carve it out of the game's buffer keeps that knowledge where it belongs.
struct MaterialDesc {
    // The vertex side.
    const uint32_t *vertex_program = nullptr;
    uint32_t vertex_instruction_count = 0;
    // Which vertex registers the declaration feeds, and the declaration's elements, which between them give
    // the pipeline's vertex input state. The count matters twice over: it says which varyings exist, and it
    // says how wide each one is.
    uint32_t vertex_input_mask = 0;
    const VertexElement *vertex_elements = nullptr;
    uint32_t vertex_element_count = 0;
    const uint32_t *stream_stride = nullptr; // may be null; the declaration's own stride is used then

    // The pixel side. `pixel_shader` may be null for a shader the game draws with no fragment stage. In
    // that case `fragment_glsl` may supply the console's fixed-function stages; with neither, the colour
    // comes straight from the vertex stage and a pass-through fragment shader is used.
    const PixelShaderDef *pixel_shader = nullptr;
    const char *fragment_glsl = nullptr;
    uint32_t tex_cube = 0; // one bit per stage: the console's stage samples a cube map

    // The render state that reaches the pipeline rather than the shader. The PipelineFlag bits are the
    // console's defaults for this game, and each one here is a decision the console made in fixed function.
    uint32_t flags = 0;
    uint32_t color_format = 0; // a gfx::VertexFormatCode
    uint32_t depth_format = 0;
    uint32_t sample_count = 1;
};

// Build the pipeline. Returns false with a reason in `err`; the reason is the one to show, because the
// alternative -- letting the driver's error through -- names a Vulkan call rather than a shader.
bool build_material(gfx::Pipeline *out, const MaterialDesc &desc, const char **err);

// The state a cache key must distinguish. Two materials with equal keys produce identical GLSL and identical
// pipeline state, so one can stand in for the other; anything left out of this is a bug that shows up as the
// wrong shader on a later draw, with nothing in the log.
uint64_t material_cache_key(const MaterialDesc &desc);

} // namespace xd3d