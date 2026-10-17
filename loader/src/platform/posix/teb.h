// Per-thread Xbox TEB anchor on Linux.
//
// Why this file exists
// --------------------
// Game code addresses the Xbox KPCR through the fs segment. On Windows the loader could aim fs at
// Windows TLS slots (xthread.cpp rewrote the displacements to 0xE10 + 4*slot). That is not possible
// here, and it was measured, not assumed (2026-10-17, kernel 6.17.0-29-generic):
//
//   modify_ldt          (syscall 123) -> ENOSYS, every entry_number and contents
//   arch_prctl(ARCH_SET_FS) (384)    -> EINVAL; the i386 arch_prctl_32 only accepts ARCH_SET_GS,
//                                        and GS belongs to glibc
//   set_thread_area    (syscall 243) -> EINVAL
//   %gs:0                          -> valid, per thread, owned by glibc
//   %fs:0                          -> faults; the segment base is 0 and unusable
//
// So instead of stealing %fs we borrow glibc's thread pointer. The Xbox TEB lives in an ordinary
// __thread block, and every `fs:[disp32]` in the image is rewritten to `gs:[disp32 + K]`:
//
//   K = &block - %gs:0        (constant for the whole binary: the glibc TLS layout is identical
//                               on every thread, so K is the same everywhere)
//
// Each rewritten instruction keeps its length. `mov eax, fs:[0x20]` (64 A1 20 00 00 00) becomes
// `mov eax, gs:[0xFFFFFFC4]` (65 A1 C4 FF FF FF) with K = -92: the 0x64 prefix byte becomes 0x65 and
// the displacement gains K. patch_fs_accesses() already rewrites exactly those bytes, so it ports
// over almost unchanged.
//
// Consequences, both budgeted in .agents/plan.md:
//   - fs:[0] (the SEH chain head) can no longer be left alone: there is no host SEH on Linux to
//     share it with, so it is remapped too and seh.h implements the chain itself.
//   - K is computed at runtime, never hardcoded, so a glibc TLS layout change moves K with it.
#pragma once

#include <stddef.h>
#include <stdint.h>

// The Xbox TEB fields the loader remaps, laid out at their original Xbox displacements so that
// gs:[xbox_disp + K] lands exactly on the field. Static asserts below keep the two in step.
struct XboxTeb {
    uint32_t seh;        // 0x00  ExceptionList (SEH chain head)
    uint32_t tls_array;  // 0x04  NtTib.StackBase, used by XAPI as the TLS array
    uint32_t pad_08[6];  // 0x08
    uint32_t prcb;       // 0x20  KPCR.Prcb
    uint32_t irql;       // 0x24  KPCR.Irql (byte on Xbox, kept as a dword)
    uint32_t thread;     // 0x28  KPRCB.CurrentThread
    uint32_t pad_2c[11]; // 0x2C
    uint32_t dpc_active; // 0x58  KPRCB.DpcRoutineActive
};
static_assert(offsetof(struct XboxTeb, seh) == 0x00, "seh must sit at Xbox displacement 0x00");
static_assert(offsetof(struct XboxTeb, tls_array) == 0x04, "tls_array must sit at 0x04");
static_assert(offsetof(struct XboxTeb, prcb) == 0x20, "prcb must sit at 0x20");
static_assert(offsetof(struct XboxTeb, irql) == 0x24, "irql must sit at 0x24");
static_assert(offsetof(struct XboxTeb, thread) == 0x28, "thread must sit at 0x28");
static_assert(offsetof(struct XboxTeb, dpc_active) == 0x58, "dpc_active must sit at 0x58");

// The glibc thread pointer, i.e. %gs:0. Reading it is how K is derived and how the loader verifies
// at runtime that a thread really carries the expected anchor.
uint32_t teb_thread_pointer();

// K = &g_xteb - %gs:0. Computed on the first call, from whichever thread calls it; constant after.
uint32_t teb_k();

// Recompute K in the calling thread and compare it with the process-wide value. The patcher's
// displacements are baked from teb_k(), so a mismatch would mean patched code reads another thread's
// state. Returns false and reports the difference; the loader treats that as fatal.
bool teb_k_matches_here();

// This thread's Xbox TEB. Ordinary pointer access: %gs:0 + K is exactly &g_xteb, so the loader
// itself never needs a segment override -- only the rewritten game instructions do.
XboxTeb *teb();

// gs displacement the patcher must bake for a given Xbox displacement. Returns UINT32_MAX for an
// Xbox displacement that is not remapped, which leaves that instruction untouched.
uint32_t teb_patch_offset(uint32_t xbox_disp);

// Human-readable name of an Xbox displacement, for the patch log.
const char *teb_field_name(uint32_t xbox_disp);

// True if xbox_disp is one of the remapped fields (0x00 included: the SEH head is remapped here).
bool teb_is_remapped(uint32_t xbox_disp);