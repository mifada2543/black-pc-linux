// Unit tests for the Linux TEB anchor and the mini-SEH chain.
//
// No XBE needed: everything here is exercised directly. Run with:
//   loader/build/linux/tests/test_platform
#include "platform/posix/fs_patch.h"
#include "platform/posix/seh.h"
#include "platform/posix/teb.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <vector>

static int g_failures;
static int g_checks;

static void check(bool cond, const char *what, const char *detail = nullptr)
{
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        g_failures++;
        printf("  FAIL %s%s%s\n", what, detail ? " -- " : "", detail ? detail : "");
    }
}

// ---------------------------------------------------------------------------
// TEB anchor
// ---------------------------------------------------------------------------

static int g_thread_k_ok;
static uint32_t g_main_k;

static void *teb_thread(void *arg)
{
    (void)arg;
    uint32_t k = teb_k();
    g_thread_k_ok = (k == g_main_k) && teb_k_matches_here();

    // This thread's block must be its own, not the main thread's.
    XboxTeb *t = teb();
    t->irql = 0x22222222u;
    t->dpc_active = 0x33333333u;
    return nullptr;
}

static void test_teb()
{
    printf("\nTEB anchor\n");
    XboxTeb *t = teb();
    g_main_k = teb_k();

    check(teb_k_matches_here(), "K recomputed on this thread matches the process value");
    check(teb_thread_pointer() != 0, "%gs:0 is a real glibc TCB pointer");

    // The patched displacement for each remapped Xbox field must land exactly on the field.
    struct { uint32_t xbox; size_t want; } cases[] = {
        {0x00, offsetof(XboxTeb, seh)},
        {0x04, offsetof(XboxTeb, tls_array)},
        {0x20, offsetof(XboxTeb, prcb)},
        {0x24, offsetof(XboxTeb, irql)},
        {0x28, offsetof(XboxTeb, thread)},
        {0x58, offsetof(XboxTeb, dpc_active)},
    };
    bool all_land = true;
    for (auto &c : cases) {
        uint32_t disp = teb_patch_offset(c.xbox);
        // gs:[disp] resolves to tcb + disp == &xteb + want
        if (teb_thread_pointer() + disp != (uint32_t)(uintptr_t)t + c.want) all_land = false;
    }
    check(all_land, "every remapped displacement resolves onto its field");

    check(teb_patch_offset(0x34) == UINT32_MAX, "an unknown Xbox displacement is left unpatched");
    check(!teb_is_remapped(0x34) && teb_is_remapped(0x00),
          "fs:[0x00] is remapped on Linux (unlike Windows) while others are not");

    // Round-trip through the addressing a rewritten instruction would use.
    t->prcb = 0xDEAD0001u; t->irql = 0xDEAD0002u; t->thread = 0xDEAD0003u;
    t->tls_array = 0xDEAD0004u; t->dpc_active = 0xDEAD0005u; t->seh = 0xDEAD0006u;
    bool rt = t->prcb == 0xDEAD0001u && t->irql == 0xDEAD0002u && t->thread == 0xDEAD0003u &&
              t->tls_array == 0xDEAD0004u && t->dpc_active == 0xDEAD0005u && t->seh == 0xDEAD0006u;
    check(rt, "all six fields round-trip");

    // Second thread: same K, separate storage.
    pthread_t th;
    if (pthread_create(&th, nullptr, teb_thread, nullptr) != 0) {
        check(false, "pthread_create");
        return;
    }
    pthread_join(th, nullptr);
    check(g_thread_k_ok, "K is identical on a second thread");
    check(t->irql == 0xDEAD0002u && t->dpc_active == 0xDEAD0005u,
          "the other thread's writes did not leak into this one");
}

// ---------------------------------------------------------------------------
// mini-SEH
// ---------------------------------------------------------------------------

// Handlers shaped like the ones the game's CRT installs: a searcher, a resumer that fixes the
// context, an unwind-pass recorder, and a protector frame like _local_unwind2's.
static int g_visited;
static ExceptionRegistration *g_visit_order[8];
static ExceptionRecord g_seen_record;
static uint32_t g_seen_flags;
static uint32_t g_seen_eip;
static uint32_t g_seen_ebp;
static uint32_t g_seen_esp;
static uint32_t g_seen_eax[8];

