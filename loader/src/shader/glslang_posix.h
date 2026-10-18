// GLSL to SPIR-V, at runtime, through libglslang.
//
// This is the whole of the shader runtime's dependency, and it is deliberately one function wide. The game
// hands the loader NV2A microcode while it is running; something has to turn that into SPIR-V while the
// game waits. Writing a compiler to do it would be a second compiler to get wrong, so libglslang is linked
// instead (plan section 7, decision 10).
//
// Two things about that choice are worth writing down rather than rediscovering:
//
//   * The `glslangValidator` binary that a build already uses proves nothing about runtime capability. It is
//     a build-time tool for fixed shaders; the microcode arrives at runtime.
//   * The library has to exist for **i386**. This loader is a 32-bit process, and an x86-64 libglslang on
//     the same machine is invisible to it. CMake looks for the 32-bit one and says so plainly if it is
//     missing, rather than letting the failure arrive as a link error about a symbol.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace xd3d {

enum ShaderStage {
    SHADER_STAGE_VERTEX = 0,
    SHADER_STAGE_FRAGMENT = 1,
};

// Compile one shader. Returns the SPIR-V as a heap buffer the caller frees with free_shader_spirv, or null
// with a reason in `err` -- never a partially valid module, because a pipeline built from one of those
// fails much later and much less legibly.
uint32_t *compile_glsl_to_spirv(const char *source, uint32_t stage, size_t *out_size, const char **err);

void free_shader_spirv(uint32_t *spirv);

// True when the library was linked in and usable. CMake decides whether this file is compiled at all; this
// reports whether the library answered, which is a different question -- a libglslang that refuses to
// initialise is a runtime fact, not a build one.
bool shader_compiler_available();

} // namespace xd3d