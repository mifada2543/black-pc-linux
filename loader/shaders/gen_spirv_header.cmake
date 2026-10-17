# Turn SPIR-V into a C header. Run by CMake at build time, once per shader.
#
#   cmake -DINPUT=<file.spv> -DOUTPUT=<file.h> -DNAME=kProbeVertSpv -P gen_spirv_header.cmake
#
# The name is used verbatim as the array's identifier, so it has to be capitalised the way a C identifier
# should be -- the stage is appended by the caller, which knows it as "vert", not as "Vert".
#
# The point of the indirection is ordering: the header has to exist before anything that includes it is
# compiled, and it can only be written after glslangValidator has produced the SPIR-V. Doing this with
# file(READ) at configure time would need the .spv to already exist in a clean tree, which it never does.
#
# Output is one uint8_t per byte of the .spv, so `sizeof` the array *is* the file's size and codeSize is that
# number. An earlier version emitted one entry per byte into a uint32_t array, which made every consumer pass
# four times the SPIR-V it had: llvmpipe's shader JIT read past the end and died inside
# vkCreateGraphicsPipelines, with no error anywhere to say why. SPIR-V is a little-endian word stream, and a
# byte copy of the file preserves it exactly -- which is why this cannot get the order wrong.

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED NAME)
  message(FATAL_ERROR "gen_spirv_header.cmake needs INPUT, OUTPUT and NAME")
endif()

file(READ "${INPUT}" hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," words "${hex}")

# 12 bytes per line: readable in a diff, and short enough not to wrap.
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" formatted "${words}")

file(WRITE "${OUTPUT}"
"// Generated from ${INPUT} -- do not edit. See shaders/spirv.h.in for why this exists.\n\
#pragma once\n\
\n\
#include <stdint.h>\n\
\n\
static const uint8_t ${NAME}[] = {\n\
${formatted}};\n")