static long handler_reject(ExceptionRecord *r, void *frame, Context *c, void *d)
{
    (void)c;
    (void)d;
    if (g_visited < 8) g_visit_order[g_visited] = (ExceptionRegistration *)frame;
    g_visited++;
    g_seen_record = *r;
    return ExceptionContinueSearch; // not mine -- let the next frame try
}

static long handler_execute(ExceptionRecord *r, void *frame, Context *c, void *d)
{
    (void)r;
    (void)frame;
    (void)d;
    g_visited++;
    c->eip += 2; // fix the cause and skip the faulting instruction; the CONTEXT is the resume state
    return ExceptionContinueExecution;
}

static long handler_unwind(ExceptionRecord *r, void *frame, Context *c, void *d)
{
    (void)d;
    if (g_visited < 8) {
        g_visit_order[g_visited] = (ExceptionRegistration *)frame;
        g_seen_eax[g_visited] = c->eax;
    }
    g_visited++;
    g_seen_record = *r;
    g_seen_flags = r->exception_flags;
    g_seen_eip = c->eip;
    g_seen_ebp = c->ebp;
    g_seen_esp = c->esp;
    c->eax += 1; // the Windows test suite pins that this edit lands in RtlUnwind's return value
    return ExceptionContinueSearch;
}

// A collided unwind, as the CRT's __unwind_handler reports one: it stores its own registration record
// in the dispatcher slot, and the outer walk unlinks that frame and resumes at its prev.
static long handler_collide(ExceptionRecord *r, void *frame, Context *c, void *d)
{
    (void)r;
    (void)c;
    if (g_visited < 8) g_visit_order[g_visited] = (ExceptionRegistration *)frame;
    g_visited++;
    *(ExceptionRegistration **)d = (ExceptionRegistration *)frame;
    return ExceptionCollidedUnwind;
}

static void push_frame(ExceptionRegistration *rec, void *handler, ExceptionRegistration *prev)
{
    rec->handler = handler;
    rec->prev = prev;
    teb()->seh = (uint32_t)(uintptr_t)rec;
}

// A hand-written stdcall call site for seh_unwind, standing in for the game's CRT: it pushes the four
// arguments itself, exactly as _global_unwind2 does before RtlUnwind, and hands back ESP and EBP as
// they were before the pushes, so the context the unwind builds can be checked exactly -- no C frame
// can know those values, because the compiler owns the stack. It also names the instruction after the
// call: that is what an unwind handler must see as context->Eip, and what a synthesised record must
// carry as ExceptionAddress (the Wine suite's hand-assembled code_mem pins the same thing). Being
// hand-written, the call also reaches seh_unwind off the 16-byte psABI alignment (four pushes from an
// aligned C site) -- the kind of stack MSVC-built game callers provide -- so a function that starts
// relying on 16-byte stack alignment will fault here first.
extern "C" uint32_t call_unwind_asm(void *target, void *target_ip, ExceptionRecord *rec, uint32_t rv, uint32_t *esp_out,
                                    uint32_t *ebp_out);
extern "C" uint32_t unwind_asm_return_ip; // the text address of the instruction after that call

__asm__(".text\n"
        ".globl call_unwind_asm\n"
        ".type call_unwind_asm, @function\n"
        "call_unwind_asm:\n"      // (target, target_ip, rec, rv, esp_out, ebp_out)
        "    movl 20(%esp), %edx\n" // esp_out, loaded while the arguments are still where the caller left them
        "    movl 24(%esp), %ecx\n" // ebp_out
        "    movl %esp, (%edx)\n"
        "    movl %ebp, (%ecx)\n"
        "    pushl 16(%esp)\n"     // rv; every push moves ESP by four, so the next argument is at 16(%esp) again
        "    pushl 16(%esp)\n"     // rec
        "    pushl 16(%esp)\n"     // target_ip
        "    pushl 16(%esp)\n"     // target
        "    call seh_unwind\n"
        "unwind_asm_return_addr:\n"
        "    ret\n"
        ".size call_unwind_asm, .-call_unwind_asm\n"
        ".data\n"
        ".align 4\n"
        ".globl unwind_asm_return_ip\n"
        "unwind_asm_return_ip:\n"
        "    .long unwind_asm_return_addr\n"
        ".text\n"); // GCC emits everything after a top-level __asm__ assuming .text; leave it that way.

