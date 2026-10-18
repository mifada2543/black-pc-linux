#include "d3d/vshader_store_posix.h"

#include "platform/posix/log.h"

#include <vector>

namespace xd3d {
namespace {

// The shelf. A vector rather than a map because the handle is an index, and a vector cannot hand out an
// index it does not have -- so a stale handle fails by being out of range rather than by pointing at
// whatever has since taken its place.
std::vector<VertexProgram> g_programs;

} // namespace

uint32_t vertex_program_create(const uint32_t *declaration, const uint32_t *function, uint32_t usage,
                               const char **err)
{
    if (err) *err = nullptr;
    if (!declaration || !function) {
        if (err) *err = "a vertex shader needs both a declaration and a program";
        return 0;
    }

    VertexProgram vp;
    vp.usage = usage;
    if (!parse_vertex_declaration(declaration, VERTEX_DECLARATION_TOKEN_LIMIT, &vp.declaration, err)) return 0;

    uint32_t type = 0, instructions = 0;
    const char *why = nullptr;
    if (!read_program_header(function, &type, &instructions, &why)) {
        if (err) *err = why ? why : "the program header could not be read";
        return 0;
    }
    vp.program_type = type;
    vp.instructions = instructions;
    // Four dwords an instruction, one dword past them for the header itself.
    vp.program.assign(function, function + (size_t)instructions * 4 + 1);

    // The constants the declaration named. Copied, and bounded by what the declaration claimed, so a count
    // larger than the memory behind it is a refusal rather than a read past the end of the game's buffer.
    if (vp.declaration.constant_address && vp.declaration.constant_count) {
        const uint32_t floats = vp.declaration.constant_count * 4;
        const float *src = (const float *)(uintptr_t)vp.declaration.constant_address;
        vp.constants.assign(src, src + floats);
    }

    // A slot marked dead is reused; a live one is a leak the game asked for. The handle is the index shifted
    // up one with bit 0 set, which is what leaves an FVF -- bit 0 clear -- to mean what the console means.
    uint32_t slot = 0;
    for (; slot < g_programs.size(); slot++)
        if (!g_programs[slot].live) break;
    if (slot == g_programs.size()) g_programs.push_back(VertexProgram());

    vp.live = true;
    g_programs[slot] = vp;
    return (slot << 1) | 1u;
}

VertexProgram *vertex_program(uint32_t handle)
{
    if (vertex_handle_is_fvf(handle)) return nullptr; // a fixed-function format, not ours
    const uint32_t slot = handle >> 1;
    if (slot >= g_programs.size()) return nullptr;    // a handle the game made up
    return g_programs[slot].live ? &g_programs[slot] : nullptr;
}

void vertex_program_destroy(uint32_t handle)
{
    VertexProgram *vp = vertex_program(handle);
    if (vp) vp->live = false;
}

size_t vertex_program_count()
{
    size_t n = 0;
    for (size_t i = 0; i < g_programs.size(); i++)
        if (g_programs[i].live) n++;
    return n;
}

} // namespace xd3d