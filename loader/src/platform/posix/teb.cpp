#include "platform/posix/teb.h"

#include "platform/posix/log.h"

#include "platform/posix/seh.h" // EXCEPTION_CHAIN_END

#include <stdio.h>

// The Xbox TEB for this thread. One per thread by construction -- this is the anchor every rewritten
// game instruction resolves through. The SEH head starts at EXCEPTION_CHAIN_END, as NT's
// ExceptionList does: the game's prologues chain against whatever word they find, and NT's chain
// walkers treat ~0 -- not NULL -- as the empty chain. Everything else starts zero.
static __thread XboxTeb g_xteb = {
    .seh = EXCEPTION_CHAIN_END,
    .stack_base = 0,
    .stack_limit = 0,
    .fiber = 0,
    .pad_10 = {},
    .prcb = 0,
    .irql = 0,
    .thread = 0,
    .pad_2c = {},
    .dpc_active = 0,
    .pad_5c = {},
    .tls_slots = {},
    .tail = {},
};

// K is a property of the binary, not of the thread: glibc lays the TCB and the thread blocks out
// identically on every thread, so &g_xteb sits at the same offset from %gs:0 everywhere. It is
// computed once and then only verified.
static uint32_t g_k;
static bool g_k_valid;

uint32_t teb_thread_pointer()
{
    uint32_t v;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(v));
    return v;
}

// Computes K on first use and caches it. Safe to call from any thread: the glibc TLS layout is the
// same on every thread, so whichever thread gets here first computes the value all of them share.
static uint32_t compute_k()
{
    if (!g_k_valid) {
        g_k = (uint32_t)(uintptr_t)&g_xteb - teb_thread_pointer();
        g_k_valid = true;
        logf("teb: %%gs:0 = 0x%08x  &xteb = 0x%08x  K = 0x%08x (signed %d)", teb_thread_pointer(),
             (uint32_t)(uintptr_t)&g_xteb, g_k, (int)g_k);
    }
    return g_k;
}

uint32_t teb_k() { return compute_k(); }

XboxTeb *teb() { return &g_xteb; }

bool teb_k_matches_here()
{
    // Establish K first if nobody has. This function's answer has to mean "the same value the patcher baked"
    // even when it is the first thing to ask -- which is exactly what happens on a thread the loader did not
    // create, such as the one a caller opens a game thread on before the image is patched.
    const uint32_t process_k = teb_k();
    const uint32_t here = (uint32_t)(uintptr_t)&g_xteb - teb_thread_pointer();
    if (here == process_k) return true;
    logc("teb: K mismatch: process K = 0x%08x, this thread = 0x%08x", process_k, here);
    return false;
}

// Xbox displacement -> what the patch log calls it. The block is laid out at the Xbox's own
// displacements, so every displacement inside it is remapped by adding K; this table is only for names,
// which is what makes a boot log readable ("TLS slot 3" rather than "KPCR +0xE1C").
//
// fs:[0x00] is named because on Linux the SEH chain head is ours to own (seh.h), unlike on Windows where
// it was shared with the host and deliberately left alone.
struct Remap {
    uint32_t xbox_disp;
    const char *what;
};
static const Remap kRemaps[] = {
    {0x00, "ExceptionList (SEH chain head)"},
    {0x04, "NtTib.StackBase (XAPI reads this as the TLS array)"},
    {0x08, "NtTib.StackLimit"},
    {0x0C, "NtTib.FiberData/Fiber"},
    {0x20, "KPCR.Prcb"},
    {0x24, "KPCR.Irql (a byte on the Xbox, a dword here)"},
    {0x28, "KPRCB.CurrentThread"},
    {0x58, "KPRCB.DpcRoutineActive"},
};

uint32_t teb_patch_offset(uint32_t xbox_disp)
{
    // Everything the block covers is answerable, because the block *is* the Xbox's layout. Leaving an
    // access unpatched would be fatal on Linux -- %fs:0 faults -- so the only question is whether the
    // displacement lands inside the block; past that the patcher reports it and refuses.
    if (xbox_disp >= XBOX_DISP_MAX) return UINT32_MAX;
    return teb_k() + xbox_disp;
}

// A name for any displacement, including those with no table entry: the log has to be able to say what
// an access was even when the answer is "somewhere in the KPRCB".
static char g_name_buf[64];

const char *teb_field_name(uint32_t xbox_disp)
{
    for (const Remap &r : kRemaps)
        if (r.xbox_disp == xbox_disp) return r.what;
    if (xbox_disp >= 0xE10 && xbox_disp < 0xE10 + TLS_SLOT_COUNT * 4) {
        snprintf(g_name_buf, sizeof g_name_buf, "TLS slot %u", (xbox_disp - 0xE10) / 4);
        return g_name_buf;
    }
    if (xbox_disp >= 0x20 && xbox_disp < 0x20 + KPRCB_SIZE) {
        snprintf(g_name_buf, sizeof g_name_buf, "KPRCB +0x%x", xbox_disp - 0x20);
        return g_name_buf;
    }
    snprintf(g_name_buf, sizeof g_name_buf, "KPCR +0x%x", xbox_disp);
    return g_name_buf;
}

bool teb_is_remapped(uint32_t xbox_disp) { return xbox_disp < XBOX_DISP_MAX; }
