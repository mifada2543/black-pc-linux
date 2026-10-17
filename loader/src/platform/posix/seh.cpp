#include "platform/posix/seh.h"

#include "platform/posix/teb.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

// CONTEXT flags. FULL is what the signal bridge fills in; the unwind context (seh_unwind) claims
// control and integer only, the parts of the caller's state it reconstructs. Nothing here reads the
// debug or FP extensions, and claiming them would be a lie the game could act on.
static constexpr uint32_t CONTEXT_FULL = 0x00010007;
static constexpr uint32_t CONTEXT_CONTROL = 0x00010001; // Ebp, Eip, SegCs, EFlags, Esp, SegSs
static constexpr uint32_t CONTEXT_INTEGER = 0x00010002; // Edi, Esi, Ebx, Edx, Ecx, Eax

// ---------------------------------------------------------------------------
// chain head
// ---------------------------------------------------------------------------

uint32_t seh_head() { return teb()->seh; }

static void seh_set_head(ExceptionRegistration *r) { teb()->seh = (uint32_t)(uintptr_t)r; }

static ExceptionRegistration *seh_top() { return (ExceptionRegistration *)(uintptr_t)teb()->seh; }

// ---------------------------------------------------------------------------
// dispatch and unwind
// ---------------------------------------------------------------------------

namespace {

// Set while loader plumbing (the walks below, the signal bridge's build-up) is running: a fault there
// means the loader itself is broken, and re-entering the dispatcher from it would be nonsense. It is
// cleared around every call INTO game code -- handlers during dispatch and unwind -- so that a
// handler which faults gets the ordinary nested dispatch NT would give it, and so that a catch
// (which abandons these frames on its way out; see seh.h) cannot leave the flag stuck up for the
// next, unrelated fault.
volatile sig_atomic_t g_in_bridge;

// Call a frame handler and return its disposition, handing it NT's fourth argument: a dispatcher
// context whose first and only meaningful member on i386 is the registration pointer that a handler
// may store into (the CRT's __unwind_handler does, during a collided unwind). The callee may not
// return at all -- a catch runs the __except block outside the dispatcher's frames -- in which case
// g_in_bridge is left clear, which is exactly what the next, unrelated fault must see.
long call_handler(SehHandler handler, ExceptionRecord *record, ExceptionRegistration *frame, Context *context,
                  ExceptionRegistration **dispatch_out)
{
    ExceptionRegistration *dispatch = nullptr;
    g_in_bridge = 0;
    long rc = handler(record, frame, context, &dispatch);
    g_in_bridge = 1;
    *dispatch_out = dispatch;
    return rc;
}

// The invalid-target / invalid-disposition paths of the unwind. NT raises a fresh non-continuable
// exception (STATUS_INVALID_UNWIND_TARGET / STATUS_INVALID_DISPOSITION) and lets the search continue
// from there; that machinery is not built yet, so these report and end the process instead of
// guessing (see the notes at the top of seh.h).
[[noreturn]] void unwind_fail(const char *why, ExceptionRecord *record, const Context *context)
{
    fprintf(stderr, "seh: RtlUnwind: %s\n", why);
    seh_fatal(record, context);
    _exit(3);
}

} // namespace

