// What a draw call turns into: a topology and a vertex count.
//
// This is arithmetic with no device behind it, which is exactly why it is worth its own tests. A factor of
// three, or a strip where a fan belongs, produces a scene that draws as coloured noise rather than as
// nothing at all -- and nothing about that failure points at the number that was wrong.
//
// The two halves under test are the plan (which topology, how many vertices) and the expansion (the indices
// a type with no Vulkan topology needs). The plan is pure; the expansion is pure too, and together they
// cover every one of the console's ten primitive types.
#include "d3d/draw_posix.h"

#include "d3d/hle_table_posix.h"
#include "d3d/vshader_store_posix.h"

#include <stdio.h>
#include <string.h>

#include <vector>

using namespace xd3d;

static int g_checks = 0, g_failures = 0;

static void check(bool ok, const char *what)
{
    printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
    g_checks++;
    if (!ok) g_failures++;
}

static void check_eq(uint64_t got, uint64_t want, const char *what)
{
    if (got != want) printf("  ....  %s: dapat %llu, harap %llu\n", what, (unsigned long long)got,
                            (unsigned long long)want);
    check(got == want, what);
}

static const char *topology_name(gfx::Topology t)
{
    switch (t) {
    case gfx::TopologyPoints: return "points";
    case gfx::TopologyLines: return "lines";
    case gfx::TopologyLineStrip: return "line strip";
    case gfx::TopologyTriangles: return "triangles";
    case gfx::TopologyTriangleStrip: return "triangle strip";
    }
    return "?";
}

// ---- the plan ----

static void test_the_types_that_map_directly()
{
    printf("\nthe types Vulkan has a topology for\n");

    DrawPlan p;
    check(plan_draw(XPT_POINTLIST, 8, 0, &p), "a point list plans");
    check_eq(p.topology, gfx::TopologyPoints, "as points");
    check_eq(p.vertex_count, 8, "and all eight vertices reach the driver -- one point each");
    check(!p.needs_indices, "with no indices");

    check(plan_draw(XPT_LINELIST, 8, 0, &p), "a line list plans");
    check_eq(p.topology, gfx::TopologyLines, "as lines");
    check_eq(p.vertex_count, 8, "and all eight vertices -- four lines, and vkCmdDraw counts vertices");

    check(plan_draw(XPT_LINESTRIP, 8, 0, &p), "a line strip plans");
    check_eq(p.topology, gfx::TopologyLineStrip, "as a line strip");
    check_eq(p.vertex_count, 8, "and all eight vertices -- a strip of n is n-1 lines but n vertices");

    check(plan_draw(XPT_TRIANGLELIST, 9, 0, &p), "a triangle list plans");
    check_eq(p.topology, gfx::TopologyTriangles, "as triangles");
    check_eq(p.vertex_count, 9, "and all nine vertices");

    check(plan_draw(XPT_TRIANGLESTRIP, 9, 0, &p), "a triangle strip plans");
    check_eq(p.topology, gfx::TopologyTriangleStrip, "as a triangle strip");
    check_eq(p.vertex_count, 9, "and all nine vertices");

    // StartVertex is the game's, and it must survive: the vertices come from a buffer the game filled and
    // the draw says where in it to start.
    check(plan_draw(XPT_TRIANGLELIST, 3, 120, &p), "a triangle list with a start vertex plans");
    check_eq(p.first_vertex, 120, "and the start vertex is carried through");
}

static void test_the_types_that_need_indices()
{
    printf("\nthe types Vulkan has no topology for\n");

    DrawPlan p;
    check(plan_draw(XPT_QUADLIST, 8, 0, &p), "a quad list plans");
    check_eq(p.topology, gfx::TopologyTriangles, "as triangles, because Vulkan has no quads");
    check(p.needs_indices, "with indices written");
    check_eq(p.index_count, 12, "two quads is twelve indices -- six each");

    check(plan_draw(XPT_QUADSTRIP, 8, 0, &p), "a quad strip plans");
    check_eq(p.index_count, 12, "two quads of strip is twelve indices as well");

    check(plan_draw(XPT_TRIANGLEFAN, 5, 0, &p), "a triangle fan plans");
    check_eq(p.index_count, 9, "five fan vertices is three triangles, nine indices");
    check(plan_draw(XPT_POLYGON, 5, 0, &p), "a polygon plans");
    check_eq(p.index_count, 9, "and the same way -- it is a fan");

    check(plan_draw(XPT_LINELOOP, 8, 0, &p), "a line loop plans");
    check_eq(p.index_count, 9, "and needs nine vertices: eight around and the first again to close it");
}