static void test_seh()
{
    printf("\nmini-SEH\n");

    ExceptionRecord rec;
    memset(&rec, 0, sizeof rec);
    rec.exception_code = EXCEPTION_ACCESS_VIOLATION;
    rec.exception_address = (void *)0x00123456u;
    rec.number_parameters = 2;
    rec.exception_information[0] = 0x00123456u;

    Context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.eip = 0x00123456u;

    SehDispatch d;

    // 1. Empty chain, in both sentinels a real chain can end on -> search, unhandled.
    for (uint32_t head : {EXCEPTION_CHAIN_END, 0u}) {
        teb()->seh = head;
        long rc = seh_dispatch(&rec, &ctx, &d);
        check(rc == ExceptionContinueSearch && d.unhandled, "empty chain reports the fault unhandled");
    }

    // 2. Two frames, both search -> exhausted, both visited, newest first.
    ExceptionRegistration a, b;
    g_visited = 0;
    push_frame(&a, (void *)handler_reject, nullptr);
    push_frame(&b, (void *)handler_reject, &a);
    long rc = seh_dispatch(&rec, &ctx, &d);
    check(rc == ExceptionContinueSearch && d.unhandled, "a chain of searchers exhausts");
    check(g_visited == 2, "both handlers ran");
    check(g_visit_order[0] == &b && g_visit_order[1] == &a, "search order is newest to oldest");
    check(g_seen_record.exception_code == EXCEPTION_ACCESS_VIOLATION, "the handler saw the exception code");
    check(g_seen_record.exception_information[0] == 0x00123456u, "exception information survived");

    // 3. Inner frame searches, outer frame resumes -> dispatch stops and names the context.
    g_visited = 0;
    ctx.eip = 0x00123456u;
    push_frame(&a, (void *)handler_execute, nullptr);
    push_frame(&b, (void *)handler_reject, &a);
    rc = seh_dispatch(&rec, &ctx, &d);
    check(rc == ExceptionContinueExecution && !d.unhandled, "an outer frame resumed the fault");
    check(g_visited == 2 && g_visit_order[0] == &b && g_visit_order[1] == &a,
          "the inner frame searched, then the outer frame resumed");
    check(d.resume_context == &ctx, "resume_context points at the faulting context");

    // 4. RtlRaiseException resumes from the CONTEXT, not from ExceptionAddress.
    ctx.eip = 0x00123456u;
    teb()->seh = (uint32_t)(uintptr_t)&a;
    g_visited = 0;
    bool resumed = seh_raise(&rec, &ctx);
    check(resumed && ctx.eip == 0x00123458u, "seh_raise applies the handler's EIP change (0x123456 -> 0x123458)");
    check(rec.exception_address == (void *)0x00123456u, "record->ExceptionAddress is not the resume point");

    // 5. A NONCONTINUABLE record is not resumable: NT would raise STATUS_NONCONTINUABLE_EXCEPTION as a
    //    fresh exception; this port reports it as unhandled (one diagnostic line on stderr).
    rec.exception_flags |= EXCEPTION_NONCONTINUABLE;
    ctx.eip = 0x00123456u;
    g_visited = 0;
    rc = seh_dispatch(&rec, &ctx, &d);
    check(rc == ExceptionContinueSearch && d.unhandled && g_visited == 1,
          "a non-continuable exception is not resumed");
    rec.exception_flags &= ~EXCEPTION_NONCONTINUABLE;

    // 6. RtlUnwind to a frame: every frame above it is called in unwind mode, then unlinked, and the
    //    running EAX comes back. The records are one array so that addresses increase with age, as a
    //    stack's do: f[0] is at the lowest address and is the newest frame, f[3] the oldest. (Separate
    //    locals can be laid out in any order -- the walk's address check caught exactly that here.)
    ExceptionRegistration f[4];
    push_frame(&f[3], (void *)handler_unwind, nullptr); // oldest first, then down the stack
    push_frame(&f[2], (void *)handler_unwind, &f[3]);
    push_frame(&f[1], (void *)handler_unwind, &f[2]);
    push_frame(&f[0], (void *)handler_unwind, &f[1]);

    // Unwinding to the newest frame itself must do nothing at all.
    g_visited = 0;
    uint32_t asm_esp, asm_ebp;
    uint32_t rv = call_unwind_asm(&f[0], nullptr, &rec, 0xDEAD0000u, &asm_esp, &asm_ebp);
    check(rv == 0xDEAD0000u && g_visited == 0, "unwinding to the newest frame calls nothing and returns EAX");

    // To f[2]: f[0] and f[1] run, in that order, and are unlinked; the target stays installed.
    rec.exception_flags = EXCEPTION_NONCONTINUABLE;
    g_visited = 0;
    rv = call_unwind_asm(&f[2], (void *)0x8BAD0000u /* TargetIp: ignored on i386 */, &rec, 0xDEAD0000u, &asm_esp,
                         &asm_ebp);
    check(rv == 0xDEAD0002u, "handler EAX edits accumulate into the return value");
    check(g_visited == 2 && g_visit_order[0] == &f[0] && g_visit_order[1] == &f[1],
          "unwind order is newest to oldest, stopping at the target");
    check(teb()->seh == (uint32_t)(uintptr_t)&f[2], "the target frame is left installed");
    check((rec.exception_flags & (EXCEPTION_NONCONTINUABLE | EXCEPTION_UNWINDING)) ==
              (EXCEPTION_NONCONTINUABLE | EXCEPTION_UNWINDING),
          "the caller's record gained EXCEPTION_UNWINDING and kept its own flags");
    check(g_seen_flags == (EXCEPTION_NONCONTINUABLE | EXCEPTION_UNWINDING),
          "handlers see the caller's record in unwind mode");
    check(g_seen_record.exception_code == EXCEPTION_ACCESS_VIOLATION, "a caller-supplied record keeps its code");
    check(g_seen_eax[0] == 0xDEAD0000u && g_seen_eax[1] == 0xDEAD0001u, "each handler sees the running ReturnValue");
    check(g_seen_eip == unwind_asm_return_ip, "context->Eip is exactly the instruction after the call");
    check(g_seen_esp == asm_esp, "context->Esp is where the stdcall return will leave the caller");
    check(g_seen_ebp == asm_ebp, "context->Ebp is the caller's frame pointer at the call");

    // 7. Record == NULL synthesises STATUS_UNWIND; a NULL target unwinds everything, with
    //    EXCEPTION_EXIT_UNWIND set.
    push_frame(&f[1], (void *)handler_unwind, &f[2]);
    push_frame(&f[0], (void *)handler_unwind, &f[1]);
    g_visited = 0;
    rv = call_unwind_asm(&f[1], nullptr, nullptr, 7u, &asm_esp, &asm_ebp);
    check(rv == 8u && g_visited == 1 && g_visit_order[0] == &f[0], "unwind to f[1] runs only f[0]");
    check(g_seen_record.exception_code == STATUS_UNWIND, "a NULL record becomes STATUS_UNWIND");
    check(g_seen_eip == unwind_asm_return_ip, "context->Eip is exactly the instruction after the call");
    check(g_seen_record.exception_address == (void *)(uintptr_t)unwind_asm_return_ip,
          "the synthesised record's ExceptionAddress is the call's return address");
    check(g_seen_flags == EXCEPTION_UNWINDING, "the synthesised record carries EXCEPTION_UNWINDING");

    g_visited = 0;
    rv = call_unwind_asm(nullptr, nullptr, nullptr, 0u, &asm_esp, &asm_ebp);
    check(rv == 3u, "an exit unwind runs the remaining frames and returns the accumulated EAX");
    check(g_visited == 3 && g_visit_order[0] == &f[1] && g_visit_order[1] == &f[2] && g_visit_order[2] == &f[3],
          "exit unwind order and range");
    check((g_seen_flags & EXCEPTION_EXIT_UNWIND) != 0, "a NULL target sets EXCEPTION_EXIT_UNWIND");
    check(teb()->seh == 0, "an exit unwind empties the chain");

    // 8. Collided unwind: the protector answers ExceptionCollidedUnwind and names its own record; the
    //    walk unlinks that frame and resumes at its prev, as NT's loop does.
    ExceptionRegistration p[4];
    push_frame(&p[3], (void *)handler_unwind, nullptr);
    push_frame(&p[2], (void *)handler_unwind, &p[3]);
    push_frame(&p[1], (void *)handler_unwind, &p[2]);
    push_frame(&p[0], (void *)handler_collide, &p[1]);
    g_visited = 0;
    rv = call_unwind_asm(&p[3], nullptr, nullptr, 0xDEAD0000u, &asm_esp, &asm_ebp);
    check(g_visited == 3 && g_visit_order[0] == &p[0] && g_visit_order[1] == &p[1] && g_visit_order[2] == &p[2],
          "collided unwind resumes at the protector's prev");
    check(rv == 0xDEAD0002u, "frames after the collision still accumulate EAX");
    check(teb()->seh == (uint32_t)(uintptr_t)&p[3], "collided unwind stops at the target");

    // 9. Struct sizes must match what MSVC-compiled game code expects.
    check(sizeof(ExceptionRecord) == 80, "EXCEPTION_RECORD is 80 bytes, as MSVC compiled it");
    check(sizeof(ExceptionRegistration) == 8, "registration record is {handler, prev}");
}

