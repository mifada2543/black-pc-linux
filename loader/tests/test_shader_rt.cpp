// The shader runtime: GLSL in, SPIR-V out, on a machine with no image and no game.
//
// This is the first thing that can prove the runtime half of A5 works at all. It needs no GPU, no window
// and no XBE: it compiles a shader and checks the words that come back. What it cannot prove is that those
// words make a pipeline -- that is A0's gate, and it is still waiting on a machine whose GPU submits.
//
// The two checks that matter are that a valid shader compiles, and that an invalid one is refused *with a
// log*. The second is not a nicety: the whole value of a runtime compiler is the message it prints when the
// translation it was handed is wrong, and a wrapper that swallowed it would turn every future shader bug
// into "the pipeline failed to create".
#include "shader/glslang_posix.h"

#include <stdio.h>
#include <string.h>

#include <string>

static int g_checks = 0, g_failures = 0;

static void check(bool ok, const char *what)
{
    printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
    g_checks++;
    if (!ok) g_failures++;
}

// A SPIR-V module starts with a five-word header. Checking it is what makes "it compiled" mean something:
// glslang handing back a buffer that is not a module is not a failure it reports, it is a failure we would
// only find inside the driver.
static bool looks_like_spirv(const uint32_t *w, size_t bytes)
{
    if (bytes < 5 * sizeof(uint32_t)) return false;
    if (w[0] != 0x07230203u) return false;
    // SPIR-V 1.0 and nothing newer, and that is checked rather than assumed: llvmpipe's shader JIT on a
    // 32-bit Mesa has been observed to fault inside vkCreateGraphicsPipelines on the newer targets, and the
    // whole port aims at the oldest thing that works. A compiler upgraded into emitting 1.3 would pass
    // every other check here and fail on a machine none of this runs on.
    if (w[1] != 0x00010000u) return false;
    if (w[4] != 0u) return false; // the schema word is zero by convention
    return true;
}

static const char kVertex[] =
    "#version 450\n"
    "layout(location = 0) in vec3 in_position;\n"
    "layout(location = 0) out vec4 out_colour;\n"
    "void main() {\n"
    "    gl_Position = vec4(in_position, 1.0);\n"
    "    out_colour = vec4(1.0, 0.5, 0.25, 1.0);\n"
    "}\n";

static const char kFragment[] =
    "#version 450\n"
    "layout(location = 0) in vec4 in_colour;\n"
    "layout(location = 0) out vec4 out_colour;\n"
    "void main() {\n"
    "    out_colour = in_colour;\n"
    "}\n";

static void test_a_valid_shader_compiles()
{
    printf("\na valid shader compiles\n");

    check(xd3d::shader_compiler_available(), "the shader runtime answers for itself");

    size_t bytes = 0;
    const char *err = nullptr;
    uint32_t *spirv = xd3d::compile_glsl_to_spirv(kVertex, xd3d::SHADER_STAGE_VERTEX, &bytes, &err);
    check(spirv != nullptr, "a vertex shader compiles");
    if (!spirv) return;
    check(bytes > 0, "and produces some words");
    check(looks_like_spirv(spirv, bytes), "those words are a SPIR-V module, not merely a buffer");
    // The magic word is worth stating separately: it is the one thing that distinguishes a module from a
    // successful allocation.
    check(spirv[0] == 0x07230203u, "with the SPIR-V magic word first");
    xd3d::free_shader_spirv(spirv);
    printf("       (%zu bytes of SPIR-V)\n", bytes);

    err = nullptr;
    spirv = xd3d::compile_glsl_to_spirv(kFragment, xd3d::SHADER_STAGE_FRAGMENT, &bytes, &err);
    check(spirv != nullptr, "a fragment shader compiles");
    if (spirv) {
        check(looks_like_spirv(spirv, bytes), "and it is a module too");
        xd3d::free_shader_spirv(spirv);
    }
}

static void test_a_broken_shader_is_refused()
{
    printf("\na broken shader is refused, with a log\n");

    size_t bytes = 12345; // deliberately wrong: a refusal must clear it, not leave the caller's value
    const char *err = nullptr;
    uint32_t *spirv = xd3d::compile_glsl_to_spirv("#version 450\nvoid main() { this is not glsl; }",
                                                   xd3d::SHADER_STAGE_VERTEX, &bytes, &err);
    check(spirv == nullptr, "a shader that does not compile is refused");
    check(bytes == 0, "and the size is cleared, so a caller cannot use a stale number");
    // The log is the point. Without it the only symptom would be a pipeline that refuses to create, in a
    // different layer, much later.
    check(err != nullptr && err[0] != '\0', "and it comes with something to read");
    if (err && err[0]) printf("       log: %.90s\n", err);

    spirv = xd3d::compile_glsl_to_spirv(nullptr, xd3d::SHADER_STAGE_VERTEX, &bytes, &err);
    check(spirv == nullptr && err != nullptr, "no source at all is refused rather than crashing");
}

static void test_it_can_be_called_again()
{
    printf("\nit survives being called again\n");

    // glslang keeps global state. The second call is the one that finds out whether the first released
    // something it should not have, and whether the library needs a finalise it never gets.
    for (int i = 0; i < 4; i++) {
        size_t bytes = 0;
        const char *err = nullptr;
        uint32_t *spirv = xd3d::compile_glsl_to_spirv(kVertex, xd3d::SHADER_STAGE_VERTEX, &bytes, &err);
        if (!spirv) {
            check(false, "a repeated compile still works");
            return;
        }
        check(looks_like_spirv(spirv, bytes), "a repeated compile still produces a module");
        xd3d::free_shader_spirv(spirv);
    }
    check(true, "four compiles in a row, each freed, all well formed");
}

int main()
{
    printf("BLACK loader -- the shader runtime\n");
    test_a_valid_shader_compiles();
    test_a_broken_shader_is_refused();
    test_it_can_be_called_again();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}