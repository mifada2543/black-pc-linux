// The shader objects the game creates, and the handles it holds.
//
// A vertex shader on the console is two pointers -- a declaration and a program -- and a handle that stands
// for both. The handle convention is the console's, not ours, and it is load-bearing: a handle with bit 0
// clear is a *fixed-function vertex format* rather than a shader, and the game sets those by passing the FVF
// straight to SetVertexShader. Any scheme of ours that did not leave bit 0 clear to the FVF would turn every
// fixed-function draw into a shader lookup that misses.
//
// What is stored here is a copy. The game owns both buffers and is free to reuse them -- a loader that kept
// the pointers would translate whatever the game put there next, which is a wrong shader rather than an
// error. The constants are copied for the same reason, and bounded: a shader whose declaration names more
// constants than there are bytes behind it is refused rather than read past.
//
// Nothing in this file talks to a graphics device. Translating the program is material_posix's job and
// drawing it is draw_posix's; this is only the shelf they are taken off.
#pragma once

#include "d3d/vertex_posix.h"

#include <stdint.h>

#include <vector>

namespace xd3d {

struct VertexProgram {
    uint32_t usage = 0;
    VertexDeclaration declaration;
    std::vector<uint32_t> program; // the NV2A microcode, copied out of the game's buffer
    uint32_t instructions = 0;
    uint32_t program_type = 0; // 0x2078 normal, 0x6378 read/write, 0x7778 state
    // The constants the declaration named through CONSTMEM, copied. The invoice noted their address at
    // creation and never applied them, which loses them; a vertex shader whose constants arrive as part of
    // its declaration is the ordinary case, not an edge one.
    std::vector<float> constants;
    bool live = false;
};

// Create one and get the handle to hand the game. Refuses with a reason rather than returning a handle that
// would fault later: a caller holding a handle it could pass to SetVertexShader must be able to trust it.
uint32_t vertex_program_create(const uint32_t *declaration, const uint32_t *function, uint32_t usage,
                               const char **err);

// The program behind a handle, or null. Null covers three cases that the caller has to tell apart, and
// `is_fvf` is how: a handle with bit 0 clear is a fixed-function format and is not ours at all, while a set
// bit with a bad index is a handle the game made up.
VertexProgram *vertex_program(uint32_t handle);
inline bool vertex_handle_is_fvf(uint32_t handle) { return (handle & 1u) == 0; }

void vertex_program_destroy(uint32_t handle);
size_t vertex_program_count();

// How many declaration tokens may be walked. A declaration the console would have ended has one; a corrupt
// one has to stop somewhere, and this is where. The bound is on tokens, not on memory: the tokens live in
// the game's image and nothing here can check they are inside it, which is the same exposure the invoice
// had and the reason the walk counts.
constexpr uint32_t VERTEX_DECLARATION_TOKEN_LIMIT = 1024;

} // namespace xd3d