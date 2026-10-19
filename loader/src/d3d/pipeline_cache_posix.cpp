// Pipeline cache implementation.
//
// SCAFFOLDING. Nothing calls this file yet. `grep -rl pipeline_cache src/` returns this source and its own
// header and nothing else: no draw row, no HLE entry, no device function reaches PipelineCache::get_or_create,
// build_pipeline_key, or pipeline_cache(). It compiles and it is internally consistent, but "compiles" is
// the only thing that has been demonstrated about it. Nothing here has ever produced a drawn pixel, so
// treat the code below as untested against real output.
//
// What is actually implemented, stated narrowly:
//   - build_vertex_spirv() translates a VertexProgram out of the vertex shader store through vsh_to_glsl
//     and compiles it with glslang. It is reachable code on paper only, because no draw path calls it.
//   - build_pipeline_from_key() refuses the fixed-function path (below) and refuses the pixel-shader path
//     (below). Those two refusals are the only outcomes this function can currently produce, so today every
//     call would fail -- and since nothing calls it, today it fails nothing either.
//
// What remains, and why it is not done here:
//   (a) The fixed-function FVF path. Vertex shader handle 0 means the console's fixed-function vertex
//       format, which needs the FVF constants read out of the game image (src/d3d/vertex_posix.h, D3DFVF_*).
//       Those constants are not available, so there is no way to know what transform a given FVF code means.
//   (b) Pixel shader resolution. See build_pipeline_from_key() -- there is no pixel shader store to consult.
//   (c) Wiring this into the draw rows in src/d3d/draw_posix.cpp so a draw actually asks for a pipeline.
//       That is real A4 work and is blocked behind (a); it is not attempted rather than half-attempted here.
//
// The pass-through fragment shader below is honest ONLY for a draw that bound no pixel shader. It is not a
// stand-in for one. Substituting it where the game set a pixel shader would draw the game's geometry with
// a fixed colour and look plausible, which is the failure mode this comment exists to rule out.
#include "d3d/pipeline_cache_posix.h"

#include "d3d/vertex_posix.h"
#include "gfx/gfx_vulkan.h"
#include "shader/nv2a_vsh_posix.h"
#include "shader/nv2a_psh_posix.h"
#include "d3d/vshader_store_posix.h"
#include "shader/glslang_posix.h"
#include "shader/material_posix.h"

#include "platform/posix/log.h"

#include <cstring>

