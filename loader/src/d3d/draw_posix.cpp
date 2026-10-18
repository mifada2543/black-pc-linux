#include "d3d/draw_posix.h"

#include "d3d/hle_table_posix.h"
#include "d3d/resource_posix.h"
#include "d3d/vertex_posix.h"
#include "d3d/vshader_store_posix.h"

#include "platform/posix/log.h"

#include <string.h>

#include <vector>

namespace xd3d {
namespace {

#define DTRACE(...) logf("[d3d] " __VA_ARGS__)

uint64_t g_draws = 0, g_draws_skipped = 0;

// The one-entry index cache. See the header for why one and not a map.
struct IndexCache {
    uint32_t xprim = 0;
    uint32_t vertex_count = 0;
    uint32_t first_vertex = 0;
    bool live = false;
    std::vector<uint16_t> indices;
};
IndexCache g_cache;

// The counters are reachable from here on; the index cache below is not, because a caller has no business
// reaching past cached_indices() into it.
} // namespace

uint64_t draw_count() { return g_draws; }
uint64_t draw_skipped_count() { return g_draws_skipped; }
void draw_count_reset()
{
    g_draws = 0;
    g_draws_skipped = 0;
}

// ---- shape ----
//
// The mapping, in one table rather than a switch, because the interesting content is not the Vulkan name --
// it is the vertex count, and a table puts the numbers side by side where a reader can check them against
// the console's definitions.
struct Shape {
    gfx::Topology topology;
    uint32_t divisor;    // a primitive of this type is `divisor` vertices
    uint32_t primitives; // how many primitives that vertex count makes, minus 1; 0xFFFFFFFF for "one per vertex"
    bool indices;        // Vulkan has no topology for this, so one has to be written
    uint32_t expands;    // indices per primitive, when `indices` is set
};

// LINELOOP is the only one whose count is not obvious from the type name: n vertices in a closed loop are n
// lines, not n-1, because the last joins back to the first. The console drew the closing segment and a
// conversion that drops it leaves every polyline visibly open at one end.
// In the console's own order, which is what the index is: XPT_POINTLIST is 1, so row i is type i + 1.
// The order is not cosmetic. A table that put LINESTRIP where LINELOOP belongs would leave one of them
// mapped to the other's topology, and the game would draw a closed polygon as an open one.
static const Shape kShapes[] = {
    {gfx::TopologyPoints, 1, 0xFFFFFFFFu, false, 0},    // POINTLIST
    {gfx::TopologyLines, 2, 0, false, 0},                 // LINELIST
    {gfx::TopologyLineStrip, 1, 0, true, 2},              // LINELOOP: written out, because it closes
    {gfx::TopologyLineStrip, 1, 0, false, 0},             // LINESTRIP
    {gfx::TopologyTriangles, 3, 0, false, 0},             // TRIANGLELIST
    {gfx::TopologyTriangleStrip, 1, 0, false, 0},         // TRIANGLESTRIP
    {gfx::TopologyTriangles, 3, 0, true, 6},              // TRIANGLEFAN, written out as a list
    {gfx::TopologyTriangles, 4, 0, true, 6},              // QUADLIST: two triangles, six indices
    {gfx::TopologyTriangles, 4, 0, true, 6},              // QUADSTRIP: the same, with the winding alternating
    {gfx::TopologyTriangles, 3, 0, true, 6},              // POLYGON: a fan
};

static const Shape *shape_for(uint32_t xprim)
{
    if (xprim < XPT_POINTLIST || xprim > XPT_POLYGON) return nullptr;
    return &kShapes[xprim - XPT_POINTLIST];
}

bool plan_draw(uint32_t xprim, uint32_t vertex_count, uint32_t first_vertex, DrawPlan *out)
{
    if (!out) return false;
    *out = DrawPlan();
    const Shape *s = shape_for(xprim);
    if (!s || vertex_count == 0) return false;

    out->topology = s->topology;
    out->first_vertex = first_vertex;

    if (!s->indices) {
        // A LINESTRIP of one vertex draws nothing, and so does a TRIANGLESTRIP of two. Passing the count
        // through anyway would have the driver decide, and Vulkan's answer to a degenerate draw is
        // undefined rather than empty.
        if (xprim == XPT_LINELIST && vertex_count < 2) return false;
        if ((xprim == XPT_LINESTRIP || xprim == XPT_LINELOOP) && vertex_count < 2) return false;
        if (xprim == XPT_TRIANGLELIST && vertex_count < 3) return false;
        if ((xprim == XPT_TRIANGLESTRIP || xprim == XPT_QUADSTRIP || xprim == XPT_TRIANGLEFAN ||
             xprim == XPT_POLYGON) &&
            vertex_count < 3)
            return false;
        if (xprim == XPT_QUADLIST && vertex_count < 4) return false;
        out->vertex_count = vertex_count;
        out->valid = true;
        return true;
    }

    // The types that need indices written. `vertex_count` is what the game passed: for an indexed draw that
    // is the number of *indices*, and for DrawVertices it is the number of vertices, which is the same
    // arithmetic because the expansion consumes one index per vertex.
    uint32_t quads = 0;
    switch (xprim) {
    case XPT_TRIANGLEFAN:
    case XPT_POLYGON: out->index_count = vertex_count > 2 ? (vertex_count - 2) * 3 : 0; break;
    case XPT_QUADLIST:
    case XPT_QUADSTRIP: quads = vertex_count / 4; out->index_count = quads * 6; break;
    // n vertices around and the first one again: n segments, not n-1. A strip of the same n vertices
    // would be n-1 lines, which is the difference between a closed polygon and an open one.
    case XPT_LINELOOP: out->index_count = vertex_count > 2 ? vertex_count + 1 : 0; break;
    default: break;
    }
    if (!out->index_count) return false;

    // 16-bit indices cannot address past 65535, and a fan written out as a list has indices up to
    // first_vertex + vertex_count - 1. A shape that does not fit is refused here rather than wrapped, which
    // would draw a fan with its far end folded back over its near end.
    if ((uint64_t)first_vertex + vertex_count > 0x10000u) return false;

    out->vertex_count = vertex_count;
    out->needs_indices = true;
    out->valid = true;
    return true;
}

bool expand_indices(uint32_t xprim, uint32_t vertex_count, uint32_t first_vertex, std::vector<uint16_t> *indices,
                    uint32_t *index_count, const char **err)
{
    if (err) *err = nullptr;
    if (!indices || !index_count) {
        if (err) *err = "no room to put the indices";
        return false;
    }
    indices->clear();
    *index_count = 0;

    DrawPlan plan;
    if (!plan_draw(xprim, vertex_count, first_vertex, &plan) || !plan.needs_indices) {
        if (err) *err = "that primitive type needs no indices, or has too few vertices to expand";
        return false;
    }

    indices->reserve(plan.index_count);
    switch (xprim) {
    case XPT_TRIANGLEFAN:
    case XPT_POLYGON:
        // Vertex 0, then each pair of the rest. A fan of 5 is 0,1,2 and 0,2,3 and 0,3,4.
        for (uint32_t i = 1; i + 1 < vertex_count; i++) {
            indices->push_back((uint16_t)first_vertex);
            indices->push_back((uint16_t)(first_vertex + i));
            indices->push_back((uint16_t)(first_vertex + i + 1));
        }
        break;
    case XPT_QUADLIST:
        for (uint32_t q = 0; q < vertex_count / 4; q++) {
            const uint32_t v = first_vertex + q * 4;
            indices->push_back((uint16_t)(v + 0));
            indices->push_back((uint16_t)(v + 1));
            indices->push_back((uint16_t)(v + 2));
            indices->push_back((uint16_t)(v + 2));
            indices->push_back((uint16_t)(v + 3));
            indices->push_back((uint16_t)(v + 0));
        }
        break;
    case XPT_QUADSTRIP:
        // One pair of triangles per quad, and the winding alternates because the quads alternate orientation.
        // Getting that wrong turns the whole strip inside out, which is visible on the first quad and not
        // on any of the rest.
        for (uint32_t q = 0; q + 4 <= vertex_count; q += 4) {
            const uint32_t v = first_vertex + q;
            indices->push_back((uint16_t)(v + 0));
            indices->push_back((uint16_t)(v + 1));
            indices->push_back((uint16_t)(v + 2));
            indices->push_back((uint16_t)(v + 2));
            if ((q / 4) % 2 == 0) {
                indices->push_back((uint16_t)(v + 3));
                indices->push_back((uint16_t)(v + 0));
            } else {
                indices->push_back((uint16_t)(v + 0));
                indices->push_back((uint16_t)(v + 3));
            }
        }
        break;
    case XPT_LINELOOP:
        // The loop closes: the last vertex joins back to the first, and a strip of the vertices followed by
        // the first again draws exactly that.
        for (uint32_t i = 0; i < vertex_count; i++)
            indices->push_back((uint16_t)(first_vertex + i));
        indices->push_back((uint16_t)first_vertex);
        break;
    default: return false;
    }

    if (indices->size() != plan.index_count) {
        // A mismatch here is a bug in the expansion, not in the game, and it would be read as a short
        // index buffer rather than as a wrong one.
        if (err) *err = "the expansion produced a different number of indices than the plan promised";
        indices->clear();
        return false;
    }
    *index_count = (uint32_t)indices->size();
    return true;
}

bool cached_indices(uint32_t xprim, uint32_t vertex_count, uint32_t first_vertex, std::vector<uint16_t> *out,
                    uint32_t *index_count, const char **err)
{
    if (out && g_cache.live && g_cache.xprim == xprim && g_cache.vertex_count == vertex_count &&
        g_cache.first_vertex == first_vertex) {
        *out = g_cache.indices;
        if (index_count) *index_count = (uint32_t)g_cache.indices.size();
        return true;
    }
    std::vector<uint16_t> fresh;
    uint32_t n = 0;
    if (!expand_indices(xprim, vertex_count, first_vertex, &fresh, &n, err)) return false;
    g_cache.xprim = xprim;
    g_cache.vertex_count = vertex_count;
    g_cache.first_vertex = first_vertex;
    g_cache.indices = fresh;
    g_cache.live = true;
    if (out) *out = fresh;
    if (index_count) *index_count = n;
    return true;
}

namespace {

// ---- submission ----
//
// What a draw actually has to have in order to reach the GPU. Kept as one small struct so a draw that
// cannot proceed says which thing was missing, which is the difference between a bug report and a shrug.
struct DrawInputs {
    const uint8_t *vertices = nullptr;
    uint32_t stride = 0;
    uint32_t vertex_count = 0;
    uint32_t first_vertex = 0;
    const uint16_t *game_indices = nullptr;
    bool ok = false;
    const char *why = nullptr;
};

DrawInputs gather_inputs(uint32_t vertex_count, uint32_t first_vertex)
{
    DrawInputs in;
    const DeviceState::Stream &s = state().streams[0];
    if (!s.buffer) {
        in.why = "no vertex buffer is bound to stream 0";
        return in;
    }
    const ResourceRecord *rec = resource_record(s.buffer);
    if (!rec) {
        in.why = "the bound vertex buffer is not one of ours";
        return in;
    }
    if (!s.stride) {
        in.why = "stream 0 has no stride";
        return in;
    }
    // A draw past the end of the buffer is the game's arithmetic being wrong, and the console would have
    // read past it too -- into memory that happened to be there. Here the memory is ours and might not be,
    // so it is refused by name rather than turned into a read.
    const uint64_t last = (uint64_t)(first_vertex + vertex_count) * s.stride;
    if (last > rec->bytes) {
        in.why = "the draw runs past the end of the vertex buffer";
        return in;
    }
    in.vertices = (const uint8_t *)resource_data(s.buffer) + (size_t)first_vertex * s.stride;
    in.stride = s.stride;
    in.vertex_count = vertex_count;
    in.first_vertex = first_vertex;
    in.ok = true;
    return in;
}

// Upload the managed copy of a vertex buffer and bind it. Returns false when the copy failed, which the
// draw path reports rather than drawing with whatever was there before.
bool bind_stream_zero()
{
    const ResourceRecord *rec = resource_record(state().streams[0].buffer);
    if (!rec || !rec->managed) return false;
    const char *why = nullptr;
    if (!gfx::managed_buffer_flush(rec->managed, &why)) {
        logf("[d3d] a vertex buffer could not be uploaded: %s", why ? why : "?");
        return false;
    }
    return gfx::managed_buffer_bind(rec->managed, 0, 0);
}

void submit(const char *what, uint32_t xprim, DrawPlan &plan, const DrawInputs &in, const uint16_t *game_indices)
{
    const char *why = nullptr;
    if (!plan.valid) {
        g_draws_skipped++;
        return;
    }
    if (!in.ok) {
        g_draws_skipped++;
        DTRACE("%s %u: %s", what, xprim, in.why);
        return;
    }
    if (!bind_stream_zero()) {
        g_draws_skipped++;
        return;
    }

    std::vector<uint16_t> expanded;
    const uint16_t *indices = game_indices;
    uint32_t index_count = 0;
    int32_t vertex_offset = 0;
    if (plan.needs_indices) {
        if (game_indices) {
            // The game's own indices. vertex_offset is the StartVertex of DrawVertices, applied the way
            // Vulkan wants it: as an offset added to every index rather than as a base vertex, because the
            // game already counted from its own zero.
            index_count = plan.index_count;
            vertex_offset = (int32_t)in.first_vertex;
        } else if (!cached_indices(xprim, in.vertex_count, 0, &expanded, &index_count, &why)) {
            g_draws_skipped++;
            DTRACE("%s %u: %s", what, xprim, why ? why : "the indices could not be written");
            return;
        } else {
            indices = expanded.data();
            vertex_offset = 0;
        }
        if (!gfx::bind_index_buffer_upload(indices, index_count * sizeof(uint16_t))) {
            g_draws_skipped++;
            DTRACE("%s %u: the index buffer could not be bound", what, xprim);
            return;
        }
    }

    const bool ok = indices ? gfx::draw_indexed(plan.topology, index_count, 1, 0, vertex_offset)
                            : gfx::draw(plan.topology, plan.vertex_count, 1, 0);
    if (ok) {
        g_draws++;
        DTRACE("%s %u, %u vertices -> %s", what, xprim, plan.vertex_count,
               plan.needs_indices ? "indexed" : "direct");
    } else {
        g_draws_skipped++;
        DTRACE("%s %u: the driver refused the draw: %s", what, xprim, gfx::last_error());
    }
}

} // namespace

// ---- the rows ----

void XD3D_STDCALL D3DDevice_DrawVerticesUP(uint32_t PrimitiveType, uint32_t VertexCount,
                                           const void *pVertexStreamZeroData, uint32_t VertexStreamZeroStride)
{
    DTRACE("DrawVerticesUP %u, %u, %p, %u", PrimitiveType, VertexCount, pVertexStreamZeroData,
           VertexStreamZeroStride);
    pb_flush();

    // The vertices are in memory the game chose, not in a buffer of ours, so there is nothing to upload and
    // nothing to bind: the game's own bytes are already where the driver will read them. A managed buffer is
    // the wrong tool here and using one would copy the data for no reason.
    DrawPlan plan;
    if (!plan_draw(PrimitiveType, VertexCount, 0, &plan)) {
        g_draws_skipped++;
        return;
    }
    if (!pVertexStreamZeroData || !VertexStreamZeroStride) {
        g_draws_skipped++;
        DTRACE("DrawVerticesUP %u: no data", PrimitiveType);
        return;
    }

    gfx::Buffer up;
    if (!gfx::create_buffer(&up, (uint32_t)VertexStreamZeroStride, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true)) {
        g_draws_skipped++;
        DTRACE("DrawVerticesUP %u: %s", PrimitiveType, gfx::last_error());
        return;
    }
    if (!gfx::update_buffer(&up, 0, (uint32_t)VertexStreamZeroStride, pVertexStreamZeroData)) {
        gfx::destroy_buffer(&up);
        g_draws_skipped++;
        return;
    }

    DrawInputs in;
    in.vertices = (const uint8_t *)up.mapped;
    in.stride = VertexStreamZeroStride;
    in.vertex_count = VertexCount;
    in.ok = true;

    gfx::bind_vertex_buffer(&up, 0, 0);
    std::vector<uint16_t> expanded;
    if (plan.needs_indices) {
        uint32_t n = 0;
        const char *why = nullptr;
        if (!cached_indices(PrimitiveType, VertexCount, 0, &expanded, &n, &why)) {
            gfx::destroy_buffer(&up);
            g_draws_skipped++;
            DTRACE("DrawVerticesUP %u: %s", PrimitiveType, why ? why : "the indices could not be written");
            return;
        }
        if (!gfx::bind_index_buffer_upload(expanded.data(), n * sizeof(uint16_t))) {
            gfx::destroy_buffer(&up);
            g_draws_skipped++;
            return;
        }
        const bool ok = gfx::draw_indexed(plan.topology, n, 1, 0, 0);
        if (ok) g_draws++; else g_draws_skipped++;
    } else {
        const bool ok = gfx::draw(plan.topology, plan.vertex_count, 1, 0);
        if (ok) g_draws++; else g_draws_skipped++;
    }
    // The buffer that held the game's vertices is not ours to keep, but it cannot be freed either: the
    // command buffer naming it has not run. It is released at the next frame boundary by the same list that
    // holds staging, and until then it costs one allocation per draw -- which is why this path is a fallback
    // and not the one the game mostly takes.
    gfx::destroy_buffer(&up);
}

void XD3D_STDCALL D3DDevice_DrawVertices(uint32_t PrimitiveType, uint32_t StartVertex, uint32_t VertexCount)
{
    DTRACE("DrawVertices %u, %u, %u", PrimitiveType, StartVertex, VertexCount);
    pb_flush();
    DrawPlan plan;
    if (!plan_draw(PrimitiveType, VertexCount, 0, &plan)) {
        g_draws_skipped++;
        return;
    }
    DrawInputs in = gather_inputs(VertexCount, StartVertex);
    submit("DrawVertices", PrimitiveType, plan, in, nullptr);
}

void XD3D_STDCALL D3DDevice_DrawIndexedVertices(uint32_t PrimitiveType, uint32_t VertexCount,
                                                const uint16_t *pIndexData)
{
    DTRACE("DrawIndexedVertices %u, %u, %p", PrimitiveType, VertexCount, pIndexData);
    pb_flush();
    if (!pIndexData || !VertexCount) {
        g_draws_skipped++;
        return;
    }

    // How far the indices reach decides how much of the buffer has to be uploaded: everything from the
    // lowest index used to the highest, and not one vertex more. A vertex buffer holding a whole scene must
    // not be copied in full because a small mesh indexed its last hundred vertices.
    uint32_t lowest = 0xFFFFu, highest = 0;
    for (uint32_t i = 0; i < VertexCount; i++) {
        if (pIndexData[i] < lowest) lowest = pIndexData[i];
        if (pIndexData[i] > highest) highest = pIndexData[i];
    }
    const uint32_t needed = highest - lowest + 1;

    DrawPlan plan;
    if (!plan_draw(PrimitiveType, VertexCount, 0, &plan)) {
        g_draws_skipped++;
        return;
    }

    DrawInputs in = gather_inputs(needed, lowest);
    submit("DrawIndexedVertices", PrimitiveType, plan, in, pIndexData);
}

// ---- the table ----

#define H(addr, fn, params, ret)                                                                                      \
    {                                                                                                                  \
        addr, #fn, (void *)&fn, params, ret                                                                           \
    }

static const Hle kTable[] = {
    H(0x00269c90, D3DDevice_DrawVerticesUP, "s s s s", false),
    H(0x00269e40, D3DDevice_DrawVertices, "s s s", false),
    H(0x00269f10, D3DDevice_DrawIndexedVertices, "s s s", false),
};

#undef H

const Hle *draw_rows(size_t *count)
{
    *count = sizeof kTable / sizeof kTable[0];
    return kTable;
}

} // namespace xd3d