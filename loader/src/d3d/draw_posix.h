// The three draw calls, and the question each of them starts by asking: what shape is this geometry?
//
// The Xbox named ten primitive types. Vulkan has five topologies, so the naming does not survive the trip
// intact, and the part worth being careful about is not the mapping -- it is the vertex count.
//
// `vkCmdDraw` takes vertices. The console took primitives. A LINELIST of 8 vertices is 4 lines, a
// TRIANGLEFAN of 5 is 3 triangles, and a LINELOOP of 8 is 8 lines, because a loop closes back on its own
// first vertex. So every type has to answer two questions -- which topology, and how many vertices -- and
// getting the second one wrong by a factor is how a scene draws as garbage rather than as nothing.
//
// Three of the console's types have no Vulkan topology at all:
//
//   * QUADLIST and QUADSTRIP. Vulkan has no quads. Both become triangle lists, which means writing indices
//     the console never had.
//   * TRIANGLEFAN and POLYGON, which is a fan of up to 64 sides. Vulkan 1.1 has no fan either.
//
// Those need an index buffer, and that is the part of this file that is not arithmetic. `plan_draw` says
// whether indices are needed and how to get them; the draw path asks for them once and reuses them.
//
// No Vulkan type appears here. The topology is `gfx::Topology`, a plain enum that the gfx layer maps to a
// VkPrimitiveTopology -- the rule in the plan, section 3.2, and the one place in the draw path where it
// costs something to keep.
#pragma once

#include "d3d/device_posix.h" // XD3D_STDCALL

#include "d3d/xd3d8_posix.h"

#include "gfx/gfx_vulkan.h"

#include <stdint.h>

#include <vector>

namespace xd3d {

// What a draw call turned into. Everything the submission needs and nothing it has to work out again.
struct DrawPlan {
    gfx::Topology topology = gfx::TopologyTriangles;
    uint32_t vertex_count = 0;  // what vkCmdDraw wants, not what the game called
    uint32_t first_vertex = 0;  // the StartVertex of DrawVertices, in vertices
    uint32_t index_count = 0;   // non-zero only when `needs_indices` is set
    bool needs_indices = false; // the type has no Vulkan topology and one has to be written
    bool valid = false;         // false when there is nothing to draw at all
};

// Turn (console primitive type, vertex count, first vertex) into what the driver wants. Pure: no state, no
// device, no side effects. Everything decided here is decided for the same reason every time, which is what
// makes a wrong answer findable.
//
// A vertex count too small for the type is not an error -- it is a draw of nothing, and the console drew
// nothing too. It comes back as `valid == false` rather than as a failure, because the difference matters
// to the caller: a refusal is something to report, an empty draw is not.
bool plan_draw(uint32_t xprim, uint32_t vertex_count, uint32_t first_vertex, DrawPlan *out);

// Write the indices a type that has no Vulkan topology needs. `indices` is filled and `index_count` set;
// returns false when the geometry cannot be expressed (a fan too large for 16-bit indices, say).
//
// The indices are relative to the first vertex of the draw, so `first_vertex` has to be added in: a QUADLIST
// whose vertices start at 300 produces indices above 300, not above 0.
bool expand_indices(uint32_t xprim, uint32_t vertex_count, uint32_t first_vertex, std::vector<uint16_t> *indices,
                    uint32_t *index_count, const char **err);

// The index cache the draw path keeps. One buffer per (type, count, first) is not worth it: the game draws
// the same shape over and over, so the last expansion is kept and reused until it does not match. The cache
// is deliberately one entry -- a map would be faster and would also keep every shape the game has ever
// drawn alive for the rest of the run.
bool cached_indices(uint32_t xprim, uint32_t vertex_count, uint32_t first_vertex, std::vector<uint16_t> *out,
                    uint32_t *index_count, const char **err);

// How many draws have been submitted, and how many were skipped because there was nothing to submit. Both
// are reported by --probe; a run where everything is skipped is a run where the game drew nothing, and it
// looks the same from the outside as a run that worked.
uint64_t draw_count();
uint64_t draw_skipped_count();
void draw_count_reset();

// ---- the rows ----

XD3D_STDCALL void D3DDevice_DrawVerticesUP(uint32_t PrimitiveType, uint32_t VertexCount,
                                           const void *pVertexStreamZeroData, uint32_t VertexStreamZeroStride);
XD3D_STDCALL void D3DDevice_DrawVertices(uint32_t PrimitiveType, uint32_t StartVertex, uint32_t VertexCount);
XD3D_STDCALL void D3DDevice_DrawIndexedVertices(uint32_t PrimitiveType, uint32_t VertexCount,
                                                const uint16_t *pIndexData);

// The row table, which is the last of the three empty ones.
const struct Hle *draw_rows(size_t *count);

} // namespace xd3d