static void test_counts_too_small_for_the_type()
{
    printf("\ncounts too small for the type\n");

    // Not a failure. The console drew nothing here too, and the difference matters: a refusal is something
    // to report, an empty draw is not.
    DrawPlan p;
    check(!plan_draw(XPT_TRIANGLELIST, 2, 0, &p), "two vertices are not a triangle");
    check(!plan_draw(XPT_LINELIST, 1, 0, &p), "one vertex is not a line");
    check(!plan_draw(XPT_TRIANGLESTRIP, 2, 0, &p), "two vertices are not a triangle strip");
    check(!plan_draw(XPT_QUADLIST, 3, 0, &p), "three vertices are not a quad");
    check(!plan_draw(XPT_LINESTRIP, 1, 0, &p), "one vertex is not a line strip");
    check(!plan_draw(XPT_TRIANGLELIST, 0, 0, &p), "and no vertices is not anything");

    // A degenerate triangle list is still one triangle. Refusing it would leave a hole in the scene where
    // the console drew a dot.
    check(plan_draw(XPT_TRIANGLELIST, 3, 0, &p), "three vertices are exactly one triangle");
    check_eq(p.vertex_count, 3, "and all three reach the driver");
}

static void test_an_unknown_type_is_refused()
{
    printf("\nan unknown primitive type\n");

    DrawPlan p;
    check(!plan_draw(0, 3, 0, &p), "zero is not a primitive type");
    check(!plan_draw(11, 3, 0, &p), "and neither is eleven");
    check(!plan_draw(0xFFFFFFFFu, 3, 0, &p), "nor anything else");
    check(!plan_draw(XPT_TRIANGLELIST, 3, 0, nullptr), "and a null plan is refused rather than filled");
}

static void test_indices_that_cannot_address_the_vertices()
{
    printf("\nindices that cannot address the vertices\n");

    // 16-bit indices reach 65535. A shape starting past that cannot be written at all, and writing it anyway
    // would fold the far end back over the near one instead of failing.
    DrawPlan p;
    check(!plan_draw(XPT_TRIANGLEFAN, 5, 65534, &p), "a fan starting at 65534 is refused");
    check(plan_draw(XPT_TRIANGLEFAN, 4, 65532, &p), "one starting at 65532 is not: four vertices reach 65535");
    // A type that needs no indices is not constrained by this at all.
    check(plan_draw(XPT_TRIANGLELIST, 3, 1000000, &p), "and a plain triangle list has no such limit");
}

// ---- the expansion ----

static bool indices_are(const std::vector<uint16_t> &got, const uint16_t *want, size_t n)
{
    if (got.size() != n) return false;
    for (size_t i = 0; i < n; i++)
        if (got[i] != want[i]) return false;
    return true;
}

static void test_expanding_a_fan()
{
    printf("\nexpanding a triangle fan\n");

    std::vector<uint16_t> idx;
    uint32_t n = 0;
    const char *why = nullptr;
    check(expand_indices(XPT_TRIANGLEFAN, 5, 0, &idx, &n, &why), "a fan of five expands");
    const uint16_t want[] = {0, 1, 2, 0, 2, 3, 0, 3, 4};
    if (!indices_are(idx, want, 9)) {
        for (size_t i = 0; i < idx.size(); i++) printf("%u ", idx[i]);
        printf("\n");
    }
    check(indices_are(idx, want, 9), "into three triangles that all share vertex 0");
    check_eq(n, 9, "nine indices");

    // Starting part-way along a buffer, the indices have to carry the offset -- a game that draws a fan
    // from its third hundred vertices means those vertices, and indices from zero would read the wrong ones.
    check(expand_indices(XPT_TRIANGLEFAN, 4, 300, &idx, &n, &why), "a fan at an offset expands");
    const uint16_t offset_want[] = {300, 301, 302, 300, 302, 303};
    check(indices_are(idx, offset_want, 6), "with the offset added to every index");

    check(expand_indices(XPT_POLYGON, 4, 0, &idx, &n, &why), "a polygon expands the same way");
    check(indices_are(idx, want, 6), "as a fan of the vertices it has");
}

