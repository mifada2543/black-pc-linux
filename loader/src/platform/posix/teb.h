// Per-thread Xbox anchor on Linux: the KPCR the game addresses through the fs segment.
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
// So instead of stealing %fs we borrow glibc's thread pointer. The Xbox KPCR lives in an ordinary
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
//
// The block's shape
// -----------------
// The block is laid out at the Xbox's own displacements, so no displacement needs remapping beyond adding
// K. KPCR.Prcb stays a *pointer* -- the game dereferences it -- and the values at 0x24, 0x28 and 0x58 sit
// where the game reads them, exactly as the Windows port arranged with one TLS slot each. The thread layer
// keeps the KPRCB's own copies of those three in step, so both ways of reading them agree.
//
// 0xE10 is where the TLS slots are (NT's TLS_MINIMUM_AVAILABLE = 128, and the Xbox's TEB/KPCR layout
// matches: the Windows port needed no rewriting at all for those accesses, because Windows' own TEB has
// the same array at the same displacement). This block covers the whole first page, so every fs access
// the game can reasonably make lands somewhere defined; an access past the end is reported by the
// patcher and left alone, which faults loudly rather than reading another thread's state.
#pragma once

#include <stddef.h>
#include <stdint.h>

// The fake KPRCB, which the game reaches through KPCR.Prcb. The Xbox's KPCR holds a *pointer* to the
// KPRCB at +0x20 and keeps Irql, CurrentThread and DpcRoutineActive at their own displacements; the
// Windows port therefore mapped each of those five displacements to its own TLS slot rather than relying
// on the KPRCB's memory. This block keeps that shape -- the pointer is a pointer, and the three values sit
// where the game reads them -- and the thread layer keeps the KPRCB's copies in step, so code that
// dereferences the pointer sees the same values code that reads fs:[0x24] does.
constexpr uint32_t KPRCB_SIZE = 0x300; // as generous as the reference's fake KPRCB
constexpr uint32_t TLS_SLOT_COUNT = 128;

struct XboxKprcb {
    uint32_t pad_00;              // 0x00 AffinityMask is 64 bits on NT
    uint32_t irql;                // 0x04 mirrors KPCR.Irql
    uint32_t current_thread;      // 0x08 mirrors KPRCB.CurrentThread
    uint32_t pad_0c[11];          // 0x0C
    uint32_t dpc_routine_active;  // 0x38 mirrors KPCR.DpcRoutineActive
    uint32_t pad_3c[KPRCB_SIZE / 4 - 15];
};
static_assert(offsetof(struct XboxKprcb, irql) == 0x04, "KPRCB.Irql is at +0x04");
static_assert(offsetof(struct XboxKprcb, current_thread) == 0x08, "KPRCB.CurrentThread is at +0x08");
static_assert(offsetof(struct XboxKprcb, dpc_routine_active) == 0x38, "KPRCB.DpcRoutineActive is at +0x38");
static_assert(sizeof(struct XboxKprcb) == KPRCB_SIZE, "the fake KPRCB is KPRCB_SIZE bytes");

struct XboxTeb {
    uint32_t seh;        // 0x00  ExceptionList (SEH chain head)
    uint32_t stack_base; // 0x04  NtTib.StackBase, which XAPI reads as the TLS array
    uint32_t stack_limit;// 0x08  NtTib.StackLimit
    uint32_t fiber;      // 0x0C  NtTib.FiberData/Fiber
    uint32_t pad_10[4];  // 0x10
    uint32_t prcb;       // 0x20  KPCR.Prcb: a pointer to this thread's fake KPRCB
    uint32_t irql;       // 0x24  KPCR.Irql (a byte on the Xbox, a dword here)
    uint32_t thread;     // 0x28  KPRCB.CurrentThread, read at this displacement on the Xbox
    uint32_t pad_2c[11]; // 0x2C
    uint32_t dpc_active; // 0x58  KPRCB.DpcRoutineActive, read at this displacement on the Xbox
    uint32_t pad_5c[(0xE10 - 0x5C) / 4];
    uint32_t tls_slots[TLS_SLOT_COUNT]; // 0xE10  the TLS slot array
    // 0xE10 + 128 slots reaches 0x1010, which is past the first page: the covered range is two pages.
    uint32_t tail[(0x2000 - 0xE10 - TLS_SLOT_COUNT * 4) / 4];
};
static_assert(offsetof(struct XboxTeb, seh) == 0x00, "seh must sit at Xbox displacement 0x00");
static_assert(offsetof(struct XboxTeb, stack_base) == 0x04, "stack_base must sit at 0x04");
static_assert(offsetof(struct XboxTeb, stack_limit) == 0x08, "stack_limit must sit at 0x08");
static_assert(offsetof(struct XboxTeb, prcb) == 0x20, "prcb must sit at 0x20");
static_assert(offsetof(struct XboxTeb, irql) == 0x24, "irql must sit at 0x24");
static_assert(offsetof(struct XboxTeb, thread) == 0x28, "thread must sit at 0x28");
static_assert(offsetof(struct XboxTeb, dpc_active) == 0x58, "dpc_active must sit at 0x58");
static_assert(offsetof(struct XboxTeb, tls_slots) == 0xE10, "the TLS slots must sit at 0xE10");
static_assert(sizeof(struct XboxTeb) == 0x2000, "the block covers everything the game can address in it");

// The largest Xbox displacement this port answers. An fs access past it cannot be satisfied and the
// patcher says so rather than writing a displacement that lands in memory this block does not own.
constexpr uint32_t XBOX_DISP_MAX = sizeof(struct XboxTeb);

// The glibc thread pointer, i.e. %gs:0. Reading it is how K is derived and how the loader verifies
// at runtime that a thread really carries the expected anchor.
uint32_t teb_thread_pointer();

// K = &g_xteb - %gs:0. Computed on the first call, from whichever thread calls it; constant after.
uint32_t teb_k();

// Recompute K in the calling thread and compare it with the process-wide value. The patcher's
// displacements are baked from teb_k(), so a mismatch would mean patched code reads another thread's
// state. Returns false and reports the difference; the loader treats that as fatal.
bool teb_k_matches_here();

// This thread's Xbox block. Ordinary pointer access: %gs:0 + K is exactly &g_xteb, so the loader
// itself never needs a segment override -- only the rewritten game instructions do.
XboxTeb *teb();

// gs displacement the patcher must bake for a given Xbox displacement. Every displacement inside the
// block is remapped, because the block *is* laid out at the Xbox's displacements; UINT32_MAX means the
// displacement is past the end of the block, which leaves that instruction untouched.
uint32_t teb_patch_offset(uint32_t xbox_disp);

// Human-readable name of an Xbox displacement, for the patch log. "TLS slot N" and "KPCR +0x..",
// "KPRCB +0x.." where they are known, and "KPCR +0x.." generically otherwise.
const char *teb_field_name(uint32_t xbox_disp);

// True if xbox_disp is one the block answers (0x00 included: the SEH head is remapped here).
bool teb_is_remapped(uint32_t xbox_disp);