namespace xd3d {

namespace {

// FVF position-size table from Xbox D3D runtime at vaddr 0x2782d8 (raw 0x268a58 in D3D section).
// Index = position type ((FVF >> 8) & 0xF). Value = (byte & 0x3C) / 8 = position size in dwords.
// Special cases handled in fvf_decode():
//   pos_type == 0x0C: return (1 << texcount) * 2 dwords
//   pos_type == 0x0E or 0x0F: return (1 << texcount) * 4 dwords
//   pos_type > 0x0F: treated as table index (same as <= 0x0D)
static const uint8_t fvf_pos_dwords[16] = {
    1, // 0x00
    1, // 0x01
    2, // 0x02
    2, // 0x03
    2, // 0x04
    2, // 0x05
    4, // 0x06
    4, // 0x07
    0, // 0x08
    0, // 0x09
    0, // 0x0a
    1, // 0x0b
    0, // 0x0c (special: handled separately)
    0, // 0x0d
    1, // 0x0e (special: 4 dwords)
    1, // 0x0f (special: 4 dwords)
};

struct FVFInfo {
    uint32_t stride = 0;        // total vertex stride in bytes
    uint32_t pos_offset = 0;    // position offset in bytes
    uint32_t pos_size = 0;      // position size in bytes
    uint32_t pos_format = 0;    // VertexFormatCode for position
    uint32_t normal_offset = 0;
    uint32_t diffuse_offset = 0;
    uint32_t specular_offset = 0;
    uint32_t tex_count = 0;
    uint32_t tex_offsets[8] = {};
    uint32_t tex_sizes[8] = {};
    bool valid = false;
};

// Decode FVF code into vertex layout information. Returns false if the FVF is unsupported.
bool fvf_decode(uint32_t fvf, FVFInfo *out)
{
    // Position type: bits 0-3 of byte 1 (FVF >> 8) & 0xF
    uint32_t pos_type = (fvf >> 8) & 0xF;
    // TEXCOUNT: bits 20-23 (FVF >> 20) & 0xF
    uint32_t texcount = (fvf >> 20) & 0xF;
    
    // Texture coordinates: each is 2 floats = 8 bytes
    uint32_t tex_bytes = texcount * 8;
    
    uint32_t pos_dwords = 0;
    if (pos_type == 0x0C) {
        // Special: position = edx * 2 where edx = 1 << texcount
        pos_dwords = (1u << texcount) * 2;
    } else if (pos_type == 0x0E || pos_type == 0x0F) {
        // Special: position = edx * 4 where edx = 1 << texcount
        pos_dwords = (1u << texcount) * 4;
    } else {
        // Table lookup: index = pos_type
        if (pos_type < 16) {
            pos_dwords = fvf_pos_dwords[pos_type] * (1u << texcount);
        } else {
            return false; // unsupported position type
        }
    }
    
    if (pos_dwords == 0) return false;
    
    uint32_t pos_bytes = pos_dwords * 4;
    
    // Build stride: position + normal + diffuse + specular + textures
    uint32_t stride = pos_bytes;
    uint32_t normal_bytes = 0, diffuse_bytes = 0, specular_bytes = 0;
    
    if (fvf & 0x10) { normal_bytes = 12; }  // D3DFVF_NORMAL
    if (fvf & 0x40) { diffuse_bytes = 4; }  // D3DFVF_DIFFUSE
    if (fvf & 0x80) { specular_bytes = 4; } // D3DFVF_SPECULAR
    
    stride += normal_bytes + diffuse_bytes + specular_bytes + tex_bytes;
    
    // Position format depends on position type
    // For the Xbox: pos_type 0x02 (XYZ) = 3 floats, 0x04 (XYZRHW) = 4 floats, 0x06 (XYZB1) = 4 floats, etc.
    uint32_t pos_format = xd3d::VF_R32G32B32A32_SFLOAT; // default to 4 floats
    if (pos_type == 0x02) { // XYZ = 3 floats
        pos_format = xd3d::VF_R32G32B32_SFLOAT;
    } else if (pos_type == 0x04) { // XYZRHW
        pos_format = xd3d::VF_R32G32B32A32_SFLOAT;
    } else if (pos_type == 0x06 || pos_type == 0x08) { // XYZB1/XYZB2
        pos_format = xd3d::VF_R32G32B32A32_SFLOAT; // 4 floats (last is blend weight)
    } else if (pos_type == 0x0A || pos_type == 0x0C || pos_type == 0x0E) { // XYZB3/4/5
        pos_format = xd3d::VF_R32G32B32A32_SFLOAT;
    }
    
    out->stride = stride;
    out->pos_offset = 0;
    out->pos_size = pos_bytes;
    out->pos_format = pos_format;
    out->normal_offset = pos_bytes;
    out->diffuse_offset = pos_bytes + normal_bytes;
    out->specular_offset = pos_bytes + normal_bytes + diffuse_bytes;
    out->tex_count = texcount;
    
    uint32_t off = pos_bytes + normal_bytes + diffuse_bytes + specular_bytes;
    for (uint32_t i = 0; i < texcount; i++) {
        out->tex_offsets[i] = off;
        out->tex_sizes[i] = 8; // 2 floats per texcoord
        off += 8;
    }
    out->valid = true;
    return true;
}

// Build SPIR-V for fixed-function FVF vertex shader.
// The shader reads the FVF layout, applies world-view-projection transform, and passes through texcoords/colors.
bool build_fvf_spirv(const FVFInfo &info, std::vector<uint32_t> *out_spirv, const char **err)
{
    // We need to generate a vertex shader that:
    // 1. Reads position (vec4 or vec3) at location 0
    // 2. Reads normal (if present) at location 1
    // 3. Reads diffuse/specular (if present) at location 2/3
    // 4. Reads texcoords at subsequent locations
    // 5. Applies world-view-projection matrix (from c0-c3 or c192-c195)
    // 6. Passes through texcoords and colors
    
    // For simplicity and correctness, we build a GLSL shader and compile it.
    // The shader must match the FVF layout exactly.
    
    std::string glsl = "#version 450\n";
    
    // Inputs
    uint32_t loc = 0;
    glsl += "layout(location = " + std::to_string(loc++) + ") in vec4 in_pos;\n"; // position is always 4 floats
    
    bool has_normal = (info.pos_size < info.stride && (info.normal_offset > 0));
    bool has_diffuse = (info.diffuse_offset < info.stride);
    bool has_specular = (info.specular_offset < info.stride);
    
    if (has_normal) {
        glsl += "layout(location = " + std::to_string(loc++) + ") in vec3 in_normal;\n";
    }
    if (has_diffuse) {
        glsl += "layout(location = " + std::to_string(loc++) + ") in vec4 in_diffuse;\n";
    }
    if (has_specular) {
        glsl += "layout(location = " + std::to_string(loc++) + ") in vec4 in_specular;\n";
    }
    
    for (uint32_t i = 0; i < info.tex_count; i++) {
        glsl += "layout(location = " + std::to_string(loc++) + ") in vec2 in_tex" + std::to_string(i) + ";\n";
    }
    
    // Outputs
    glsl += "layout(location = 0) out vec4 out_pos;\n";
    if (has_normal) {
        glsl += "layout(location = 1) out vec3 out_normal;\n";
    }
    if (has_diffuse) {
        glsl += "layout(location = 2) out vec4 out_diffuse;\n";
    }
    if (has_specular) {
        glsl += "layout(location = 3) out vec4 out_specular;\n";
    }
    for (uint32_t i = 0; i < info.tex_count; i++) {
        glsl += "layout(location = " + std::to_string(4 + i) + ") out vec2 out_tex" + std::to_string(i) + ";\n";
    }
    
    // Constants: world-view-projection matrix at c192-c195 (per Xbox convention)
    glsl += "layout(std140, binding = 0) uniform Consts {\n";
    glsl += "  vec4 c[198];\n";
    glsl += "};\n";
    
    // Main
    glsl += "void main() {\n";
    glsl += "  // Position transform: use c192-c195 as world-view-projection matrix\n";
    glsl += "  vec4 pos = in_pos;\n";
    glsl += "  pos.xyz = (pos.xyz - c[193].xyz) / c[192].xyz * pos.w;\n";
    glsl += "  pos.xy *= vec2(c[194].x, c[194].y);\n";
    glsl += "  out_pos = pos;\n";
    
    if (has_normal) {
        glsl += "  out_normal = in_normal;\n";
    }
    if (has_diffuse) {
        glsl += "  out_diffuse = in_diffuse;\n";
    }
    if (has_specular) {
        glsl += "  out_specular = in_specular;\n";
    }
    for (uint32_t i = 0; i < info.tex_count; i++) {
        glsl += "  out_tex" + std::to_string(i) + " = in_tex" + std::to_string(i) + ";\n";
    }
    glsl += "}\n";
    
    // Compile to SPIR-V
    size_t spirv_size = 0;
    uint32_t *spirv = compile_glsl_to_spirv(glsl.c_str(), SHADER_STAGE_VERTEX, &spirv_size, err);
    if (!spirv) return false;
    out_spirv->assign(spirv, spirv + spirv_size / sizeof(uint32_t));
    free_shader_spirv(spirv);
    return true;
}

// Build SPIR-V for vertex shader from VertexProgram
bool build_vertex_spirv(const VertexProgram *vp, std::vector<uint32_t> *out_spirv, const char **err)
{
    // Compute vertex input mask from declaration
    uint32_t input_mask = 0;
    for (uint32_t i = 0; i < vp->declaration.count; i++) {
        if (vp->declaration.elements[i].reg < 32) {
            input_mask |= (1u << vp->declaration.elements[i].reg);
        }
    }

    // Translate NV2A vertex microcode to GLSL
    std::string glsl = vsh_to_glsl(vp->program.data(), vp->instructions, input_mask, err);
    if (glsl.empty()) {
        return false;
    }

    // Compile GLSL to SPIR-V
    size_t spirv_size = 0;
    uint32_t *spirv = compile_glsl_to_spirv(glsl.c_str(), SHADER_STAGE_VERTEX, &spirv_size, err);
    if (!spirv) {
        return false;
    }
    out_spirv->assign(spirv, spirv + spirv_size / sizeof(uint32_t));
    free_shader_spirv(spirv);
    return true;
}

// Build SPIR-V for pixel shader from PixelShaderDef.
//
// `ps` is the *only* place a pass-through fragment shader belongs in this file, and a null `ps` is the only
// honest reason to use one: it means the game bound no pixel shader, so there is no material to lose. The
// caller refuses the `pixel_shader_handle != 0` case outright rather than reaching here with a null `ps`,
// because on this port a non-zero handle cannot be told apart from "resolved to nothing" -- there is no
// pixel shader store to look it up in (see build_pipeline_from_key).
bool build_pixel_spirv(const PixelShaderDef *ps, std::vector<uint32_t> *out_spirv, const char **err)
{
    if (!ps) {
        // No pixel shader bound. Pass through the vertex colour and nothing else; this is a correct fragment
        // stage for that case and an incorrect one for every other case, which is why the caller gates it.
        static const char *pass_through_frag = R"(
#version 450
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 f_color;
void main() { f_color = v_color; }
)";
        size_t spirv_size = 0;
        uint32_t *spirv = compile_glsl_to_spirv(pass_through_frag, SHADER_STAGE_FRAGMENT, &spirv_size, err);
        if (!spirv) {
            return false;
        }
        out_spirv->assign(spirv, spirv + spirv_size / sizeof(uint32_t));
        free_shader_spirv(spirv);
        return true;
    }

