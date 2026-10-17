// Threads: what the game can see after it creates one.
//
// The checks are about *observable* state, not about the loader's bookkeeping. A worker thread reads the
// same displacements the game's code reads -- fs:[0x20], fs:[0x24], fs:[0x28], fs:[0xE10 + 4*slot] -- and has
// to find its own values there, not another thread's. That is the property the fs->gs rewrite exists to
// provide, and a thread test that only checked return codes would not touch it.
#include "kernel/k_sync_posix.h" // the IRQL wrappers, which publish into the block
#include "kernel/k_thread_posix.h"

#include "platform/posix/teb.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <string>

static int g_failures, g_checks;

static void check(bool cond, const std::string &what)
{
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s\n", what.c_str());
        g_failures++;
    }
    fflush(stdout);
}

static void check_u32(uint32_t got, uint32_t want, const std::string &what)
{
    g_checks++;
    if (got == want) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s (got 0x%08x, want 0x%08x)\n", what.c_str(), got, want);
        g_failures++;
    }
    fflush(stdout);
}

// ---- what the worker thread finds about itself ------------------------------------------------------------

static uint32_t g_worker_ran;
static uint32_t g_worker_kthread;
static uint32_t g_worker_kprcb;
static uint32_t g_worker_stack_base;
static uint32_t g_worker_slot5;
static uint32_t g_worker_irql_seen;
static uint32_t g_worker_context_seen;
static uint32_t g_worker_status;
static int g_worker_failures;

// Runs as an Xbox start routine: stdcall, one argument.
static void __attribute__((stdcall)) worker(void *context)
{
    g_worker_context_seen = (uint32_t)(uintptr_t)context;
    XboxTeb *b = teb();

    // The KPCR fields, at the displacements the patched instructions read.
    g_worker_kprcb = b->prcb;
    g_worker_kthread = b->thread;
    g_worker_stack_base = b->stack_base;


    // A TLS slot of this thread's own.
    b->tls_slots[5] = 0xFEEDFACE;
    g_worker_slot5 = b->tls_slots[5];

    // Mark the TLS data block so the main thread can look for the pattern.
    memset((void *)(uintptr_t)b->stack_limit, 0xA5, 64);

    // Raising the IRQL has to show up where the game reads it.
    const uint8_t old = KeRaiseIrqlToDpcLevel();
    g_worker_irql_seen = teb()->irql;
    KfLowerIrql(old);
    if (teb()->irql != IRQL_PASSIVE) g_worker_failures++;

    if (!teb_k_matches_here()) g_worker_failures++;
    g_worker_ran = 1;

    // Terminating from inside the thread is the Xbox's way to end one, and it must not return.
    PsTerminateSystemThread((NTSTATUS)0x1234);
    g_worker_failures += 1000; // reached only if PsTerminateSystemThread returned
}

// A second entry shape: cdecl, no arguments, which is how the XBE entry point is called.
static void cdecl_worker() { g_worker_ran = 2; }

// The XPKSYSTEM_ROUTINE shape: called with the start routine and its context.
static uint32_t g_wrapper_ran;
static void __attribute__((stdcall)) system_wrapper(void *routine, void *context)
{
    g_wrapper_ran = 1;
    ((void(__attribute__((stdcall)) *)(void *))(uintptr_t)routine)(context);
}

// ----------------------------------------------------------------------------------------------------------

static void test_the_main_thread()
{
    printf("\nthe loader's own thread\n");

    XboxThread *t = kthread_current();
    check(t != nullptr, "the calling thread has a record");
    if (!t) return;

    XboxTeb *b = teb();
    check_u32(b->prcb, (uint32_t)(uintptr_t)&t->prcb, "fs:[0x20] points at this thread's own KPRCB");
    check_u32(b->thread, (uint32_t)(uintptr_t)t->ethread, "fs:[0x28] is this thread's KTHREAD");
    check_u32(b->irql, xk_current_irql(), "fs:[0x24] agrees with the IRQL the sync layer tracks");
    check_u32(b->irql, t->prcb.irql, "and the KPRCB's copy of it agrees too");
    check_u32(t->prcb.current_thread, b->thread, "so does the KPRCB's CurrentThread");
    check(b->stack_limit != 0 && b->stack_base > b->stack_limit, "StackLimit and StackBase bracket the TLS data");
    check_u32(b->stack_base, b->stack_limit + t->tls_size, "with the slot array starting where the Xbox puts it");
    check(xhandle_is(t->handle, XHANDLE_THREAD), "the adopted thread's handle says it is a thread");
    uint32_t tls_data_in_kthread;
    memcpy(&tls_data_in_kthread, t->ethread + 0x28, 4);
    check_u32(tls_data_in_kthread, (uint32_t)(uintptr_t)t->tls_data, "KTHREAD.TlsData points at the TLS data");
}