static void test_expanding_quads()
{
    printf("\nexpanding quads\n");

    std::vector<uint16_t> idx;
    uint32_t n = 0;
    const char *why = nullptr;
    check(expand_indices(XPT_QUADLIST, 8, 0, &idx, &n, &why), "two quads expand");
    // Each quad is 0,1,2 and 2,3,0 -- the diagonal is 2-0, which is what makes the two triangles share an
    // edge instead of overlapping. Getting the winding wrong draws a bow tie, which is visible on the first
    // quad and not on any of the rest.
    const uint16_t want[] = {0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4};
    if (!indices_are(idx, want, 12)) {
        for (size_t i = 0; i < idx.size(); i++) printf("%u ", idx[i]);
        printf("\n");
    }
    check(indices_are(idx, want, 12), "into two triangles per quad, sharing the 2-0 edge");
    check_eq(n, 12, "twelve indices");

    check(expand_indices(XPT_QUADLIST, 5, 0, &idx, &n, &why), "a partial quad list expands");
    check_eq(n, 6, "and the odd vertex left over is dropped, as the console dropped it");
}

static void test_expanding_a_quad_strip()
{
    printf("\nexpanding a quad strip\n");

    std::vector<uint16_t> idx;
    uint32_t n = 0;
    const char *why = nullptr;
    check(expand_indices(XPT_QUADSTRIP, 8, 0, &idx, &n, &why), "two quads of strip expand");
    const uint16_t want[] = {
        0, 1, 2, 2, 3, 0, // the first quad
        4, 5, 6, 6, 4, 7, // and the second, wound the other way round
    };
    if (!indices_are(idx, want, 12)) {
        for (size_t i = 0; i < idx.size(); i++) printf("%u ", idx[i]);
        printf("\n");
    }
    check(indices_are(idx, want, 12), "with the winding alternating, which a strip requires");

    // Three quads, with the winding alternating. If it did not, every other quad would be inside out, and
    // the strip would look right for half its length -- which is the worst way for it to be wrong.
    check(expand_indices(XPT_QUADSTRIP, 12, 0, &idx, &n, &why), "three quads of strip expand");
    check_eq(n, 18, "into eighteen indices");
    const uint16_t three[] = {
        0, 1, 2, 2, 3, 0, //  first quad, winding one way
        4, 5, 6, 6, 4, 7, //  second, the other way
        8, 9, 10, 10, 11, 8, // and third, back to the first
    };
    if (!indices_are(idx, three, 18)) {
        for (size_t i = 0; i < idx.size(); i++) printf("%u ", idx[i]);
        printf("\n");
    }
    check(indices_are(idx, three, 18), "with the winding alternating on every other quad");
}

static void test_expanding_a_line_loop()
{
    printf("\nexpanding a line loop\n");

    std::vector<uint16_t> idx;
    uint32_t n = 0;
    const char *why = nullptr;
    check(expand_indices(XPT_LINELOOP, 4, 0, &idx, &n, &why), "a loop of four expands");
    // The loop closes: four vertices around and the first again is four lines, where a strip of the same
    // four would be three. Dropping the closing segment leaves every polyline open at one end.
    const uint16_t want[] = {0, 1, 2, 3, 0};
    check(indices_are(idx, want, 5), "with the first vertex repeated to close it");
    check_eq(n, 5, "five indices for four vertices");
}

static void test_what_will_not_expand()
{
    printf("\nwhat will not expand\n");

    std::vector<uint16_t> idx;
    uint32_t n = 0;
    const char *why = nullptr;
    check(!expand_indices(XPT_TRIANGLELIST, 3, 0, &idx, &n, &why), "a type that needs no indices refuses");
    check(why != nullptr, "with a reason");
    check(!expand_indices(XPT_TRIANGLEFAN, 2, 0, &idx, &n, &why), "and so does a fan of two");
    check(!expand_indices(XPT_TRIANGLEFAN, 5, 0, nullptr, &n, &why), "and so does nowhere to put them");
    check(!expand_indices(XPT_TRIANGLEFAN, 5, 0, &idx, nullptr, &why), "and so does nowhere to count them");

    // A refusal must leave nothing behind that a later caller could mistake for an answer.
    check(idx.empty(), "and the index list is left empty rather than half written");
}