// Walk the chain. `budget` bounds the walk so a corrupted prev pointer cannot make the loader spin
// through unmapped memory forever; 512 frames is far beyond any plausible nesting.
long seh_dispatch(ExceptionRecord *record, Context *context, SehDispatch *out)
{
    g_in_bridge = 1;

    out->record = record;
    out->context = context;
    out->code = record->exception_code;
    out->resume_context = nullptr;
    out->unhandled = false;

    // Walk with a cursor rather than re-reading the head: a handler that answers continue-search does
    // not unlink itself (its epilogue still has to pop it later -- and during a catch it never runs),
    // so re-reading the head would spin on the same frame forever.
    ExceptionRegistration *frame = seh_top();

    for (int budget = 0; budget < 512; budget++) {
        if (!frame || (uintptr_t)frame == EXCEPTION_CHAIN_END) {
            out->unhandled = true;
            return ExceptionContinueSearch;
        }
        auto handler = (SehHandler)frame->handler;
        if (!handler) {
            fprintf(stderr, "seh: frame %p has a NULL handler\n", (void *)frame);
            out->unhandled = true;
            return ExceptionContinueSearch;
        }

        ExceptionRegistration *dispatch = nullptr;
        long rc = call_handler(handler, record, frame, context, &dispatch);

        if (rc == ExceptionContinueExecution) {
            if (record->exception_flags & EXCEPTION_NONCONTINUABLE) {
                // NT hands STATUS_NONCONTINUABLE_EXCEPTION back to the kernel, which raises it as a
                // fresh exception; reported instead, see the notes at the top of seh.h.
                fprintf(stderr, "seh: handler at %p asked to resume a non-continuable exception 0x%08x\n",
                        frame->handler, record->exception_code);
                out->unhandled = true;
                return ExceptionContinueSearch;
            }
            // The handler edited *context in place; the caller resumes from it.
            out->resume_context = context;
            return ExceptionContinueExecution;
        }
        if (rc != ExceptionContinueSearch) {
            // ExceptionNestedException, ExceptionCollidedUnwind (legal only during an unwind) or
            // garbage. NT raises STATUS_INVALID_DISPOSITION; reported instead, see seh.h.
            fprintf(stderr, "seh: handler at %p returned disposition %ld, expected 0 or 1\n", frame->handler, rc);
            out->unhandled = true;
            return ExceptionContinueSearch;
        }

        frame = frame->prev; // continue past this frame, which stays installed
    }

    fprintf(stderr, "seh: chain longer than 512 frames, giving up\n");
    out->unhandled = true;
    return ExceptionContinueSearch;
}

bool seh_raise(ExceptionRecord *record, Context *context)
{
    g_in_bridge = 1;

    SehDispatch d;
    long rc = seh_dispatch(record, context, &d);

    g_in_bridge = 0; // about to return to game code
    if (rc == ExceptionContinueExecution && d.resume_context) return true;

    if (d.unhandled) seh_fatal(record, context);
    return false;
}