// ----------------------------------------------------------------------------------------------------------

static void test_created_thread()
{
    printf("\na thread the game creates\n");

    XboxThread *main_t = kthread_current();
    uint32_t main_slot5 = teb()->tls_slots[5];
    teb()->tls_slots[5] = 0x12345678;

    int marker = 0;
    uint32_t handle = 0, tid = 0;
    g_worker_failures = 0;
    g_worker_ran = 0;

    check(PsCreateSystemThreadEx(&handle, 0, 0x40000, 0x2000, &tid, (void *)worker, &marker, 0, 0, nullptr) ==
              KT_SUCCESS,
          "PsCreateSystemThreadEx succeeds");
    check_u32(handle & 0xF0000000u, 0x40000000u, "the handle is tagged as a thread handle");
    check(tid != 0, "and a thread id comes back");
    check_u32(kthread_count(), 2, "the loader knows about two threads now");

    XboxThread *w = kthread_from_handle(handle);
    check(w != nullptr, "the handle resolves to its record");
    if (!w) return;
    check_u32(kthread_from_kthread(kthread_kthread(w)) == w ? 1u : 0u, 1u,
              "and the record's KTHREAD resolves back to it");
    check(w != main_t, "which is not the calling thread's record");

    // The worker's own view of itself.
    for (int i = 0; i < 400 && g_worker_ran == 0; i++) usleep(5 * 1000);
    check_u32(g_worker_ran, 1, "the worker ran");
    check_u32(g_worker_failures, 0, "and found everything it looks for in order");
    check_u32(g_worker_context_seen, (uint32_t)(uintptr_t)&marker, "it was called with its context");
    check_u32(g_worker_kprcb, (uint32_t)(uintptr_t)&w->prcb, "fs:[0x20] in the worker is the worker's KPRCB");
    check_u32(g_worker_kthread, (uint32_t)(uintptr_t)w->ethread, "fs:[0x28] is the worker's KTHREAD");
    check_u32(g_worker_stack_base, (uint32_t)(uintptr_t)(w->tls_data + w->tls_size),
              "its StackBase is its own TLS array");
    check_u32(w->tls_size, 0x2000, "and it got the amount of TLS data it asked for");
    check_u32(g_worker_irql_seen, IRQL_DISPATCH, "raising the IRQL showed up at fs:[0x24]");
    check_u32(g_worker_slot5, 0xFEEDFACE, "it could write its own TLS slot");

    // Isolation: two threads' blocks, TLS data and slots must not touch.
    check_u32(teb()->tls_slots[5], 0x12345678, "this thread's TLS slot is unchanged by the worker");
    check(memcmp((const void *)(uintptr_t)teb()->stack_limit, "\xA5\xA5\xA5\xA5", 4) != 0,
          "and the worker's TLS data did not land in this thread's");
    uint32_t worker_tls_in_kthread;
    memcpy(&worker_tls_in_kthread, w->ethread + 0x28, 4);
    check_u32(worker_tls_in_kthread, (uint32_t)(uintptr_t)w->tls_data, "the worker's KTHREAD.TlsData is its own");

    // It terminated itself, so it is no longer running.
    for (int i = 0; i < 200 && !w->finished; i++) usleep(5 * 1000);
    check(w->finished, "the worker finished when it called PsTerminateSystemThread");
    check_u32((uint32_t)w->exit_status, 0x1234, "with the status it was given");
    check_u32(kthread_live_count(), 1, "and the live count dropped back");
}

// ----------------------------------------------------------------------------------------------------------

static volatile int g_suspended_ran;

static void __attribute__((stdcall)) suspended_worker(void *)
{
    g_suspended_ran = 1;
}

static void test_suspend_resume()
{
    printf("\nsuspend and resume\n");

    g_suspended_ran = 0;
    uint32_t handle = 0, tid = 0;
    check(PsCreateSystemThreadEx(&handle, 0, 0x40000, 0, &tid, (void *)suspended_worker, nullptr,
                                 1 /* suspended */, 0, nullptr) == KT_SUCCESS,
          "a thread created suspended");
    usleep(80 * 1000);
    check_u32((uint32_t)g_suspended_ran, 0, "has not run");

    uint32_t previous = 0xFFFFFFFF;
    check(NtResumeThread(handle, &previous) == KT_SUCCESS, "NtResumeThread succeeds");
    check_u32(previous, 1, "reporting a previous suspend count of one");
    for (int i = 0; i < 400 && !g_suspended_ran; i++) usleep(5 * 1000);
    check_u32((uint32_t)g_suspended_ran, 1, "and the thread runs");

    // Suspend a thread that has already run: nothing observable happens to a finished thread, but the count
    // still behaves.
    check(NtSuspendThread(handle, &previous) == KT_SUCCESS, "NtSuspendThread succeeds");
    check_u32(previous, 0, "reporting a previous count of zero");
    check(NtResumeThread(handle, &previous) == KT_SUCCESS, "and NtResumeThread balances it");
    check_u32(previous, 1, "reporting the count it brought down");

    check(NtSuspendThread(0xFFFFFFFF, &previous) == KT_INVALID_HANDLE, "suspend with no handle is refused");
    check(NtResumeThread(0, &previous) == KT_INVALID_HANDLE, "and so is resume");
}

