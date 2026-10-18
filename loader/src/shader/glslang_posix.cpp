#include "shader/glslang_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

// glslang's C++ interface. It is the C++ one and not the C one because the C interface cannot name an
// entry point -- glslang_shader_create() leaves it empty and linking then fails with "Each stage requires
// one entry point", which is a confusing way to say the wrapper never set one.
//
// The signatures below were read from the installed headers rather than from memory. That matters: this
// file was written against a remembered version of the API first, and did not compile, because setEnvInput
// takes an EShLanguage here (not a version) and the SPIR-V emitter is a free function
// (glslang::GlslangToSpv) rather than a method on a tools object.
//
// Everything above this file deals in SPIR-V words and never in glslang types.
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

namespace xd3d {
namespace {

constexpr size_t kLogMax = 2048;

// glslang keeps global state and expects initialise/finalise to bracket it. Doing that per compile would be
// correct and slow; doing it once is what the library documents.
bool g_initialised = false;

bool ensure_initialised(const char **err)
{
    if (g_initialised) return true;
    if (!glslang::InitializeProcess()) {
        if (err) *err = "glslang refused to initialise";
        return false;
    }
    g_initialised = true;
    return true;
}

EShLanguage stage_of(uint32_t stage)
{
    return stage == SHADER_STAGE_FRAGMENT ? EShLangFragment : EShLangVertex;
}

// The message flags: SPIR-V generation rules and Vulkan's requirements. Together they are what turns a
// shader that parses into one that can actually be a pipeline, so leaving either off produces a module that
// fails later, inside the driver.
constexpr EShMessages kMessages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

// The log has to be copied out, not pointed at. glslang's getInfoLog() returns a pointer into the TShader's
// own info sink, and the TShader is destroyed before this function returns -- so handing that pointer back
// gives the caller a dangling one. The first version of this file did exactly that, and the test caught it
// by printing a log of two garbage characters instead of the GLSL error that was there a moment earlier.
//
// A function-local static, not a caller-provided buffer: it keeps the signature one pointer wide, and shader
// creation happens on the boot thread. If a second thread ever compiles, this needs to become thread-local.
char g_log[kLogMax];

const char *copy_log(const char *text)
{
    if (!text) text = "";
    strncpy(g_log, text, kLogMax - 1);
    g_log[kLogMax - 1] = '\0';
    return g_log;
}

} // namespace

bool shader_compiler_available()
{
    const char *err = nullptr;
    return ensure_initialised(&err);
}

uint32_t *compile_glsl_to_spirv(const char *source, uint32_t stage, size_t *out_size, const char **err)
{
    if (out_size) *out_size = 0;
    if (err) *err = nullptr;
    if (!source) {
        if (err) *err = "no shader source";
        return nullptr;
    }
    if (!ensure_initialised(err)) return nullptr;

    const EShLanguage lang = stage_of(stage);

    glslang::TShader shader(lang);
    const char *sources[] = {source};
    const char *names[] = {stage == SHADER_STAGE_FRAGMENT ? "fragment" : "vertex"};
    const int lengths[] = {(int)strlen(source)};
    shader.setStringsWithLengthsAndNames(sources, lengths, names, 1);
    shader.setEntryPoint("main");
    // Vulkan 1.0 for the same reason the build-time compiler is told the same thing: nothing in this port
    // needs an instruction newer than SPIR-V 1.0, and llvmpipe's shader JIT on a 32-bit Mesa has been
    // observed to fault inside vkCreateGraphicsPipelines on the newer targets.
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientNone, glslang::EShTargetVulkan_1_0);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    // The four-argument parse does not force a default version or profile, so the shader's own #version
    // decides. Forcing one was wrong in a way that only showed up as a compile error about ES shaders
    // needing version 310: the probe's shaders declare 450, and overriding that with 100 made the compiler
    // read them as an ES shader from before layout qualifiers existed.
    //
    // The resource table is glslang's defaults and is NOT optional. Passing null compiles, links, and then
    // segfaults inside TBuiltIns::initialize, several calls away from the line that caused it -- found by a
    // backtrace, not by reading. The XDK's own limits are far below glslang's defaults, so the defaults do
    // not constrain a console-era shader and nothing has to be tuned to match them.
    if (!shader.parse(GetDefaultResources(), 450, false, kMessages)) {
        // The log is the whole value of this function when it fails: a GLSL error without its line and its
        // message cannot be acted on by anyone.
        if (err) *err = copy_log(shader.getInfoLog());
        if (err && (!*err || !**err)) *err = copy_log("the shader did not compile, and glslang said nothing about why");
        return nullptr;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(kMessages)) {
        if (err) *err = copy_log(program.getInfoLog());
        if (err && (!*err || !**err)) *err = copy_log("the shader compiled and then did not link");
        return nullptr;
    }
    if (!program.mapIO()) {
        if (err) *err = copy_log(program.getInfoLog());
        if (err && (!*err || !**err)) *err = copy_log("the program's inputs and outputs could not be mapped");
        return nullptr;
    }

    const glslang::TIntermediate *intermediate = program.getIntermediate(lang);
    if (!intermediate) {
        if (err) *err = "no intermediate representation was produced";
        return nullptr;
    }

    std::vector<unsigned int> spirv;
    glslang::GlslangToSpv(*intermediate, spirv);
    // Zero words is not a shader. Handing it on would put an empty module into a pipeline and move the
    // failure several calls away from its cause.
    if (spirv.empty()) {
        if (err) *err = "glslang produced no SPIR-V";
        return nullptr;
    }

    // Copied out rather than handed back as a pointer into glslang's vector: the program is destroyed at the
    // end of this function, so a pointer into it would be a use-after-free at the first draw.
    const size_t bytes = spirv.size() * sizeof(uint32_t);
    uint32_t *out = (uint32_t *)malloc(bytes);
    if (!out) {
        if (err) *err = "no memory for the compiled shader";
        return nullptr;
    }
    memcpy(out, spirv.data(), bytes);
    if (out_size) *out_size = bytes;
    return out;
}

void free_shader_spirv(uint32_t *spirv)
{
    free(spirv);
}

} // namespace xd3d