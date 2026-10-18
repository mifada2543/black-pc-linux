// The vertex-format table: what an Xbox D3DVSDT_* type is, how wide it is, and what it becomes.
//
// Three separate jobs meet here, and they are kept apart because they fail differently:
//
//   * `vsdt_size` is a fact about the console. It decides where the next element starts, so being wrong
//     shifts every element after the mistake -- which is why the table is transcribed rather than derived.
//   * `vertex_format_code` is a decision about Vulkan. Two of the console's types have no Vulkan format at
//     all, and a table that quietly rounded them to something close would move the bug from the shader
//     into the geometry. They say so instead.
//   * `build_vertex_layout` turns a shader's element list into bindings and attributes. It is pure
//     arithmetic over the two above.
//
// No `Vk*` type appears here, or anywhere else in d3d/: this is a plain integer code that the gfx layer
// maps to a VkFormat (see gfx_format_for_vertex_code). That boundary is the rule in the plan, section 3.2,
// and this file is where keeping it costs something real rather than being decoration.
#pragma once

#include <stdint.h>

namespace xd3d {

// What the console calls a D3DVSDT_* type. These are the console's own numbers, and they are the key to
// every table in this file: they are what a shader declaration carries in its element tokens.
enum VertexStreamType : uint32_t {
    VST_NONE = 0x02,
    VST_FLOAT1 = 0x12,
    VST_FLOAT2 = 0x22,
    VST_FLOAT3 = 0x32,
    VST_FLOAT4 = 0x42,
    VST_FLOAT2H = 0x72,
    VST_D3DCOLOR = 0x40,
    VST_SHORT1 = 0x15,
    VST_SHORT2 = 0x25,
    VST_SHORT3 = 0x35,
    VST_SHORT4 = 0x45,
    VST_NORMSHORT1 = 0x11,
    VST_NORMSHORT2 = 0x21,
    VST_NORMSHORT3 = 0x31,
    VST_NORMSHORT4 = 0x41,
    VST_NORMPACKED3 = 0x16,
    VST_PBYTE1 = 0x14,
    VST_PBYTE2 = 0x24,
    VST_PBYTE3 = 0x34,
    VST_PBYTE4 = 0x44,
};

// One element of a shader's declaration, as the console stores it.
struct VertexElement {
    uint8_t stream;
    uint8_t reg;
    uint8_t type; // a VertexStreamType
    uint16_t offset; // byte offset within the stream
};

// Bytes one element of this type occupies. Zero for VST_NONE and for anything this build does not know,
// which is how a declaration token stream is validated rather than trusted.
uint32_t vsdt_size(uint32_t type);

// What a type becomes on the Vulkan side, as a plain code. The codes are this file's own; gfx maps them
// to VkFormat with gfx_format_for_vertex_code, so that d3d/ never has to name a Vulkan type.
//
// VF_UNPACKED is the one that matters: NORMPACKED3 is three signed bytes packed into one dword, which is
// an NV2A format with no Vulkan equivalent. It cannot be expressed as a vertex format at all -- a
// translated shader has to read the dword and unpack it by hand. Reporting it as the nearest plausible
// format (an RGBA8 one) would put three components in the right place and leave the fourth holding
// whatever the buffer happened to contain, which draws wrong geometry instead of failing.
enum VertexFormatCode : uint32_t {
    VF_UNPACKED = 0,
    VF_R32_SFLOAT,
    VF_R32G32_SFLOAT,
    VF_R32G32B32_SFLOAT,
    VF_R32G32B32A32_SFLOAT,
    VF_R16G16_SFLOAT, // FLOAT2H
    VF_R8G8B8A8_UNORM, // D3DCOLOR
    VF_R16_SINT,       // SHORT1
    VF_R16G16_SINT,
    VF_R16G16B16_SINT,
    VF_R16G16B16A16_SINT,
    VF_R16_SNORM, // NORMSHORT1
    VF_R16G16_SNORM,
    VF_R16G16B16_SNORM,
    VF_R16G16B16A16_SNORM,
    VF_R8_SNORM, // PBYTE1
    VF_R8G8_SNORM,
    VF_R8G8B8_SNORM,
    VF_R8G8B8A8_SNORM,
    VF_NONE, // nothing is read for this element
};

VertexFormatCode vertex_format_code(uint32_t type);

// True when Vulkan can express the type as a vertex format. False for VF_UNPACKED and VF_NONE: the first
// has to be unpacked by the shader, the second is not read at all.
bool vertex_format_is_native(uint32_t code);

// ---- the layout a pipeline needs ----

struct VertexAttributeDesc {
    uint32_t binding;
    uint32_t location; // the shader register
    uint32_t format;   // a VertexFormatCode
    uint32_t offset;
};

struct VertexBindingDesc {
    uint32_t binding;
    uint32_t stride;
    uint32_t rate; // 0 = per vertex, 1 = per instance
};

struct VertexLayout {
    VertexAttributeDesc attributes[32];
    uint32_t attribute_count = 0;
    VertexBindingDesc bindings[16];
    uint32_t binding_count = 0;
    uint32_t stream_stride[16] = {}; // what each stream was measured to be
};

// Turn a declaration into the layout a graphics pipeline needs. `stream_stride` is what the game told
// SetStreamSource; where it is zero the stride is measured from the declaration instead, because the
// console falls back to the declaration's own layout when the game has not given one.
//
// Returns false and fills `err` when the declaration cannot be turned into a layout at all: an unknown
// element type, a register used twice, or a stream beyond the sixteenth. Those are refused rather than
// patched over, because each of them produces a pipeline that reads the wrong bytes and draws something
// plausible.
bool build_vertex_layout(const VertexElement *elements, uint32_t count, const uint32_t *stream_stride,
                         VertexLayout *out, const char **err);

// ---- reading a declaration ----

// What a shader's declaration turns into. `stream_stride` is measured from the declaration, which is what
// the console used wherever the game had not given a stride of its own through SetStreamSource.
struct VertexDeclaration {
    VertexElement elements[32];
    uint32_t count = 0;
    uint32_t stream_stride[16] = {};
    // CONSTMEM: the game hands the shader's constants here, and the invoice notes them at creation time
    // rather than applying them. Keeping the address and the count is what lets A5 decide what to do
    // instead of losing them.
    uint32_t constant_address = 0;
    uint32_t constant_count = 0;
    bool ended = false;     // the declaration said where it ends
    bool ran_out = false;   // it did not, within the bound
};

// Read a declaration token stream. `max_tokens` bounds the whole walk, including the tokens a CONSTMEM or
// EXT step skips over -- which the obvious loop guard does not, and that is how a malformed declaration
// walks off the end of a buffer the game still owns.
//
// Refused, not truncated: a declaration that runs out is a game bug or a decode mistake, and returning the
// half that happened to parse produces a shader reading registers it was never given.
bool parse_vertex_declaration(const uint32_t *decl, uint32_t max_tokens, VertexDeclaration *out, const char **err);

// The program header the microcode starts with: the low half is the program type (0x2078 normal, 0x6378
// read/write, 0x7778 state), the high half the instruction count.
bool read_program_header(const uint32_t *function, uint32_t *program_type, uint32_t *instructions,
                         const char **err);

} // namespace xd3d