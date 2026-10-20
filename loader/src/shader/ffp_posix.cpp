#include "shader/ffp_posix.h"

#include <stdio.h>
#include <string.h>

#include "shader/varyings.h"

namespace xd3d {
namespace {

constexpr uint32_t kStageCount = 4;
constexpr uint32_t kPerStage = 32;

const uint32_t *stage(const uint32_t *stages, uint32_t i) { return stages + i * kPerStage; }

// One argument, as the expression that reads it. The stage's sampled texture is passed in because the
// argument names a stage, not a texture register: txt0 at stage 1 is that stage's own sample.
std::string arg_expr(const uint32_t *s, uint32_t raw, const std::string &tex, bool *diffuse, bool *specular,
                     const char **err)
{
    const uint32_t sel = raw & XTA_SELECTMASK;
    std::string e;
    switch (sel) {
    case XTA_DIFFUSE: e = "d0"; *diffuse = true; break;
    case XTA_CURRENT: e = "cur"; break;
    case XTA_TEXTURE: e = tex; break;
    case XTA_TFACTOR: e = "tfactor"; break;
    case XTA_SPECULAR: e = "d1"; *specular = true; break;
    case XTA_TEMP: e = "tmp"; break;
    default:
        if (err) *err = "a texture argument this port has not translated";
        return std::string();
    }
    (void)s;
    if (raw & XTA_COMPLEMENT) e = "(vec4(1.0) - " + e + ")";
    if (raw & XTA_ALPHAREPLICATE) e = "(" + e + ").aaaa";
    return e;
}

// One operation, on whole vec4s. The caller takes `.rgb` from the colour path and `.a` from the alpha path,
// which is what lets both paths share this: the console's arithmetic was the same on four lanes, and only
// the final channel choice differed.
std::string op_expr(uint32_t op, const std::string &a1, const std::string &a2, const std::string &a0,
                    const std::string &tex, const char **err)
{
    switch (op) {
    case XTO_SELECTARG1: return a1;
    case XTO_SELECTARG2: return a2;
    case XTO_MODULATE: return "(" + a1 + " * " + a2 + ")";
    case XTO_MODULATE2X: return "(" + a1 + " * " + a2 + " * 2.0)";
    case XTO_MODULATE4X: return "(" + a1 + " * " + a2 + " * 4.0)";
    case XTO_ADD: return "(" + a1 + " + " + a2 + ")";
    case XTO_ADDSIGNED: return "(" + a1 + " + " + a2 + " - 0.5)";
    case XTO_ADDSIGNED2X: return "((" + a1 + " + " + a2 + " - 0.5) * 2.0)";
    case XTO_SUBTRACT: return "(" + a1 + " - " + a2 + ")";
    case XTO_ADDSMOOTH: return "(" + a1 + " + " + a2 + " - " + a1 + " * " + a2 + ")";
    // The blend operations interpolate arg1 over arg2 by a factor the console named in the op. That is the
    // order D3D documents, and it is the one that makes BLENDTEXTUREALPHA on an opaque texture read arg1.
    case XTO_BLENDDIFFUSEALPHA: return "mix(" + a2 + ", " + a1 + ", d0.a)";
    case XTO_BLENDCURRENTALPHA: return "mix(" + a2 + ", " + a1 + ", cur.a)";
    case XTO_BLENDTEXTUREALPHA: return "mix(" + a2 + ", " + a1 + ", " + tex + ".a)";
    case XTO_BLENDFACTORALPHA: return "mix(" + a2 + ", " + a1 + ", tfactor.a)";
    case XTO_BLENDTEXTUREALPHAPM: return "(" + a1 + " + " + a2 + " * (1.0 - " + tex + ".a))";
    case XTO_DOTPRODUCT3: return "vec4(dot(" + a1 + ".rgb, " + a2 + ".rgb))";
    case XTO_MULTIPLYADD: return "(" + a1 + " * " + a2 + " + " + a0 + ")";
    case XTO_LERP: return "mix(" + a2 + ", " + a1 + ", " + a0 + ")";
    default:
        if (err) *err = "a texture operation this port has not translated";
        return std::string();
    }
}

// True when the operation reads the diffuse alpha even though no argument names DIFFUSE.
bool op_needs_diffuse(uint32_t op) { return op == XTO_BLENDDIFFUSEALPHA; }

} // namespace

bool build_fixed_function_shader(const uint32_t *stages, FixedFunctionShader *out, const char **err)
{
    if (err) *err = nullptr;
    if (!stages || !out) {
        if (err) *err = "no texture stage state";
        return false;
    }

    FixedFunctionShader r;

    // A DISABLE ends the chain; the console ignored the stages after it. Op 1 is the console's second
    // spelling of DISABLE (its table has it twice), and treating it as an operation would sample a texture
    // the game never bound.
    for (uint32_t i = 0; i < kStageCount; i++) {
        const uint32_t color = stage(stages, i)[XTSS_COLOROP];
        if (color == XTO_DISABLE || color == 1) break;
        r.stages = i + 1;
    }
    if (r.stages == 0) {
        if (err) *err = "no texture stage is enabled";
        return false;
    }

    bool diffuse = false, specular = false;
    std::string body;

    // The colour each stage's texture argument reads. Sampling is emitted where the stage is, so an
    // unused stage costs nothing and a disabled one is never sampled -- binding a texture to a stage the
    // shader does not sample would otherwise be a descriptor with no reader.
    for (uint32_t i = 0; i < r.stages; i++) {
        char line[128];
        snprintf(line, sizeof line, "  vec4 ts%u = texture(s%u, t%u.xy);\n", (unsigned)i, (unsigned)i,
                 (unsigned)i);
        body += line;
    }

    // Stage 0's CURRENT is the diffuse colour: the console seeded the combiner with it before the first
    // stage ran. Stage 1 and later see the previous stage's result in `cur`.
    bool stage0_reads_current = false;
    for (int path = 0; path < 2; path++) {
        const uint32_t *s = stage(stages, 0);
        const uint32_t op = path ? s[XTSS_ALPHAOP] : s[XTSS_COLOROP];
        if (op == XTO_DISABLE || op == 1) continue;
        const uint32_t a1 = path ? s[XTSS_ALPHAARG1] : s[XTSS_COLORARG1];
        const uint32_t a2 = path ? s[XTSS_ALPHAARG2] : s[XTSS_COLORARG2];
        const uint32_t a0 = path ? s[XTSS_ALPHAARG0] : s[XTSS_COLORARG0];
        const uint32_t args[3] = {a1, a2, a0};
        for (uint32_t k = 0; k < 3; k++)
            if ((args[k] & XTA_SELECTMASK) == XTA_CURRENT) stage0_reads_current = true;
        if (op == XTO_BLENDCURRENTALPHA) stage0_reads_current = true;
    }

    for (uint32_t i = 0; i < r.stages; i++) {
        const uint32_t *s = stage(stages, i);
        const uint32_t color_op = s[XTSS_COLOROP];
        const uint32_t alpha_op = s[XTSS_ALPHAOP];
        const bool color_on = !(color_op == XTO_DISABLE || color_op == 1);
        const bool alpha_on = !(alpha_op == XTO_DISABLE || alpha_op == 1);

        r.color_op[i] = color_op;
        r.alpha_op[i] = alpha_op;
        r.color_arg[i][0] = s[XTSS_COLORARG0];
        r.color_arg[i][1] = s[XTSS_COLORARG1];
        r.color_arg[i][2] = s[XTSS_COLORARG2];
        r.alpha_arg[i][0] = s[XTSS_ALPHAARG0];
        r.alpha_arg[i][1] = s[XTSS_ALPHAARG1];
        r.alpha_arg[i][2] = s[XTSS_ALPHAARG2];

        char ts[8];
        snprintf(ts, sizeof ts, "ts%u", (unsigned)i);
        char block[1024];
        snprintf(block, sizeof block, "  {\n    // stage %u\n", (unsigned)i);
        body += block;

        std::string color_expr;
        if (color_on) {
            std::string a1 = arg_expr(s, s[XTSS_COLORARG1], ts, &diffuse, &specular, err);
            if (a1.empty()) return false;
            std::string a2 = arg_expr(s, s[XTSS_COLORARG2], ts, &diffuse, &specular, err);
            if (a2.empty()) return false;
            std::string a0 = arg_expr(s, s[XTSS_COLORARG0], ts, &diffuse, &specular, err);
            if (a0.empty()) return false;
            color_expr = op_expr(color_op, a1, a2, a0, ts, err);
            if (color_expr.empty()) return false;
        }

        std::string alpha_expr;
        if (alpha_on) {
            std::string a1 = arg_expr(s, s[XTSS_ALPHAARG1], ts, &diffuse, &specular, err);
            if (a1.empty()) return false;
            std::string a2 = arg_expr(s, s[XTSS_ALPHAARG2], ts, &diffuse, &specular, err);
            if (a2.empty()) return false;
            std::string a0 = arg_expr(s, s[XTSS_ALPHAARG0], ts, &diffuse, &specular, err);
            if (a0.empty()) return false;
            alpha_expr = op_expr(alpha_op, a1, a2, a0, ts, err);
            if (alpha_expr.empty()) return false;
        }

        if (op_needs_diffuse(color_op) || op_needs_diffuse(alpha_op)) diffuse = true;

        // The result register. 1 is CURRENT (the console's usual); 5 is TEMP, which a later stage can read.
        const uint32_t result = s[XTSS_RESULTARG] & XTA_SELECTMASK;
        const char *dst = result == XTA_TEMP ? "tmp" : "cur";
        if (result != XTA_TEMP && result != XTA_CURRENT) {
            if (err) *err = "a texture result register this port has not translated";
            return false;
        }

        std::string rhs;
        if (color_on && alpha_on) {
            rhs = "vec4((" + color_expr + ").rgb, (" + alpha_expr + ").a)";
        } else if (color_on) {
            rhs = "vec4((" + color_expr + ").rgb, cur.a)";
        } else {
            rhs = "vec4(cur.rgb, (" + alpha_expr + ").a)";
        }
        body += "    " + std::string(dst) + " = clamp(" + rhs + ", 0.0, 1.0);\n  }\n";
    }

    r.uses_diffuse = diffuse || stage0_reads_current;
    r.uses_specular = specular;

    std::string s;
    s += "#version 450\n";
    s += "// Translated from the console's fixed-function texture stages (see src/shader/ffp_posix.cpp).\n";
    s += "layout(location = 0) out vec4 outColour;\n";
    if (r.uses_diffuse) {
        char b[64];
        snprintf(b, sizeof b, "layout(location = %u) in vec4 d0;\n", (unsigned)VARYING_D0);
        s += b;
    }
    if (r.uses_specular) {
        char b[64];
        snprintf(b, sizeof b, "layout(location = %u) in vec4 d1;\n", (unsigned)VARYING_D1);
        s += b;
    }
    for (uint32_t i = 0; i < r.stages; i++) {
        char b[64];
        snprintf(b, sizeof b, "layout(location = %u) in vec4 t%u;\n", (unsigned)varying_texture(i), (unsigned)i);
        s += b;
    }
    for (uint32_t i = 0; i < r.stages; i++) {
        char b[96];
        snprintf(b, sizeof b, "layout(binding = %u) uniform sampler2D s%u;\n", (unsigned)i, (unsigned)i);
        s += b;
    }
    // The pixel half's constant block, at the binding nv2a_psh_posix declares. The members before
    // tfactor are not used here, but std140 places tfactor after them and a block that omitted them would
    // read the wrong bytes.
    s += "layout(std140, set = 0, binding = 9) uniform Consts {\n"
         "  vec4 pc[18];\n"
         "  vec4 fogColor;\n"
         "  vec4 bumpMat[4];\n"
         "  vec4 tfactor;\n"
         "};\n";
    s += "void main() {\n";
    s += r.uses_diffuse ? "  vec4 cur = d0;\n" : "  vec4 cur = vec4(1.0);\n";
    s += "  vec4 tmp = vec4(0.0);\n";
    s += body;
    s += "  outColour = cur;\n";
    s += "}\n";

    r.glsl = s;
    *out = r;
    return true;
}

} // namespace xd3d