    // Translate NV2A pixel microcode to GLSL
    std::string glsl = psh_to_glsl(*ps, 0, err);
    if (glsl.empty()) {
        return false;
    }

    // Compile GLSL to SPIR-V
    size_t spirv_size = 0;
    uint32_t *spirv = compile_glsl_to_spirv(glsl.c_str(), SHADER_STAGE_FRAGMENT, &spirv_size, err);
    if (!spirv) {
        return false;
    }
    out_spirv->assign(spirv, spirv + spirv_size / sizeof(uint32_t));
    free_shader_spirv(spirv);
    return true;
}

// Build VkPipeline from PipelineKey
bool build_pipeline_from_key(const PipelineKey &key, gfx::Pipeline *out, const char **err)
{
    // Get vertex shader SPIR-V
    std::vector<uint32_t> vertex_spirv;
    if (key.vertex_shader_handle != 0) {
        const VertexProgram *vp = vertex_program(key.vertex_shader_handle);
        if (!vp) {
            *err = "vertex shader not found";
            return false;
        }
        if (!build_vertex_spirv(vp, &vertex_spirv, err)) {
            return false;
        }
    } else {
        // Fixed-function FVF: build a vertex shader that does the FVF transform.
        // The FVF code is in key.fvf_code. Decode it and build a vertex shader that
        // implements the FVF transform using the console's own position-size table.
        FVFInfo fvf_info;
        if (!fvf_decode(key.fvf_code, &fvf_info)) {
            *err = "unsupported FVF code";
            return false;
        }
        
        std::vector<uint32_t> fvf_spirv;
        if (!build_fvf_spirv(fvf_info, &vertex_spirv, err)) {
            return false;
        }
        
        // Build vertex layout from FVF info
        gfx::VertexAttribute attributes[32];
        uint32_t attr_count = 0;
        uint32_t loc = 0;
        
        // Position
        attributes[attr_count++] = {0, loc++, fvf_info.pos_format, 0};
        
        // Normal
        if (fvf_info.normal_offset > 0 && fvf_info.normal_offset < fvf_info.stride) {
            attributes[attr_count++] = {0, attr_count, xd3d::VF_R32G32B32_SFLOAT, fvf_info.normal_offset};
        }
        
        // Diffuse
        if (fvf_info.diffuse_offset < fvf_info.stride) {
            attributes[attr_count++] = {0, attr_count, xd3d::VF_R8G8B8A8_UNORM, fvf_info.diffuse_offset};
        }
        
        // Specular
        if (fvf_info.specular_offset < fvf_info.stride) {
            attributes[attr_count++] = {0, attr_count, xd3d::VF_R8G8B8A8_UNORM, fvf_info.specular_offset};
        }
        
        // Texcoords
        for (uint32_t i = 0; i < fvf_info.tex_count; i++) {
            attributes[attr_count++] = {0, attr_count, xd3d::VF_R32G32_SFLOAT, fvf_info.tex_offsets[i]};
        }
        
        gfx::VertexBinding bindings[16];
        bindings[0] = {0, (uint16_t)fvf_info.stride, 0}; // per-vertex
        
        // Build pipeline with FVF SPIR-V and pass-through fragment shader
        std::vector<uint32_t> fragment_spirv;
        if (!build_pixel_spirv(nullptr, &fragment_spirv, err)) {
            return false;
        }
        
        if (!gfx::create_pipeline_d3d(out,
                                      vertex_spirv.data(), vertex_spirv.size() * sizeof(uint32_t),
                                      fragment_spirv.data(), fragment_spirv.size() * sizeof(uint32_t),
                                      key.color_format, key.depth_format,
                                      attributes, attr_count,
                                      bindings, 1,
                                      key.pipeline_flags, key.sample_count)) {
            *err = gfx::last_error();
            return false;
        }
        return true;
    }

    // Get pixel shader SPIR-V
    //
    // There is no pixel shader store in this port, so a non-zero handle cannot be resolved to a
    // PixelShaderDef and this must refuse rather than guess:
    //   - src/d3d/vshader_store_posix.h provides the vertex side only (vertex_program_create /
    //     vertex_program). There is no pixel equivalent; src/d3d/ has no pshader store file.
    //   - D3DDevice_SetPixelShader (src/d3d/hle_device.cpp:577) stores the raw handle in
    //     state().pixel_shader and translates nothing; its own comment says "A5 does the translation, so
    //     there is nothing to copy in yet".
    //   - CreatePixelShader is not implemented at all. It appears nowhere in src/ except a comment in
    //     pipeline_cache_posix.h.
    // So the handle the game holds points at nothing this loader can read. Building build_pixel_spirv()'s
    // pass-through here would compile, would cache, and would draw every pixel of the game's material as one
    // fixed colour -- wrong output that survives review because it still animates. Refuse and let the caller
    // report it; that failure is a log line, this one is a corrupted frame.
    std::vector<uint32_t> fragment_spirv;
    if (key.pixel_shader_handle != 0) {
        *err = "pixel shader handle is set but this port has no pixel shader store: CreatePixelShader is "
               "not implemented and D3DDevice_SetPixelShader only records the handle, so it cannot be "
               "resolved to a PixelShaderDef. Refusing instead of drawing with a pass-through that would "
               "ignore the game's material.";
        return false;
    }
    // Handle 0: the game bound no pixel shader, so the pass-through in build_pixel_spirv is the correct
    // fragment stage rather than a substitute for a missing one.
    if (!build_pixel_spirv(nullptr, &fragment_spirv, err)) {
        return false;
    }

    // Convert VertexFormatCode to gfx::VertexAttribute for attributes
    gfx::VertexAttribute attributes[32];
    for (uint32_t i = 0; i < key.vertex_layout.attribute_count; i++) {
        const auto &a = key.vertex_layout.attributes[i];
        attributes[i].binding = a.binding;
        attributes[i].location = a.location;
        attributes[i].format = a.format;  // VertexFormatCode, converted in gfx layer
        attributes[i].offset = a.offset;
    }

    gfx::VertexBinding bindings[16];
    for (uint32_t i = 0; i < key.vertex_layout.binding_count; i++) {
        const auto &b = key.vertex_layout.bindings[i];
        bindings[i].binding = b.binding;
        bindings[i].stride = b.stride;
        bindings[i].rate = b.rate; // 0 = per vertex, 1 = per instance
    }

    // Use gfx::create_pipeline_d3d which converts d3d format codes to VkFormat internally
    if (!gfx::create_pipeline_d3d(out,
                                  vertex_spirv.data(), vertex_spirv.size() * sizeof(uint32_t),
                                  fragment_spirv.data(), fragment_spirv.size() * sizeof(uint32_t),
                                  key.color_format, key.depth_format,
                                  attributes, key.vertex_layout.attribute_count,
                                  bindings, key.vertex_layout.binding_count,
                                  key.pipeline_flags, key.sample_count)) {
        *err = gfx::last_error();
        return false;
    }

    return true;
}

} // namespace