// ---------------------------------------------------------------------------
// fs -> gs patcher
//
// The fixtures below are the byte sequences GNU as actually emitted for the instructions named
// (checked with `objdump -d -M intel` on a compiled snippet), not hand-typed guesses. A patcher that
// only ever saw hand-typed bytes would happily agree with itself about the wrong encoding.
// ---------------------------------------------------------------------------

struct Fixture {
    const char *asm_text;
    std::vector<uint8_t> bytes;
    uint32_t xbox_disp;   // 0 if there is no displacement to remap
    bool expect_patched;
};

// `mov eax, dword ptr fs:[0x20]` and friends, as GNU as encoded them.
static std::vector<uint8_t> b(std::initializer_list<int> v) { return std::vector<uint8_t>(v.begin(), v.end()); }

static const Fixture kFixtures[] = {
    {"mov eax, fs:[0x20]",              b({0x64, 0xA1, 0x20, 0, 0, 0}),                0x20, true},
    {"mov eax, fs:[0x04]",              b({0x64, 0xA1, 0x04, 0, 0, 0}),                0x04, true},
    {"mov fs:[0], esp",                 b({0x64, 0x89, 0x25, 0, 0, 0, 0}),                0x00, true},
    {"movzx eax, byte fs:[0x24]",       b({0x64, 0x0F, 0xB6, 0x05, 0x24, 0, 0, 0}),        0x24, true},
    {"mov ecx, fs:[0x28]",              b({0x64, 0x8B, 0x0D, 0x28, 0, 0, 0}),             0x28, true},
    {"mov eax, fs:[0x58]",              b({0x64, 0xA1, 0x58, 0, 0, 0}),                0x58, true},
    {"pop dword fs:[0x28]",             b({0x64, 0x8F, 0x05, 0x28, 0, 0, 0}),             0x28, true},
    {"mov fs:[0], eax",                 b({0x64, 0xA3, 0x00, 0, 0, 0}),                0x00, true},
    {"mov eax, fs:[0x34] (unmapped)",   b({0x64, 0xA1, 0x34, 0, 0, 0}),                0x34, false},
    {"pop dword fs:[eax] (no disp)",    b({0x64, 0x8F, 0x00}),                          0,    false},
};

