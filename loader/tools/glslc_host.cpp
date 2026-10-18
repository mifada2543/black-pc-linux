// GLSL to SPIR-V, as a build-time tool. A stand-in for glslangValidator.
//
// The reason this exists: the loader's shader runtime is libglslang (plan section 7, decision 10), and on a
// machine where only the i386 libraries are installed the amd64 `glslangValidator` binary is gone -- apt
// removes it as a conflict. Without this tool the build-time shaders vanish too, and the A0 gate stops
// linking, which is a confusing way to learn that a package was upgraded.
//
// The interface is deliberately the subset of glslangValidator's that the build actually uses --
//
//     glslc_host -V --target-env vulkan1.0 -o out.spv in.glsl
//
// -- so the CMake command line does not have to know which of the two is present. -V is accepted and
// ignored: this tool only ever emits SPIR-V, and refusing the flag would make it look like a different
// compiler at the call site.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

static char *read_all(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return nullptr;
    }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return nullptr;
    }
    const size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (size_out) *size_out = got;
    return buf;
}

int main(int argc, char **argv)
{
    const char *target_env = "vulkan1.0";
    const char *out_path = nullptr;
    const char *in_path = nullptr;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-V") == 0) continue;
        if (strcmp(a, "--target-env") == 0 && i + 1 < argc) {
            target_env = argv[++i];
        } else if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (a[0] != '-' && !in_path) {
            in_path = a;
        }
    }
    if (!in_path || !out_path) {
        fprintf(stderr, "usage: %s -V --target-env <env> -o <out.spv> <in>\n", argv[0]);
        return 2;
    }

    const bool vulkan10 = strncmp(target_env, "vulkan1.0", 9) == 0;

    size_t size = 0;
    char *source = read_all(in_path, &size);
    if (!source) {
        fprintf(stderr, "glslc_host: cannot read %s\n", in_path);
        return 1;
    }

    // The stage comes from the file's extension, because glslangValidator infers it the same way and the
    // build's shader names already carry it (probe.vert, probe.frag).
    const char *dot = strrchr(in_path, '.');
    const EShLanguage lang = (dot && strcmp(dot, ".frag") == 0) ? EShLangFragment : EShLangVertex;

    if (!glslang::InitializeProcess()) {
        fprintf(stderr, "glslc_host: glslang refused to initialise\n");
        free(source);
        return 1;
    }

    constexpr EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

    glslang::TShader shader(lang);
    const char *sources[] = {source};
    const char *names[] = {dot ? dot + 1 : "shader"};
    const int lengths[] = {(int)size};
    shader.setStringsWithLengthsAndNames(sources, lengths, names, 1);
    shader.setEntryPoint("main");
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientNone,
                       vulkan10 ? glslang::EShTargetVulkan_1_0 : glslang::EShTargetVulkan_1_2);
    shader.setEnvClient(glslang::EShClientVulkan,
                        vulkan10 ? glslang::EShTargetVulkan_1_0 : glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    // The four-argument parse leaves the shader's own #version in charge. Forcing a default instead is what
    // made this fail the first time, reading a "#version 450" shader as an ES shader from before layout
    // qualifiers existed.
    if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
        fprintf(stderr, "glslc_host: %s did not compile\n%s\n", in_path, shader.getInfoLog());
        glslang::FinalizeProcess();
        free(source);
        return 1;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages) || !program.mapIO()) {
        fprintf(stderr, "glslc_host: %s did not link\n%s\n", in_path, program.getInfoLog());
        glslang::FinalizeProcess();
        free(source);
        return 1;
    }

    const glslang::TIntermediate *intermediate = program.getIntermediate(lang);
    if (!intermediate) {
        fprintf(stderr, "glslc_host: %s produced no intermediate form\n", in_path);
        glslang::FinalizeProcess();
        free(source);
        return 1;
    }

    std::vector<unsigned int> spirv;
    glslang::GlslangToSpv(*intermediate, spirv);
    if (spirv.empty()) {
        fprintf(stderr, "glslc_host: %s produced no SPIR-V\n", in_path);
        glslang::FinalizeProcess();
        free(source);
        return 1;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "glslc_host: cannot write %s\n", out_path);
        glslang::FinalizeProcess();
        free(source);
        return 1;
    }
    const size_t wrote = fwrite(spirv.data(), sizeof(unsigned int), spirv.size(), out);
    fclose(out);

    glslang::FinalizeProcess();
    free(source);

    if (wrote != spirv.size()) {
        fprintf(stderr, "glslc_host: wrote %zu of %zu words to %s\n", wrote, spirv.size(), out_path);
        return 1;
    }
    return 0;
}