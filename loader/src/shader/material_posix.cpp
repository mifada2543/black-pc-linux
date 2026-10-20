#include "shader/material_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "shader/glslang_posix.h"
#include "shader/nv2a_vsh_posix.h"

namespace xd3d {
namespace {

// Both halves' SPIR-V has to outlive the pipeline call and not one instruction longer: vkCreateGraphicsPipelines
// copies what it needs, and freeing earlier leaves the module pointing at nothing.
struct Spirv {
    uint32_t *words = nullptr;
    size_t bytes = 0;
    void release() { free_shader_spirv(words); words = nullptr; bytes = 0; }
};

// FNV-1a over the bytes that decide what the shader is. Chosen because it is three lines and the key is
// only ever compared, never trusted: a collision means one material stands in for another, which is the kind
// of bug worth being loud about. So the key also folds in the length and the flags, and the caller is
// expected to keep the first material it built under a given key rather than merging across them.
uint64_t hash_bytes(uint64_t h, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t hash_u64(uint64_t h, uint64_t v) { return hash_bytes(h, &v, sizeof v); }

} // namespace

uint64_t material_cache_key(const MaterialDesc &desc)
{
    // Folding the instruction *count* in, not just the pointer: the microcode lives in the game's image, so
    // two programs can share an address at different times and a key built from the address alone would call
    // them the same material.
    uint64_t h = 1469598103934665603ull;
    if (desc.vertex_program && desc.vertex_instruction_count) {
        h = hash_bytes(h, desc.vertex_program,
                        (size_t)desc.vertex_instruction_count * 4 * sizeof(uint32_t));
    }
    h = hash_u64(h, desc.vertex_input_mask);
    if (desc.vertex_elements && desc.vertex_element_count) {
        h = hash_bytes(h, desc.vertex_elements, desc.vertex_element_count * sizeof(VertexElement));
        if (desc.stream_stride) h = hash_bytes(h, desc.stream_stride, 16 * sizeof(uint32_t));
    }
    if (desc.pixel_shader) h = hash_bytes(h, desc.pixel_shader, sizeof(PixelShaderDef));
    if (desc.fragment_glsl) {
        // The generated fixed-function shader is a function of the texture stage state, and folding the
        // text in is how two stage configurations with the same everything else stay apart.
        h = hash_bytes(h, desc.fragment_glsl, strlen(desc.fragment_glsl));
        h = hash_u64(h, 0x46465053u); // "FFPS": an empty or truncated string still has to be a distinct key
    }
    h = hash_u64(h, desc.tex_cube);
    h = hash_u64(h, desc.flags);
    h = hash_u64(h, desc.color_format);
    h = hash_u64(h, desc.depth_format);
    h = hash_u64(h, desc.sample_count);
    return h;
}

bool build_material(gfx::Pipeline *out, const MaterialDesc &desc, const char **err)
{
    if (err) *err = nullptr;
    if (!out) {
        if (err) *err = "no pipeline to fill";
        return false;
    }

    Spirv vs, ps;
    const char *why = nullptr;

    // ---- the vertex half ----
    std::string vsrc;
    if (desc.vertex_program && desc.vertex_instruction_count) {
        vsrc = vsh_to_glsl(desc.vertex_program, desc.vertex_instruction_count, desc.vertex_input_mask, &why);
        if (vsrc.empty()) {
            if (err) *err = why ? why : "the vertex program did not translate";
            return false;
        }
        vs.words = compile_glsl_to_spirv(vsrc.c_str(), SHADER_STAGE_VERTEX, &vs.bytes, &why);
        if (!vs.words) {
            if (err) *err = why ? why : "the vertex shader did not compile";
            return false;
        }
    }

    // ---- the pixel half ----
    std::string psrc;
    const char *pixel_source = desc.fragment_glsl;
    if (desc.pixel_shader) {
        psrc = psh_to_glsl(*desc.pixel_shader, desc.tex_cube, &why);
        if (psrc.empty()) {
            if (err) *err = why ? why : "the pixel program did not translate";
            vs.release();
            return false;
        }
        pixel_source = psrc.c_str();
    }
    if (pixel_source) {
        ps.words = compile_glsl_to_spirv(pixel_source, SHADER_STAGE_FRAGMENT, &ps.bytes, &why);
        if (!ps.words) {
            if (err) *err = why ? why : "the pixel shader did not compile";
            vs.release();
            return false;
        }
    }

    // A material with no fragment stage still draws: it writes colour out of a vertex shader, which is what
    // the console did for anything without a pixel program, and what the A0 gate's own probe does. glslang
    // is handed a fragment shader that passes its varying through, because a graphics pipeline needs both
    // stages and there is no such thing as a pipeline with one.
    Spirv fallback_ps;
    const bool have_ps = ps.words != nullptr;
    if (!have_ps) {
        // The vertex stage's diffuse colour, at the location the vertex translator writes it to. This used
        // to read location 0, which no vertex shader writes: gl_Position is built in and is not a varying.
        static const char kPassThrough[] =
            "#version 450\n"
            "layout(location = 1) in vec4 d0;\n"
            "layout(location = 0) out vec4 outColour;\n"
            "void main() { outColour = d0; }\n";
        fallback_ps.words = compile_glsl_to_spirv(kPassThrough, SHADER_STAGE_FRAGMENT, &fallback_ps.bytes, &why);
        if (!fallback_ps.words) {
            if (err) *err = why ? why : "even the pass-through fragment shader did not compile";
            vs.release();
            return false;
        }
    }

    // ---- the vertex input state ----
    VertexLayout layout{};
    if (desc.vertex_elements && desc.vertex_element_count) {
        // A local reason, not the caller's: the caller gets one message for the whole material, and
        // "its vertex declaration is wrong" is a better one than a Vulkan call's name.
        const char *why_layout = nullptr;
        if (!build_vertex_layout(desc.vertex_elements, desc.vertex_element_count, desc.stream_stride, &layout,
                                 &why_layout)) {
            if (err) *err = why_layout ? why_layout : "the vertex declaration could not be laid out";
            vs.release();
            fallback_ps.release();
            return false;
        }
    }

    // The codes come out of the d3d layer and are turned into Vulkan formats here, which is the one place
    // that happens (section 3.2).
    std::vector<gfx::VertexAttribute> attrs;
    attrs.reserve(layout.attribute_count);
    for (uint32_t i = 0; i < layout.attribute_count; i++) {
        gfx::VertexAttribute a;
        a.binding = layout.attributes[i].binding;
        a.location = layout.attributes[i].location;
        a.format = layout.attributes[i].format; // a VertexFormatCode; create_pipeline resolves it
        a.offset = layout.attributes[i].offset;
        attrs.push_back(a);
    }
    std::vector<gfx::VertexBinding> binds;
    binds.reserve(layout.binding_count);
    for (uint32_t i = 0; i < layout.binding_count; i++) {
        gfx::VertexBinding b;
        b.binding = layout.bindings[i].binding;
        b.stride = layout.bindings[i].stride;
        b.rate = layout.bindings[i].rate;
        binds.push_back(b);
    }

    gfx::PipelineDesc d;
    d.vertex_spirv = vs.words ? vs.words : nullptr;
    d.vertex_spirv_size = vs.bytes;
    const Spirv &use_ps = have_ps ? ps : fallback_ps;
    d.fragment_spirv = use_ps.words;
    d.fragment_spirv_size = use_ps.bytes;
    // Straight pass-through: MaterialDesc's colour/depth fields are already gfx AttachmentFormat codes,
    // which is the whole point of that enum. This used to cast them to VkFormat to fill a PipelineDesc
    // field, which meant shader/ named a Vulkan type for no reason (plan section 3.2).
    d.color_format = desc.color_format;
    d.depth_format = desc.depth_format;
    d.attributes = attrs.empty() ? nullptr : attrs.data();
    d.attribute_count = (uint32_t)attrs.size();
    d.bindings = binds.empty() ? nullptr : binds.data();
    d.binding_count = (uint32_t)binds.size();
    d.flags = desc.flags;
    d.sample_count = desc.sample_count;

    const bool ok = gfx::create_pipeline(out, d);

    vs.release();
    ps.release();
    fallback_ps.release();

    if (!ok) {
        // The driver's own message names a Vulkan call. What is wanted here is the thing *before* that:
        // which shader, and what the compiler said about it.
        if (err) {
            static char msg[256];
            snprintf(msg, sizeof msg, "the pipeline was refused%s%s",
                     vsrc.empty() ? "" : " (vertex translated)", gfx::last_error());
            *err = msg;
        }
        return false;
    }
    return true;
}

} // namespace xd3d