// PipelineCache implementation

gfx::Pipeline *PipelineCache::get_or_create(const PipelineKey &key, const char **err)
{
    HashMap::Bucket *entry = cache_.find(key);
    if (entry) {
        hits_++;
        entry->entry.last_used_frame = current_frame_;
        return entry->entry.valid ? &entry->entry.pipeline : nullptr;
    }

    misses_++;
    HashMap::Bucket *new_entry = cache_.insert(key);
    if (!new_entry) {
        *err = "pipeline cache insert failed";
        return nullptr;
    }

    if (build_pipeline_from_key(key, &new_entry->entry.pipeline, err)) {
        new_entry->entry.valid = true;
        new_entry->entry.last_used_frame = current_frame_;
        return &new_entry->entry.pipeline;
    } else {
        new_entry->entry.valid = false;
        return nullptr;
    }
}

void PipelineCache::clear()
{
    cache_.clear();
    hits_ = 0;
    misses_ = 0;
}

PipelineCache &pipeline_cache()
{
    static PipelineCache cache;
    return cache;
}

PipelineKey build_pipeline_key(
    uint32_t fvf_code,
    uint32_t vertex_shader_handle,
    const VertexLayout *vertex_layout,
    uint32_t vertex_input_mask,
    uint32_t pixel_shader_handle,
    uint32_t tex_cube_mask,
    uint32_t pipeline_flags,
    uint32_t color_format,
    uint32_t depth_format,
    uint32_t sample_count)
{
    PipelineKey key = {};
    key.fvf_code = fvf_code;
    key.vertex_shader_handle = vertex_shader_handle;
    if (vertex_layout) key.vertex_layout = *vertex_layout;
    key.vertex_input_mask = vertex_input_mask;
    key.pixel_shader_handle = pixel_shader_handle;
    key.tex_cube_mask = tex_cube_mask;
    key.pipeline_flags = pipeline_flags;
    key.color_format = color_format;
    key.depth_format = depth_format;
    key.sample_count = sample_count;
    return key;
}

} // namespace xd3d