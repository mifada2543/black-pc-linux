// Binding one draw's material: its pipeline, its constants, and its textures.
//
// The draw path already knows how to record a draw and how to stage vertices. What it did not have was the
// step before the record: a pipeline to record the draw *with*, and a set of constants and textures that
// pipeline reads. That is this file. It is deliberately one function, because everything in it is decided by
// the same thing -- the device state at the moment of the draw -- and splitting it would leave the split
// points deciding which half sees which update.
//
// The pipeline is cached by the material's own key, which covers the vertex program's microcode, the vertex
// declaration, the strides, the render flags, and the fragment shader (the game's pixel program, or the
// fixed-function stages translated into one). A docked key is a wrong shader on some later draw, so the key
// is built by material_cache_key() rather than by hand here.
//
// The constants are not cached: they change between draws and are one memcpy each. They live in gfx's
// per-frame ring, so a draw recorded earlier in the frame keeps the values it was recorded with.
#include "d3d/draw_material_posix.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>

#include "d3d/device_posix.h"
#include "d3d/resource_posix.h"
#include "d3d/texture_posix.h"
#include "d3d/vshader_store_posix.h"
#include "platform/posix/log.h"
#include "shader/ffp_posix.h"
#include "shader/material_posix.h"

#define DTRACE(...) logf("[d3d] " __VA_ARGS__)