static void test_fs_patch()
{
    printf("\nfs -> gs patcher\n");
    uint32_t k = teb_k();

    // The scanner must first agree that each *remappable* fixture is a form it rewrites. The
    // register-addressed one (`pop fs:[eax]`) deliberately has no displacement and must not match.
    bool recognises = true, ignores_reg_form = false;
    for (const Fixture &f : kFixtures) {
        uint32_t off = 0, disp = 0;
        bool ok = fs_patch_recognises(f.bytes.data(), (uint32_t)f.bytes.size(), &off, &disp);
        if (!f.expect_patched) {
            if (f.xbox_disp == 0 && !ok) ignores_reg_form = true;
            continue;
        }
        if (!ok || disp != f.xbox_disp) {
            recognises = false;
            printf("       not recognised: %-28s (ok=%d disp=0x%x want 0x%x)\n", f.asm_text, ok, disp, f.xbox_disp);
        }
    }
    check(recognises, "every remappable fixture is recognised, with the right Xbox displacement");
    check(ignores_reg_form, "a register-addressed fs access is correctly not a remap candidate");

    // Patch each fixture on its own and compare byte for byte.
    int wrong_prefix = 0, wrong_disp = 0, wrongly_patched = 0;
    for (const Fixture &f : kFixtures) {
        std::vector<uint8_t> copy = f.bytes;
        uint32_t off = 0;
        fs_patch_recognises(copy.data(), (uint32_t)copy.size(), &off, nullptr);
        FsPatchStats st = patch_fs_to_gs(copy.data(), (uint32_t)copy.size());

        if (!f.expect_patched) {
            if (st.patched != 0) { wrongly_patched++; printf("       wrongly patched: %s\n", f.asm_text); }
            if (memcmp(copy.data(), f.bytes.data(), f.bytes.size()) != 0) {
                wrongly_patched++;
                printf("       bytes changed:  %s\n", f.asm_text);
            }
            continue;
        }
        if (st.patched != 1) { wrongly_patched++; printf("       not patched:   %s (st.patched=%u)\n", f.asm_text, st.patched); continue; }
        if (copy[0] != 0x65) { wrong_prefix++; printf("       bad prefix:    %s\n", f.asm_text); }

        uint32_t got;
        memcpy(&got, copy.data() + off, 4);
        uint32_t want = teb_patch_offset(f.xbox_disp);
        if (got != want) { wrong_disp++; printf("       bad disp:      %s got 0x%08x want 0x%08x\n", f.asm_text, got, want); }

        // Instruction length must be unchanged: nothing downstream may have to relocate.
        if (copy.size() != f.bytes.size()) wrong_disp++;
    }
    check(wrongly_patched == 0, "unmapped / register-addressed forms are left byte-for-byte alone");
    check(wrong_prefix == 0, "the 0x64 fs prefix became 0x65 in every rewritten instruction");
    check(wrong_disp == 0, "every displacement became K + the field offset, length unchanged");

    // Doubly-prefixed instruction: `mov eax, fs:[fs:[0x20]]`.
    {
        std::vector<uint8_t> v = b({0x64, 0x64, 0x8B, 0x05, 0x20, 0, 0, 0});
        FsPatchStats st = patch_fs_to_gs(v.data(), (uint32_t)v.size());
        uint32_t got;
        memcpy(&got, v.data() + 4, 4);
        check(st.patched == 1 && v[0] == 0x65 && v[1] == 0x65 && got == teb_patch_offset(0x20),
              "a doubly-prefixed instruction is patched once, with both prefixes normalised");
    }

    // fs:[0] is remapped here even though Windows deliberately skipped it.
    check(teb_is_remapped(0x00), "fs:[0x00] (SEH head) is remapped on Linux");

    // The critical safety property: the patched displacement must resolve, at run time, onto this
    // thread's own TEB field.
    bool lands = true;
    for (uint32_t d : {0x00u, 0x04u, 0x20u, 0x24u, 0x28u, 0x58u}) {
        uint32_t p = teb_patch_offset(d);
        if (teb_thread_pointer() + p != (uint32_t)(uintptr_t)teb() + d) lands = false;
    }
    check(lands, "each patched displacement resolves onto this thread's field (%gs:0 + disp)");
    printf("       K = 0x%08x, e.g. fs:[0x20] -> gs:[0x%08x]\n", k, teb_patch_offset(0x20));
}

int main()
{
    printf("BLACK loader -- Linux platform layer tests\n");
    printf("kernel %s\n", "i386");

    test_teb();
    test_fs_patch();
    test_seh();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}