// ----------------------------------------------------------------------------------------------------------

static void test_handles_and_priorities()
{
    printf("\nhandles, objects and priorities\n");

    uint32_t handle = 0, tid = 0;
    PsCreateSystemThreadEx(&handle, 0, 0x40000, 0, &tid, (void *)suspended_worker, nullptr, 1, 0, nullptr);
    XboxThread *w = kthread_from_handle(handle);
    check(w != nullptr, "a thread handle resolves");

    // ObReferenceObjectByHandle is how the game turns a thread handle into a KTHREAD pointer, which it then
    // passes to KeQueryBasePriorityThread and friends.
    void *object = nullptr;
    check(ObReferenceObjectByHandle(handle, nullptr, &object) == KT_SUCCESS, "ObReferenceObjectByHandle succeeds");
    check(object == kthread_kthread(w), "and returns that thread's KTHREAD");
    check_u32((uint32_t)(uintptr_t)kthread_kthread(w), (uint32_t)(uintptr_t)w->ethread, "which is its ETHREAD");

    check(KeQueryBasePriorityThread(object) == 0, "a new thread's base priority is zero");
    check(KeSetBasePriorityThread(object, 5) == 0, "raising it reports the old value");
    check_u32((uint32_t)KeQueryBasePriorityThread(object), 5, "and the new value reads back");
    check(KeSetBasePriorityThread(object, -3) == 5, "lowering it reports the old value too");
    check_u32((uint32_t)KeQueryBasePriorityThread(object), (uint32_t)-3, "and the change took");

    uint8_t not_a_thread[8] = {};
    check_u32((uint32_t)KeQueryBasePriorityThread(not_a_thread), 0,
              "an unknown thread object reports priority zero");
    check(KeSetBasePriorityThread(not_a_thread, 1) == 0, "and cannot be set");

    void *object2 = (void *)0x1234;
    check(ObReferenceObjectByHandle(0xFFFFFFFF, nullptr, &object2) == KT_OBJECT_TYPE_MISMATCH,
          "an invalid handle is not a thread");
    check(object2 == nullptr, "and leaves the output null");
    ObfDereferenceObject(object); // nothing to release, and nothing that must not crash

    // The handle survives its thread being closed -- the Xbox does not kill a thread when its handle goes.
    check(NtClose(handle) == KT_SUCCESS, "closing a thread handle succeeds");
    check(NtClose(0x4000FFFF) == KT_INVALID_HANDLE, "closing a thread handle that does not exist is refused");

    // Stop the suspended thread so the suite does not leave one behind.
    uint32_t previous = 0;
    while (NtResumeThread(handle, &previous) == KT_SUCCESS && previous > 0) {
    }
    usleep(50 * 1000);
}

// ----------------------------------------------------------------------------------------------------------

static void test_entry_shapes()
{
    printf("\nthe other entry shapes\n");

    g_worker_ran = 0;
    uint32_t handle = 0, tid = 0;

    // cdecl with no arguments: the XBE entry point's shape, which the boot thread uses.
    check(PsCreateSystemThreadEx(&handle, 0, 0x40000, 0, &tid, (void *)cdecl_worker, nullptr, 0, 0, nullptr) ==
              KT_SUCCESS,
          "a thread whose routine takes no arguments");
    for (int i = 0; i < 200 && !g_worker_ran; i++) usleep(5 * 1000);
    check_u32(g_worker_ran, 2, "runs it");

    // A system routine wrapping the start routine, the XPKSYSTEM_ROUTINE shape.
    g_worker_ran = 0;
    g_wrapper_ran = 0;
    check(PsCreateSystemThreadEx(&handle, 0, 0x40000, 0, &tid, (void *)worker, &tid, 0, 0,
                                 (void *)system_wrapper) == KT_SUCCESS,
          "a thread with a system routine");
    for (int i = 0; i < 400 && !g_worker_ran; i++) usleep(5 * 1000);
    check_u32(g_worker_ran, 1, "which ran the start routine");
    check_u32(g_wrapper_ran, 1, "and the system routine ran first, as XPKSYSTEM_ROUTINE does");
}

int main()
{
    printf("BLACK loader -- kernel threads\n");

    test_the_main_thread();
    test_created_thread();
    test_suspend_resume();
    test_handles_and_priorities();
    test_entry_shapes();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}