// The varyings between the vertex and fragment halves of one material.
//
// This exists because two translators are written by hand and they have to agree on where each varying
// lives. When only the vertex half existed, it wrote no texture coordinates at all -- it multiplied them
// by the console's texel scale and then dropped them -- and nothing noticed, because nothing was reading
// them yet. The fragment half reads them, so the contract is written down here rather than left to both
// sides getting it right.
//
// The numbers are not free choices: Vulkan links a fragment shader's inputs against the vertex shader's
// outputs by location, and a mismatch is a pipeline the driver refuses with a message that does not say
// which side was wrong. So there is one list, and both translators take their locations from it.
//
// A varying either side does not use is simply not declared there. Declaring one and leaving it unwritten is
// legal but wasteful, and on some drivers it is worse than wasteful: an unwritten input reads undefined.
#pragma once

#include <stdint.h>

namespace xd3d {

// Locations, in the order the console's own pipeline laid its varyings out. Gaps are deliberate: they are
// the console's register numbers, and keeping the gaps means a disassembly of a frame and a line of GLSL
// can be read against each other.
enum Varying : uint32_t {
    VARYING_POSITION = 0, // gl_Position, which is built in and is never declared as a varying
    VARYING_D0 = 1,
    VARYING_D1 = 2,
    VARYING_T0 = 3,
    VARYING_T1 = 4,
    VARYING_T2 = 5,
    VARYING_T3 = 6,
    VARYING_FOG = 7,
    VARYING_PTS = 8,
    VARYING_COUNT = 9,
};

// The texture coordinate for stage `i`, or 0 for an index past the fourth. Written as a function rather than
// used as a plain enum so that a caller's `i` cannot be assumed to be in range.
inline uint32_t varying_texture(uint32_t stage) { return VARYING_T0 + (stage < 4 ? stage : 0); }

} // namespace xd3d