static void test_the_index_cache()
{
    printf("\nthe index cache\n");

    std::vector<uint16_t> a, b;
    uint32_t na = 0, nb = 0;
    const char *why = nullptr;
    check(cached_indices(XPT_QUADLIST, 8, 0, &a, &na, &why), "a quad list is cached");
    check(cached_indices(XPT_QUADLIST, 8, 0, &b, &nb, &why), "and the same one comes back");
    check(a == b && na == nb, "identical");
    check(cached_indices(XPT_QUADLIST, 12, 0, &b, &nb, &why), "a different size expands afresh");
    check_eq(nb, 18, "with the right count for three quads");
    check(cached_indices(XPT_TRIANGLEFAN, 5, 0, &b, &nb, &why), "and a different type does not reuse it");
    check_eq(nb, 9, "with a fan's count instead");
}

// ---- the shader store ----

static void test_a_shader_is_kept()
{
    printf("\nshader objects\n");

    // Built by hand rather than transcribed, because the shape is what is under test and the constants are
    // the console's: token 0 is the end.
    // The token layout the parser reads: type in bits 29-31, and for an element the vertex type in bits
    // 16-23 and the register in bits 0-4. A zero token ends the declaration.
    uint32_t tokens[16];
    memset(tokens, 0, sizeof tokens);
    tokens[0] = (2u << 29) | ((uint32_t)VST_FLOAT3 << 16) | 0u; // one element: position as three floats
    tokens[1] = 0u;                                            // D3DDECL_END

    // A program header (type 0x2078, one instruction) followed by one instruction of zeroes.
    uint32_t program[5];
    memset(program, 0, sizeof program);
    program[0] = 0x2078u | (1u << 16); // program type, then the instruction count in the high half

    const char *why = nullptr;
    const uint32_t h = vertex_program_create(tokens, program, 0, &why);
    check(h != 0, "a shader is created");
    if (!h) {
        printf("       %s\n", why ? why : "");
        return;
    }
    check((h & 1u) != 0, "with bit 0 set, which is what tells an FVF apart from a shader");
    check(vertex_handle_is_fvf(XPT_TRIANGLELIST * 0 + 0x0002u), "and an FVF has bit 0 clear");

    VertexProgram *vp = vertex_program(h);
    check(vp != nullptr, "and it is found by its handle");
    if (vp) {
        check_eq(vp->declaration.count, 1, "with the one element its declaration named");
        check_eq(vp->instructions, 1, "and the instruction count from the program header");
        check_eq(vp->program_type, 0x2078, "and the program type");
        check_eq(vp->program.size(), 5, "and the microcode copied, header included");
        // The copy matters: the game owns its buffer and may reuse it, and a loader that kept the pointer
        // would translate whatever the game put there next.
        program[5 - 1] = 0xDEADBEEFu;
        check_eq(vp->program[4], 0u, "and it is a copy, not the game's buffer");
    }

    check_eq(vertex_program_count(), 1, "one shader is live");
    vertex_program_destroy(h);
    check_eq(vertex_program_count(), 0, "and destroying it takes it off the shelf");
    check(vertex_program(h) == nullptr, "so the handle no longer finds one");
    check(vertex_program(0x1234u) == nullptr, "and a handle the game made up finds none either");
    check(vertex_program(0x0002u) == nullptr, "and an FVF is not looked up at all");
}

static void test_what_a_shader_will_not_accept()
{
    printf("\nwhat a shader will not accept\n");

    // Two elements and no end token, so the walk has to run out rather than stop. Every remaining token is
    // an element, because a zero in the middle ends it.
    uint32_t tokens[VERTEX_DECLARATION_TOKEN_LIMIT + 8];
    for (uint32_t i = 0; i < VERTEX_DECLARATION_TOKEN_LIMIT + 8; i++)
        tokens[i] = (2u << 29) | ((uint32_t)VST_FLOAT3 << 16) | (uint32_t)(i & 0x1F);

    uint32_t program[8];
    memset(program, 0, sizeof program);
    program[0] = 0x2078u | (1u << 16); // program type, then the instruction count in the high half

    const char *why = nullptr;
    check(vertex_program_create(tokens, program, 0, &why) == 0, "a declaration with no end token is refused");
    check(why != nullptr, "with a reason");

    uint32_t no_header[8] = {0, 0, 0, 0, 0, 0, 0, 0}; // no instructions in the header
    check(vertex_program_create(tokens, no_header, 0, &why) == 0, "and so is a program with no header");
    check(why != nullptr, "with a reason too");

    check(vertex_program_create(nullptr, program, 0, &why) == 0, "no declaration at all is refused");
    check(vertex_program_create(tokens, nullptr, 0, &why) == 0, "and so is no program");
    check_eq(vertex_program_count(), 0, "and nothing was left on the shelf");
}

