// Pipeline cache implementation.
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

// Build SPIR-V for pixel shader from PixelShaderDef
bool build_pixel_spirv(const PixelShaderDef *ps, std::vector<uint32_t> *out_spirv, const char **err)
{
    if (!ps) {
        // No pixel shader - use a trivial pass-through
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
        // Fixed-function FVF - need to build a vertex shader that does the FVF transform
        // This is a placeholder; full FVF implementation needs the FVF constants from XBE
        *err = "fixed-function FVF not yet implemented (needs XBE for FVF constants)";
        return false;
    }

    // Get pixel shader SPIR-V
    std::vector<uint32_t> fragment_spirv;
    if (key.pixel_shader_handle != 0) {
        // Pixel shader handling would go here
        // For now, use pass-through
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
        fragment_spirv.assign(spirv, spirv + spirv_size / sizeof(uint32_t));
        free_shader_spirv(spirv);
    } else {
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
        fragment_spirv.assign(spirv, spirv + spirv_size / sizeof(uint32_t));
        free_shader_spirv(spirv);
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