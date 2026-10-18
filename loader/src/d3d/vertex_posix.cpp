#include "d3d/vertex_posix.h"

#include <stdio.h>
#include <string.h>

namespace xd3d {

// Bytes one element occupies. Transcribed from the console's own table rather than computed: the widths are
// irregular (FLOAT3 is 12 bytes but PBYTE3 is 3, and NORMPACKED3 is 4 bytes carrying three components), so
// there is nothing to derive them from. A type this build does not know has size zero, which is what makes
// an unknown token in a declaration visible instead of a stride that quietly goes wrong.
uint32_t vsdt_size(uint32_t type)
{
    switch (type) {
    case VST_FLOAT1: return 4;
    case VST_FLOAT2: return 8;
    case VST_FLOAT3: return 12;
    case VST_FLOAT4: return 16;
    case VST_FLOAT2H: return 4;
    case VST_D3DCOLOR: return 4;
    case VST_SHORT1: return 2;
    case VST_SHORT2: return 4;
    case VST_SHORT3: return 6;
    case VST_SHORT4: return 8;
    case VST_NORMSHORT1: return 2;
    case VST_NORMSHORT2: return 4;
    case VST_NORMSHORT3: return 6;
    case VST_NORMSHORT4: return 8;
    case VST_NORMPACKED3: return 4;
    case VST_PBYTE1: return 1;
    case VST_PBYTE2: return 2;
    case VST_PBYTE3: return 3;
    case VST_PBYTE4: return 4;
    case VST_NONE: return 0;
    default: return 0;
    }
}

// The two halves of this table are different in kind and are written differently on purpose. Sizes come
// from the console; the Vulkan side is a decision, and the decision is that a type with no Vulkan format is
// reported as such. SHORT and NORMSHORT differ only in whether the shader sign-extends or normalises them,
// which is a shader-side question -- the integer formats below are what a vertex buffer can hold, and the
// translated shader reads them as signed or unsigned according to which of the two the console meant.
VertexFormatCode vertex_format_code(uint32_t type)
{
    switch (type) {
    case VST_FLOAT1: return VF_R32_SFLOAT;
    case VST_FLOAT2: return VF_R32G32_SFLOAT;
    case VST_FLOAT3: return VF_R32G32B32_SFLOAT;
    case VST_FLOAT4: return VF_R32G32B32A32_SFLOAT;
    case VST_FLOAT2H: return VF_R16G16_SFLOAT;
    case VST_D3DCOLOR: return VF_R8G8B8A8_UNORM;
    case VST_SHORT1: return VF_R16_SINT;
    case VST_SHORT2: return VF_R16G16_SINT;
    case VST_SHORT3: return VF_R16G16B16_SINT;
    case VST_SHORT4: return VF_R16G16B16A16_SINT;
    case VST_NORMSHORT1: return VF_R16_SNORM;
    case VST_NORMSHORT2: return VF_R16G16_SNORM;
    case VST_NORMSHORT3: return VF_R16G16B16_SNORM;
    case VST_NORMSHORT4: return VF_R16G16B16A16_SNORM;
    case VST_PBYTE1: return VF_R8_SNORM;
    case VST_PBYTE2: return VF_R8G8_SNORM;
    case VST_PBYTE3: return VF_R8G8B8_SNORM;
    case VST_PBYTE4: return VF_R8G8B8A8_SNORM;
    // Three signed bytes in one dword, with the top byte's sign replicated. There is no Vulkan format for
    // it, and the near neighbours (RGBA8, RGBA16) would put the components roughly in place and leave the
    // fourth holding buffer bytes the shader never meant to read.
    case VST_NORMPACKED3: return VF_UNPACKED;
    case VST_NONE: return VF_NONE;
    default: return VF_NONE;
    }
}

bool vertex_format_is_native(uint32_t code)
{
    return code != VF_UNPACKED && code != VF_NONE;
}

// ---- reading a declaration ----
//
// A declaration is a stream of dwords whose top three bits say what each one is. The widths are all
// irregular, so the token stream is read rather than decoded from a table -- but every step that moves the
// cursor also moves the budget, which is the part that has to be got right. A CONSTMEM token can carry a
// count of fifteen, and a run of them walks thousands of dwords past the end of a buffer the game still
// owns without ever going round the loop again.
bool parse_vertex_declaration(const uint32_t *decl, uint32_t max_tokens, VertexDeclaration *out, const char **err)
{
    if (err) *err = nullptr;
    if (!out) return false;
    *out = VertexDeclaration{};
    if (!decl) {
        if (err) *err = "no declaration";
        return false;
    }

    uint32_t stream = 0;
    uint32_t offset[16] = {};
    uint32_t used = 0;

    while (used < max_tokens) {
        const uint32_t tok = decl[used++];
        const uint32_t type = tok >> 29;

        if (tok == 0xFFFFFFFFu || type == 7) {
            out->ended = true;
            break;
        }
        switch (type) {
        case 0: // NOP
            break;
        case 1: // STREAM
            stream = tok & 0xF;
            break;
        case 2: // STREAMDATA
            if (tok & 0x10000000u) {
                // A gap the declaration states outright: the shader does not read this space, but it still
                // occupies it. Skipping it is what keeps the offsets after it right.
                const uint32_t n = (tok >> 16) & 0xF;
                offset[stream] += (tok & 0x08000000u) ? n : n * 4;
            } else if (out->count < 32) {
                VertexElement &e = out->elements[out->count++];
                e.stream = (uint8_t)stream;
                e.reg = (uint8_t)(tok & 0x1F);
                e.type = (uint8_t)((tok >> 16) & 0xFF);
                e.offset = (uint16_t)offset[stream];
                offset[stream] += vsdt_size(e.type);
                // A width of zero here means the token named a type this build does not know. The offset
                // does not move, so every element after it would be read from the wrong place -- which is
                // why it is refused rather than laid out as best it can.
                if (!e.type || (e.type != VST_NONE && !vsdt_size(e.type))) {
                    if (err) *err = "the declaration names a vertex type this build does not know";
                    return false;
                }
            } else {
                if (err) *err = "the declaration has more elements than the console had room for";
                return false;
            }
            break;
        case 3: // TESSELLATOR
            break;
        case 4: { // CONSTMEM: count in bits 25-28, address in bits 0-7, then four dwords per constant
            const uint32_t count = (tok >> 25) & 0xF;
            out->constant_address = tok & 0xFF;
            out->constant_count = count;
            // Four dwords per constant, and the budget moves with it -- this is the step that turns a
            // malformed declaration into a read past the end.
            const uint64_t skip = (uint64_t)count * 4;
            if ((uint64_t)used + skip > max_tokens) {
                if (err) *err = "the declaration's constant block runs past its end";
                return false;
            }
            used += (uint32_t)skip;
            break;
        }
        case 5: // EXT
            used += (tok >> 24) & 0x1F;
            if (used > max_tokens) {
                if (err) *err = "the declaration's extension block runs past its end";
                return false;
            }
            break;
        default:
            // Type 6 and anything above: not a token this console emitted. Stopping is right and continuing
            // is not, because the stream's shape is no longer known.
            if (err) *err = "the declaration contains a token kind this build does not know";
            return false;
        }
    }

    if (!out->ended) {
        if (err) *err = "the declaration never said where it ends";
        return false;
    }
    for (uint32_t s = 0; s < 16; s++) out->stream_stride[s] = offset[s];
    return true;
}

bool read_program_header(const uint32_t *function, uint32_t *program_type, uint32_t *instructions,
                         const char **err)
{
    if (err) *err = nullptr;
    if (!function || !program_type || !instructions) {
        if (err) *err = "no program to read";
        return false;
    }
    const uint32_t header = function[0];
    *program_type = header & 0xFFFFu;
    *instructions = header >> 16;
    if (!*instructions) {
        if (err) *err = "a program with no instructions in it";
        return false;
    }
    return true;
}

bool build_vertex_layout(const VertexElement *elements, uint32_t count, const uint32_t *stream_stride,
                         VertexLayout *out, const char **err)
{
    if (err) *err = nullptr;
    if (!out) return false;
    *out = VertexLayout{};
    if (count > 32) {
        if (err) *err = "a vertex declaration with more than 32 elements is not one this console had";
        return false;
    }

    uint32_t seen_regs = 0;
    uint32_t end_of[16] = {};
    bool stream_used[16] = {};

    for (uint32_t i = 0; i < count; i++) {
        const VertexElement &e = elements[i];

        if (e.stream >= 16) {
            if (err) *err = "an element on a stream past the sixteenth";
            return false;
        }
        // A register the game has already defined is a second declaration of the same input. The console
        // took the first one; taking the last would move an offset a shader is already compiled against.
        if (e.reg < 32) {
            if (seen_regs & (1u << e.reg)) {
                if (err) *err = "a register declared twice";
                return false;
            }
            seen_regs |= 1u << e.reg;
        }

        const uint32_t size = vsdt_size(e.type);
        const VertexFormatCode code = vertex_format_code(e.type);
        if (code == VF_NONE && e.type != VST_NONE) {
            if (err) *err = "an element type this build does not know";
            return false;
        }

        if (code != VF_NONE) {
            if (out->attribute_count >= 32) {
                if (err) *err = "more attributes than the layout holds";
                return false;
            }
            VertexAttributeDesc &a = out->attributes[out->attribute_count++];
            a.binding = e.stream;
            a.location = e.reg;
            a.format = (uint32_t)code;
            a.offset = e.offset;
        }

        stream_used[e.stream] = true;
        const uint32_t tail = (uint32_t)e.offset + size;
        if (tail > end_of[e.stream]) end_of[e.stream] = tail;
    }

    // The stride for a stream the game gave one is the game's; where it gave none, it is however far the
    // declaration reaches, which is what the console used. A zero stride would be a pipeline with a
    // binding that reads the same vertex forever.
    uint32_t binding = 0;
    for (uint32_t s = 0; s < 16; s++) {
        if (!stream_used[s]) continue;
        uint32_t stride = stream_stride ? stream_stride[s] : 0;
        if (!stride) stride = end_of[s];
        if (!stride) {
            if (err) *err = "a stream with no stride, from neither the game nor the declaration";
            return false;
        }
        out->stream_stride[s] = stride;
        VertexBindingDesc &b = out->bindings[out->binding_count++];
        b.binding = binding;
        b.stride = stride;
        b.rate = 0;
        // The attribute rows name the *console's* stream; Vulkan needs the binding. Renumbering here is
        // the one place the two numberings meet, and doing it in one place is what keeps them in step.
        for (uint32_t a = 0; a < out->attribute_count; a++)
            if (out->attributes[a].binding == s) out->attributes[a].binding = binding;
        binding++;
    }

    return true;
}

} // namespace xd3d