extern "C" __attribute__((stdcall, noinline)) uint32_t seh_unwind(void *target_frame, void *target_ip,
                                                                   ExceptionRecord *record, uint32_t return_value)
{
    (void)target_ip; // ignored on i386; see the contract in seh.h

    // The caller-frame arithmetic promised in seh.h. __builtin_frame_address forces the frame
    // pointer, which makes [ebp] the caller's EBP and [ebp+4] this call's return address; the
    // caller's ESP once this stdcall has returned is ebp + 4 (saved EBP) + 4 (return address) +
    // 16 (four arguments). NT computes the same +24 from RtlUnwind's frame.
    const uintptr_t ebp = (uintptr_t)(const void *)__builtin_frame_address(0);

    Context context;
    memset(&context, 0, sizeof context);
    context.context_flags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    context.eax = return_value; // the running value handlers may edit
    context.ebp = *(const uint32_t *)ebp;
    context.eip = (uint32_t)(uintptr_t)__builtin_return_address(0);
    context.esp = (uint32_t)(ebp + 4 + 4 + 16);
    {
        uint32_t eflags;
        __asm__ __volatile__("pushfl; popl %0" : "=r"(eflags));
        context.eflags = eflags;
    }

    ExceptionRecord local;
    if (!record) {
        memset(&local, 0, sizeof local);
        local.exception_code = STATUS_UNWIND;
        local.exception_address = (void *)(uintptr_t)context.eip;
        record = &local;
    }
    record->exception_flags |= EXCEPTION_UNWINDING;
    if (!target_frame) record->exception_flags |= EXCEPTION_EXIT_UNWIND;

    g_in_bridge = 1;

    ExceptionRegistration *const target = (ExceptionRegistration *)target_frame;
    // A target that names a frame -- rather than NULL or NT's 0xFFFFFFFF "unwind everything"
    // sentinel -- has to be found in the chain.
    const bool target_is_frame = target_frame != nullptr && (uintptr_t)target_frame != EXCEPTION_CHAIN_END;

    ExceptionRegistration *cursor = seh_top();

    for (int budget = 0; budget < 512; budget++) {
        if (cursor == target) {
            // Target reached (for an unwind-everything form, the sentinel reached), and it stays
            // installed: everything newer than it has been handled and unlinked.
            g_in_bridge = 0;
            return context.eax;
        }
        if (!cursor || (uintptr_t)cursor == EXCEPTION_CHAIN_END) {
            if (target_is_frame) {
                char why[96];
                snprintf(why, sizeof why, "target frame %p is not in the chain", target_frame);
                unwind_fail(why, record, &context);
            }
            // NULL or 0xFFFFFFFF target: every frame has been unwound. NT returns here for the
            // sentinel and re-raises for NULL; see the header note on that one deviation.
            g_in_bridge = 0;
            return context.eax;
        }
        if ((uintptr_t)target_frame && cursor > target) {
            // Frames grow outwards in address, so this means the walk went past where the target
            // would have been: it was never in the chain. NT raises STATUS_INVALID_UNWIND_TARGET.
            char why[96];
            snprintf(why, sizeof why, "target frame %p was passed (now at %p)", target_frame, (const void *)cursor);
            unwind_fail(why, record, &context);
        }

        auto handler = (SehHandler)cursor->handler;
        if (!handler) {
            char why[96];
            snprintf(why, sizeof why, "frame %p has a NULL handler", (const void *)cursor);
            unwind_fail(why, record, &context);
        }

        ExceptionRegistration *dispatch = nullptr;
        long rc = call_handler(handler, record, cursor, &context, &dispatch);

        if (rc == ExceptionCollidedUnwind) {
            // The walk collided with a local unwind that is already running (the CRT's
            // __unwind_handler protector). The handler stored the frame to continue from in the
            // dispatcher slot; NT unlinks that frame without calling it and steps to its prev,
            // because its remaining __finally blocks are run by the local unwind that owns it.
            cursor = dispatch;
            if (!cursor || (uintptr_t)cursor == EXCEPTION_CHAIN_END)
                unwind_fail("collided-unwind handler named no frame to resume from", record, &context);
        } else if (rc != ExceptionContinueSearch) {
            char why[96];
            snprintf(why, sizeof why, "handler at %p returned disposition %ld, expected 1 or 3", cursor->handler, rc);
            unwind_fail(why, record, &context);
        }

        // Unlink the frame that was just handled, exactly as NT's RtlpUnlinkHandler does: the head
        // becomes its prev, whatever any handler did to the head in the meantime.
        seh_set_head(cursor->prev);
        cursor = cursor->prev;
    }

    unwind_fail("chain longer than 512 frames", record, &context);
}

// ---------------------------------------------------------------------------
// signal bridge
// ---------------------------------------------------------------------------
//
// Linux delivers a fault as a signal, not as an EXCEPTION_RECORD. This turns the three faults the
// game can take back into the Windows shape and pushes them through the chain.

