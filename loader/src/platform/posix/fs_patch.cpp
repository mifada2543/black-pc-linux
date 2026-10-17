#include "platform/posix/fs_patch.h"

#include "platform/posix/teb.h"

#include <stdio.h>
#include <string.h>

namespace {

// The gs segment override that replaces the fs one.
constexpr uint8_t GS = 0x65;
constexpr uint8_t FS = 0x64;

// Opcodes that take a ModRM operand and appear with an fs: prefix in XDK code (loads, stores, ALU).
// Kept identical to xthread.cpp's modrm_opcode() so both scanners accept the same instruction set.
bool modrm_opcode(uint8_t op)
{
    switch (op) {
    case 0x01: case 0x03: case 0x09: case 0x0B: case 0x21: case 0x23: case 0x29: case 0x2B:
    case 0x31: case 0x33: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x84: case 0x85:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x80: case 0x81: case 0x83: case 0xC6:
    case 0xC7: case 0xF6: case 0xF7: case 0xFF: case 0x8F:
        return true;
    }
    return false;
}

// Classify an fs-relative instruction. `p` points at the 0x64 prefix. On success *disp_offset is the
// offset of the four displacement bytes and *xbox_disp is their value.
//
// Covers the three encodings XDK code uses:
//   moffs form          64 A0/A1/A2/A3 disp32          mov al/eax, fs:[x] / mov fs:[x], al/eax
//   movzx from moffs    64 0F B6/B7/BE/BF 05 disp32    movzx r32, byte/word fs:[x]
//   ModRM with disp32   64 <op> modrm=05 disp32       the general case
bool classify(const uint8_t *p, uint32_t avail, uint32_t *disp_offset)
{
    if (avail < 3 || p[0] != FS) return false;
    uint8_t op = p[1];

    if (op == 0xA0 || op == 0xA1 || op == 0xA2 || op == 0xA3) {
        // prefix + opcode + disp32 == 6 bytes. This form is by far the most common one in XDK code
        // (`mov eax, fs:[0x20]` and friends), so the guard here has to be exactly 6.
        if (avail < 6) return false;
        *disp_offset = 2;
        return true;
    }
    if (op == 0x0F && avail >= 8 &&
        (p[2] == 0xB6 || p[2] == 0xB7 || p[2] == 0xBE || p[2] == 0xBF) && (p[3] & 0xC7) == 0x05) {
        *disp_offset = 4;
        return true;
    }
    if (modrm_opcode(op) && (p[2] & 0xC7) == 0x05) {
        // prefix + opcode + modrm + disp32 == 7 bytes is the shortest this form can be. An earlier
        // guard of 8 was off by one and silently skipped any fs access that happened to be the
        // last seven bytes of a section.
        if (avail < 7) return false;
        *disp_offset = 3;
        return true;
    }
    return false;
}

} // namespace

bool fs_patch_recognises(const uint8_t *p, uint32_t avail, uint32_t *disp_offset, uint32_t *xbox_disp)
{
    uint32_t off = 0;
    if (!classify(p, avail, &off)) return false;
    if (off + 4 > avail) return false;
    uint32_t d;
    memcpy(&d, p + off, 4);
    if (disp_offset) *disp_offset = off;
    if (xbox_disp) *xbox_disp = d;
    return true;
}

FsPatchStats patch_fs_to_gs(uint8_t *code, uint32_t size)
{
    FsPatchStats st;
    uint32_t k = teb_k();

    // The shortest form we rewrite is 6 bytes (`64 A1 disp32`, the moffs loads and stores, which are
    // the commonest in XDK code), so the scan has to reach that far. A bound of 7 silently skipped
    // every one of them.
    for (uint32_t i = 0; i + 6 <= size; i++) {
        if (code[i] != FS) continue;

        uint32_t disp_off = 0;
        if (!classify(code + i, size - i, &disp_off) || disp_off + 4 > size) {
            // fs is still present but not on a form we rewrite. Count it so the caller can report
            // an XBE whose instruction mix shifted, rather than silently leaving broken code.
            st.no_disp++;
            continue;
        }

        uint32_t disp;
        memcpy(&disp, code + i + disp_off, 4);

        uint32_t patched_disp = teb_patch_offset(disp);
        if (patched_disp == UINT32_MAX) {
            // Not one of ours. Leave the bytes alone; log small offsets because those are the ones
            // that would be a missing entry in teb.cpp's table.
            if (disp < 0x100) {
                fprintf(stderr, "fs patch: unhandled fs:[0x%x] at +0x%x, left as is\n", disp, i);
                st.unknown++;
            }
            continue;
        }

        code[i] = GS;
        // A doubly-prefixed instruction (`mov eax, fs:[fs:[0x20]]`) would otherwise keep a stray fs
        // override in front of a gs access. Both prefixes mean the same thing once the displacement
        // has moved to gs, so normalise the run.
        for (uint32_t j = i; j > 0 && code[j - 1] == FS; j--)
            code[j - 1] = GS;
        st.prefixes++;

        memcpy(code + i + disp_off, &patched_disp, 4);
        fprintf(stderr, "fs patch: +0x%06x fs:[0x%02x] -> gs:[0x%08x]  %s\n", i, disp, patched_disp,
                teb_field_name(disp));
        st.patched++;

        // Resume after the last displacement byte. classify() returns an offset relative to the
        // instruction start, so this has to be added to i -- setting i = disp_off + 3 (as the Windows
        // version does, where disp_off is absolute) rewinds the scan, and it re-patches bytes it has
        // already rewritten, corrupting the code it is supposed to fix.
        i = i + disp_off + 3;
    }

    (void)k; // k is already folded into teb_patch_offset()
    return st;
}