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
    .tls_array = 0,
    .pad_08 = {},
    .prcb = 0,
    .irql = 0,
    .thread = 0,
    .pad_2c = {},
    .dpc_active = 0,
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
    uint32_t here = (uint32_t)(uintptr_t)&g_xteb - teb_thread_pointer();
    if (here == g_k) return true;
    logc("teb: K mismatch: process K = 0x%08x, this thread = 0x%08x", g_k, here);
    return false;
}

// Xbox displacement -> field. fs:[0x00] is in the table because on Linux the SEH chain head is ours to
// own (seh.h), unlike on Windows where it was shared with the host and deliberately left alone.
struct Remap {
    uint32_t xbox_disp;
    size_t offset;
    const char *what;
};
static const Remap kRemaps[] = {
    {0x00, offsetof(struct XboxTeb, seh), "ExceptionList (SEH chain head)"},
    {0x04, offsetof(struct XboxTeb, tls_array), "NtTib.StackBase (TLS array)"},
    {0x20, offsetof(struct XboxTeb, prcb), "KPCR.Prcb"},
    {0x24, offsetof(struct XboxTeb, irql), "KPCR.Irql"},
    {0x28, offsetof(struct XboxTeb, thread), "KPRCB.CurrentThread"},
    {0x58, offsetof(struct XboxTeb, dpc_active), "KPRCB.DpcRoutineActive"},
};

uint32_t teb_patch_offset(uint32_t xbox_disp)
{
    for (const Remap &r : kRemaps)
        if (r.xbox_disp == xbox_disp) return teb_k() + (uint32_t)r.offset;
    return UINT32_MAX;
}

const char *teb_field_name(uint32_t xbox_disp)
{
    for (const Remap &r : kRemaps)
        if (r.xbox_disp == xbox_disp) return r.what;
    return nullptr;
}

bool teb_is_remapped(uint32_t xbox_disp)
{
    for (const Remap &r : kRemaps)
        if (r.xbox_disp == xbox_disp) return true;
    return false;
}