namespace xd3d {
namespace {

// ---- the constant blocks ----
//
// These mirror the GLSL blocks in nv2a_vsh_posix.cpp and nv2a_psh_posix.cpp, member for member. std140 is
// what makes that a flat array of vec4s with no padding surprises, and the static_asserts are what keep the
// two sides from drifting: a mismatch here reads the next member's bytes as this one's and draws a plausible
// wrong picture rather than failing.

constexpr uint32_t VS_BANK = 201; // 192 game registers + the viewport pair + spare addresses

struct VsConstants {
    float c[VS_BANK][4];
    float tex_scale[4][4];
    float fog_params[4];
    float fog_mode[4];
    float vm_scale[4];
};
static_assert(sizeof(VsConstants) == (VS_BANK + 7) * 16, "the vertex constant block must match nv2a_vsh's GLSL");

struct PsConstants {
    float pc[18][4];
    float fog_color[4];
    float bump_mat[4][4];
    float tfactor[4]; // appended for the fixed-function stage; the game's pixel programs ignore it
};
static_assert(sizeof(PsConstants) == (18 + 1 + 4 + 1) * 16,
              "the pixel constant block must match nv2a_psh's GLSL and ffp_posix's");

struct Cached {
    gfx::Pipeline pipeline;
    bool valid = false;
};

std::map<uint64_t, Cached> &pipelines()
{
    static std::map<uint64_t, Cached> map;
    return map;
}

// A D3D colour word (0xAARRGGBB) as four floats. The pixel translator's constants are colours, and the
// render-state array stores them in the game's own packing.
void unpack_color(uint32_t c, float *o)
{
    o[0] = ((c >> 16) & 255) / 255.0f;
    o[1] = ((c >> 8) & 255) / 255.0f;
    o[2] = (c & 255) / 255.0f;
    o[3] = (c >> 24) / 255.0f;
}

float as_float(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

} // namespace

bool bind_draw_material(uint32_t stream0_stride, const char **err)
{
    if (err) *err = nullptr;

    const uint32_t handle = state().vertex_shader;
    if (vertex_handle_is_fvf(handle)) {
        if (err) *err = "the fixed-function vertex path is not implemented yet";
        return false;
    }
    VertexProgram *vp = vertex_program(handle);
    if (!vp) {
        if (err) *err = "the vertex shader handle names no program";
        return false;
    }

    // ---- the fragment half ----
    FixedFunctionShader ff;
    const char *why = nullptr;
    bool have_ff = false;
    if (state().pixel_shader) {
        // The game's pixel programs are not translated yet: CreatePixelShader has nowhere to keep its
        // definition. Refusing is the honest answer until it does.
        if (err) *err = "the pixel program path is not implemented yet";
        return false;
    }
    if (build_fixed_function_shader(texture_state(0), &ff, &why)) {
        have_ff = true;
    } else if (!why || strcmp(why, "no texture stage is enabled") != 0) {
        // "No stage enabled" is not a failure: the draw's colour is the vertex stage's own, and the
        // material's pass-through fragment shader is that translation.
        if (err) *err = why ? why : "the fixed-function texture stages did not translate";
        return false;
    }

    // ---- the vertex input ----
    uint32_t strides[16] = {};
    for (uint32_t i = 0; i < 16; i++)
        strides[i] = state().streams[i].stride ? state().streams[i].stride : vp->declaration.stream_stride[i];
    if (stream0_stride) strides[0] = stream0_stride;

    uint32_t input_mask = 0;
    for (uint32_t i = 0; i < vp->declaration.count; i++)
        input_mask |= 1u << vp->declaration.elements[i].reg;

    // ---- the pipeline's render state ----
    const uint32_t *rs = render_state();
    uint32_t flags = 0;
    if (rs[XRS_ALPHABLENDENABLE]) flags |= gfx::PIPE_BLEND;
    // Depth is not enabled here on purpose: the window passes no depth attachment yet, and a pipeline with
    // depth state against no attachment is state that cannot be tested. It becomes real when the game's
    // D24S8 surface is wired as the gfx depth target.

    MaterialDesc md;
    md.vertex_program = vp->program.data() + 1; // past the program header, which is not an instruction
    md.vertex_instruction_count = vp->instructions;
    md.vertex_input_mask = input_mask;
    md.vertex_elements = vp->declaration.elements;
    md.vertex_element_count = vp->declaration.count;
    md.stream_stride = strides;
    md.fragment_glsl = have_ff ? ff.glsl.c_str() : nullptr;
    md.flags = flags;
    md.color_format = gfx::ATT_B8G8R8A8_UNORM;
    md.depth_format = gfx::ATT_NONE;
    md.sample_count = 1;

    const uint64_t key = material_cache_key(md);
    Cached *cached = nullptr;
    auto it = pipelines().find(key);
    if (it != pipelines().end()) {
        cached = &it->second;
    } else {
        Cached entry;
        const char *why_pipeline = nullptr;
        if (!build_material(&entry.pipeline, md, &why_pipeline)) {
            if (err) *err = why_pipeline ? why_pipeline : "the pipeline was refused";
            return false;
        }
        entry.valid = true;
        cached = &(pipelines()[key] = entry);
    }

    // ---- the target and the viewport ----
    //
    // The game's viewport is in Xbox pixels; the swapchain is whatever the window system gave us. The
    // shader's screen-space epilogue turns the game's coordinates into NDC with the Xbox viewport, so the
    // Vulkan viewport is that same rectangle scaled to the target. A negative height is deliberate: it
    // reproduces D3D's NDC-to-screen direction, where +1 is the top, which Vulkan otherwise flips.
    const float target_w = (float)(gfx::swapchain_width() ? gfx::swapchain_width() : 640);
    const float target_h = (float)(gfx::swapchain_height() ? gfx::swapchain_height() : 480);
    const float disp_w = (float)(state().display_width ? state().display_width : 640);
    const float disp_h = (float)(state().display_height ? state().display_height : 480);
    const float sx = target_w / disp_w, sy = target_h / disp_h;
    const Viewport &v = state().viewport;
    float vx = v.X * sx, vy = v.Y * sy, vw = v.Width * sx, vh = v.Height * sy;
    if (vw <= 0.0f || vh <= 0.0f) {
        vx = vy = 0.0f;
        vw = target_w;
        vh = target_h;
    }
    if (!gfx::set_viewport(vx, vy + vh, vw, -vh, v.MinZ, v.MaxZ)) {
        if (err) *err = gfx::last_error();
        return false;
    }
    const int32_t ix = (int32_t)lroundf(vx), iy = (int32_t)lroundf(vy);
    const int32_t iw = (int32_t)lroundf(vw), ih = (int32_t)lroundf(vh);
    gfx::set_scissor((uint32_t)(ix < 0 ? 0 : ix), (uint32_t)(iy < 0 ? 0 : iy), (uint32_t)(iw < 0 ? 0 : iw),
                     (uint32_t)(ih < 0 ? 0 : ih));

    // ---- the constants ----
    VsConstants vc = {};
    memcpy(vc.c, state().vs_constants, sizeof state().vs_constants);
    float vp_scale[4], vp_offset[4];
    viewport_transform(vp_scale, vp_offset);
    memcpy(vc.c[192], vp_scale, sizeof vp_scale);
    memcpy(vc.c[193], vp_offset, sizeof vp_offset);

    // A pitch-linear texture is addressed in texels on the console, so the vertex shader's texture
    // coordinate has to be divided by the texture's size. A swizzled texture already uses 0..1.
    for (uint32_t s = 0; s < 4; s++) {
        float tx = 1.0f, ty = 1.0f;
        Resource *t = state().textures[s];
        if (t) {
            const PixelContainer *pc = (const PixelContainer *)t;
            if (pc->Size) {
                uint32_t w = 0, h = 0, d = 0, levels = 0;
                pixel_container_dims(pc, &w, &h, &d, &levels);
                if (w && h) {
                    tx = 1.0f / (float)w;
                    ty = 1.0f / (float)h;
                }
            }
        }
        vc.tex_scale[s][0] = tx;
        vc.tex_scale[s][1] = ty;
        vc.tex_scale[s][2] = 1.0f;
        vc.tex_scale[s][3] = 1.0f;
    }

    vc.fog_params[0] = as_float(rs[XRS_FOGSTART]);
    vc.fog_params[1] = as_float(rs[XRS_FOGEND]);
    vc.fog_params[2] = as_float(rs[XRS_FOGDENSITY]);
    vc.fog_params[3] = 1.0f;
    switch (rs[XRS_FOGTABLEMODE]) {
    case 1: vc.fog_mode[0] = 0.0f; vc.fog_mode[1] = 1.0f; break; // EXP
    case 2: vc.fog_mode[0] = 0.0f; vc.fog_mode[2] = 1.0f; break; // EXP2
    case 3: vc.fog_mode[0] = 1.0f; break;                         // LINEAR
    default: vc.fog_mode[3] = 1.0f; break;                        // NONE: per-vertex oFog
    }
    vc.vm_scale[0] = vc.vm_scale[1] = vc.vm_scale[2] = vc.vm_scale[3] = 1.0f;

    PsConstants pc = {};
    for (uint32_t i = 0; i < 8; i++) {
        unpack_color(rs[XRS_PSCONSTANT0_0 + i], pc.pc[i]);
        unpack_color(rs[XRS_PSCONSTANT1_0 + i], pc.pc[8 + i]);
    }
    unpack_color(rs[XRS_PSFINALCOMBINERCONSTANT0], pc.pc[16]);
    unpack_color(rs[XRS_PSFINALCOMBINERCONSTANT1], pc.pc[17]);
    unpack_color(rs[XRS_FOGCOLOR], pc.fog_color);
    for (uint32_t s = 0; s < 4; s++) {
        const uint32_t *ts = texture_state((int)s);
        memcpy(&pc.bump_mat[s][0], &ts[22], 4); // BUMPENVMAT00
        memcpy(&pc.bump_mat[s][1], &ts[23], 4); // BUMPENVMAT01
        memcpy(&pc.bump_mat[s][2], &ts[25], 4); // BUMPENVMAT10
        memcpy(&pc.bump_mat[s][3], &ts[24], 4); // BUMPENVMAT11
    }
    unpack_color(rs[XRS_TEXTUREFACTOR], pc.tfactor);

    // ---- the textures ----
    gfx::TextureSlot slots[4] = {};
    uint32_t slot_count = have_ff ? ff.stages : 0;
    for (uint32_t s = 0; s < slot_count; s++) {
        Resource *t = state().textures[s];
        const char *why_tex = nullptr;
        if (t) {
            const gfx::Image *img = texture_image(t, &why_tex);
            if (!img) {
                if (err) *err = why_tex ? why_tex : "the stage's texture could not be uploaded";
                return false;
            }
            slots[s].image = img;
        }
        const gfx::Sampler *smp = texture_sampler(s, &why_tex);
        if (!smp) {
            if (err) *err = why_tex ? why_tex : "the stage's sampler could not be created";
            return false;
        }
        slots[s].sampler = smp;
    }

    if (!gfx::set_pipeline(&cached->pipeline)) {
        if (err) *err = gfx::last_error();
        return false;
    }
    // Textures first: that call allocates the descriptor set, and bind_uniforms writes the two constant
    // buffers into the same set. Reversing them would write into a set that is about to be freed.
    if (!gfx::bind_textures(&cached->pipeline, slots, slot_count)) {
        if (err) *err = gfx::last_error();
        return false;
    }
    if (!gfx::bind_uniforms(&cached->pipeline, &vc, sizeof vc, &pc, sizeof pc)) {
        if (err) *err = gfx::last_error();
        return false;
    }
    return true;
}

void material_forget_all()
{
    for (auto &p : pipelines())
        if (p.second.valid) gfx::destroy_pipeline(&p.second.pipeline);
    pipelines().clear();
    texture_forget_all();
}

} // namespace xd3d