static void test_a_slot_is_reused_rather_than_leaked()
{
    printf("\nslot reuse\n");

    uint32_t tokens[16];
    memset(tokens, 0, sizeof tokens);
    tokens[0] = (2u << 29) | ((uint32_t)VST_FLOAT3 << 16) | 0u;
    uint32_t program[8];
    memset(program, 0, sizeof program);
    program[0] = 0x2078u | (1u << 16); // program type, then the instruction count in the high half

    const char *why = nullptr;
    const uint32_t a = vertex_program_create(tokens, program, 0, &why);
    const uint32_t b = vertex_program_create(tokens, program, 0, &why);
    check(a != 0 && b != 0 && a != b, "two shaders get two handles");
    check_eq(vertex_program_count(), 2, "and both are live");
    vertex_program_destroy(a);
    check_eq(vertex_program_count(), 1, "one goes");
    const uint32_t c = vertex_program_create(tokens, program, 0, &why);
    check_eq(c, a, "and its slot is reused rather than the shelf growing");
    check_eq(vertex_program_count(), 2, "still two");
    vertex_program_destroy(b);
    vertex_program_destroy(c);
    check_eq(vertex_program_count(), 0, "and both are gone");
}

// ---- the table ----

static void test_the_table_is_whole()
{
    printf("\nthe table\n");

    size_t n = 0;
    const Hle *rows = draw_rows(&n);
    check(rows != nullptr && n == 3, "all three draw rows are present");
    if (!rows) return;

    TableReport report = build_table(rows, n);
    check_eq(report.built, n, "and every one of them builds a thunk");
    for (size_t i = 0; i < report.refused.size(); i++)
        printf("  ....  %s at 0x%08x: %s\n", report.refused[i].name.c_str(), report.refused[i].address,
               report.refused[i].reason.c_str());

    size_t out_of_section = 0, without_impl = 0;
    for (size_t i = 0; i < n; i++) {
        if (rows[i].addr < 0x00260000 || rows[i].addr > 0x00280000) out_of_section++;
        if (!rows[i].impl) without_impl++;
    }
    check_eq(out_of_section, 0, "every address is in the image's D3D section");
    check_eq(without_impl, 0, "and every row names an implementation");

    // The three addresses are per-build facts, and they are the ones the invoice recorded. A row that
    // drifted here would patch a neighbouring function and nothing would say so.
    check_eq(rows[0].addr, 0x00269c90u, "DrawVerticesUP is at the address the invoice recorded");
    check_eq(rows[1].addr, 0x00269e40u, "DrawVertices is");
    check_eq(rows[2].addr, 0x00269f10u, "and DrawIndexedVertices is");

    // All three return void: a draw that reports a HRESULT on the console tells the game nothing it can act
    // on, and getting this wrong would pop the wrong number of bytes off its stack.
    check(!rows[0].returns && !rows[1].returns && !rows[2].returns, "and none of them returns a value");
}

static void test_every_console_type_is_covered()
{
    printf("\nevery console type is covered\n");

    // Ten types, and a table that forgot one would leave the game drawing with whatever the table had
    // nearest -- which is how a QUADSTRIP becomes a triangle list and nothing reports an error.
    for (uint32_t t = XPT_POINTLIST; t <= XPT_POLYGON; t++) {
        DrawPlan p;
        const uint32_t verts = t == XPT_LINELOOP || t == XPT_TRIANGLEFAN || t == XPT_POLYGON ? 8 : 12;
        char what[96];
        snprintf(what, sizeof what, "type %u plans into %s", t, topology_name(p.topology));
        const bool ok = plan_draw(t, verts, 0, &p) && p.valid;
        if (!ok) snprintf(what, sizeof what, "type %u plans", t);
        check(ok, what);
    }
}

int main()
{
    printf("BLACK loader -- primitive types, index expansion, shader objects\n");

    test_the_types_that_map_directly();
    test_the_types_that_need_indices();
    test_counts_too_small_for_the_type();
    test_an_unknown_type_is_refused();
    test_indices_that_cannot_address_the_vertices();
    test_expanding_a_fan();
    test_expanding_quads();
    test_expanding_a_quad_strip();
    test_expanding_a_line_loop();
    test_what_will_not_expand();
    test_the_index_cache();
    test_a_shader_is_kept();
    test_what_a_shader_will_not_accept();
    test_a_slot_is_reused_rather_than_leaked();
    test_the_table_is_whole();
    test_every_console_type_is_covered();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}