namespace {

void fill_context(Context *c, const ucontext_t *uc)
{
    memset(c, 0, sizeof *c);
    c->context_flags = CONTEXT_FULL;
    c->edi = (uint32_t)uc->uc_mcontext.gregs[REG_EDI];
    c->esi = (uint32_t)uc->uc_mcontext.gregs[REG_ESI];
    c->ebx = (uint32_t)uc->uc_mcontext.gregs[REG_EBX];
    c->edx = (uint32_t)uc->uc_mcontext.gregs[REG_EDX];
    c->ecx = (uint32_t)uc->uc_mcontext.gregs[REG_ECX];
    c->eax = (uint32_t)uc->uc_mcontext.gregs[REG_EAX];
    c->ebp = (uint32_t)uc->uc_mcontext.gregs[REG_EBP];
    c->eip = (uint32_t)uc->uc_mcontext.gregs[REG_EIP];
    c->esp = (uint32_t)uc->uc_mcontext.gregs[REG_ESP];
    c->eflags = (uint32_t)uc->uc_mcontext.gregs[REG_EFL];
    c->seg_cs = (uint32_t)uc->uc_mcontext.gregs[REG_CS];
    c->seg_ds = (uint32_t)uc->uc_mcontext.gregs[REG_DS];
    c->seg_fs = (uint32_t)uc->uc_mcontext.gregs[REG_FS];
    c->seg_gs = (uint32_t)uc->uc_mcontext.gregs[REG_GS];
}

void fill_record(ExceptionRecord *r, uint32_t code, const Context *c, siginfo_t *si)
{
    memset(r, 0, sizeof *r);
    r->exception_code = code;
    r->exception_address = (void *)(uintptr_t)c->eip;
    if (si) {
        r->number_parameters = 2;
        r->exception_information[0] = si->si_addr ? (uint64_t)(uintptr_t)si->si_addr : 0;
        // 0 = read, 1 = write, 2 = execute. si_code carries SEGV_ACCERR/SEGV_MAPERR but not the
        // direction, so read is the safe assumption for an unqualified access violation.
        r->exception_information[1] = 0;
    }
}

void fault_handler(int sig, siginfo_t *si, void *vctx)
{
    auto *uc = (ucontext_t *)vctx;
    Context c;
    ExceptionRecord r;
    uint32_t code = EXCEPTION_ACCESS_VIOLATION;

    if (sig == SIGILL) code = EXCEPTION_ILLEGAL_INSTRUCTION;
    else if (sig == SIGFPE) code = EXCEPTION_INT_DIVIDE_BY_ZERO;

    if (g_in_bridge) {
        // A fault in the loader's own plumbing. The walks are not meant to be re-entered half-built,
        // and a fault in fill_* just below would recurse until the stack died, so this is reported.
        fprintf(stderr, "seh: fault inside the loader's dispatcher (signal %d)\n", sig);
        _exit(3);
    }
    g_in_bridge = 1;

    fill_context(&c, uc);
    fill_record(&r, code, &c, si);

    SehDispatch d;
    long rc = seh_dispatch(&r, &c, &d);

    if (rc == ExceptionContinueExecution && d.resume_context) {
        // Copy the handler's edits back into the machine context and resume. The CONTEXT is the whole
        // resume state, as it is on NT (which continues via NtContinue); record->exception_address is
        // informational. Never returns.
        uc->uc_mcontext.gregs[REG_EAX] = c.eax;
        uc->uc_mcontext.gregs[REG_ECX] = c.ecx;
        uc->uc_mcontext.gregs[REG_EDX] = c.edx;
        uc->uc_mcontext.gregs[REG_EBX] = c.ebx;
        uc->uc_mcontext.gregs[REG_ESP] = c.esp;
        uc->uc_mcontext.gregs[REG_EBP] = c.ebp;
        uc->uc_mcontext.gregs[REG_ESI] = c.esi;
        uc->uc_mcontext.gregs[REG_EDI] = c.edi;
        uc->uc_mcontext.gregs[REG_EIP] = c.eip;
        g_in_bridge = 0; // game code is about to run again
        setcontext(uc);
    }

    seh_fatal(&r, &c);
    // seh_fatal does not return, but keep the compiler honest.
    _exit(3);
}

} // namespace

void seh_install_signal_bridge()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER; // SA_NODEFER: a handler may fault again legitimately
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    // SIGTRAP would be the int3 path (--probe). Left on the default action for now: the game code
    // that raises it is patched by hooks.cpp on Windows and is not wired up yet here.
}

void seh_fatal(const ExceptionRecord *record, const Context *context)
{
    // Mirrors what hooks.cpp reports on Windows, minus the dbghelp symbolisation (perf.cpp's job) and
    // the Windows-only address notes for the NV2A/MCPX/ACI ranges.
    fprintf(stderr,
            "unhandled exception 0x%08x at 0x%08x\n"
            "  eax=%08x ebx=%08x ecx=%08x edx=%08x\n"
            "  esi=%08x edi=%08x ebp=%08x esp=%08x eflags=%08x\n",
            record->exception_code, context->eip, context->eax, context->ebx, context->ecx, context->edx,
            context->esi, context->edi, context->ebp, context->esp, context->eflags);
    if (record->number_parameters >= 2)
        fprintf(stderr, "  fault address %08x, %s\n", record->exception_information[0],
                record->exception_information[1] == 1 ? "write" : "read");
}