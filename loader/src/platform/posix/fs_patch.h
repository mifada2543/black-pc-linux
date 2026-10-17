// Rewriting the game's fs-relative accesses so they reach our TEB on Linux.
//
// This is the Linux port of loader/src/xthread.cpp's patch_fs_accesses(). The scanning is identical;
// three things differ, all of them because %fs is not ours to aim here (see teb.h):
//
//   Windows (xthread.cpp)          Linux (here)
//   ---------------------------     -----------------------------------------------
//   prefix 0x64 left alone          prefix 0x64 becomes 0x65 (fs -> gs)
//   disp -> 0xE10 + 4*TlsAlloc      disp -> K + offsetof(XboxTeb, field), K = &xteb - %gs:0
//   fs:[0] skipped (shared chain)   fs:[0] remapped too; the SEH chain is ours (seh.h)
//
// The instruction length never changes, so nothing downstream has to relocate code: the scanner
// writes the prefix byte and the four displacement bytes in place, exactly as the Windows one did.
#pragma once

#include <stdint.h>

struct FsPatchStats {
    uint32_t patched = 0; // rewritten to gs:[K + field]
    uint32_t unknown = 0; // small fs offsets we do not remap; left as they were
    uint32_t no_disp = 0; // fs-relative instructions with a register operand, not a displacement
    uint32_t prefixes = 0;
};

// Scan a block of mapped code and rewrite its fs-relative accesses. `code`/`size` is one section;
// callers walk the section list the way patch_fs_accesses() does, so the same section filtering
// (.rdata, .data, .XTLID, $$*) applies.
FsPatchStats patch_fs_to_gs(uint8_t *code, uint32_t size);

// True if the byte sequence at `p` looks like one of the fs-relative forms we rewrite. Used by the
// test to confirm the fixtures are the shapes we think they are.
bool fs_patch_recognises(const uint8_t *p, uint32_t avail, uint32_t *disp_offset, uint32_t *xbox_disp);