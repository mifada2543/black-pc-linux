#include "kernel/k_thread_posix.h"

#include "kernel/k_sync_posix.h" // KfLowerIrql, for a thread that exits holding the dispatch mutex
#include "platform/posix/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace {

// ETHREAD fields the Xbox reads. The offsets are the console's, from the reference's notes: TlsData
// inside the KTHREAD at 0x28, UniqueThread at 0x12C, StartAddress at 0x130.
constexpr uint32_t KTHREAD_TLS_DATA = 0x28;
constexpr uint32_t ETHREAD_UNIQUE_THREAD = 0x12C;
constexpr uint32_t ETHREAD_START_ADDRESS = 0x130;

// The boot thread gets at least this much: Xbox stacks are small and the loader's own frames are deeper
// than the game's.
constexpr uint32_t MIN_HOST_STACK = 1u << 20;

std::mutex g_registry;
std::vector<XboxThread *> g_threads; // pointers, because XboxThread holds a mutex and never moves
std::atomic<uint32_t> g_unique_thread{1};
std::atomic<uint32_t> g_live{0};

// This thread's record. Set by thread_main when the loader starts the thread, and by kthread_current()
// for any thread the loader did not create.
__thread XboxThread *t_current;

// Fill this thread's block from its record: the KPRCB pointer, the three values the game reads at their
// own displacements, and the TLS data. Runs once per thread, before its entry routine.
void publish(XboxThread *t)
{
    XboxTeb *b = teb();
    b->prcb = (uint32_t)(uintptr_t)&t->prcb;
    b->irql = t->prcb.irql = xk_current_irql();
    b->thread = t->prcb.current_thread = (uint32_t)(uintptr_t)t->ethread;
    b->dpc_active = 0;
    t->prcb.dpc_routine_active = 0;
    b->stack_limit = (uint32_t)(uintptr_t)t->tls_data;
    // XAPI reads NtTib.StackBase as the TLS array, and the array follows the TLS data block -- which is
    // where the reference pointed its own slot (TlsData + TlsDataSize).
    b->stack_base = (uint32_t)(uintptr_t)(t->tls_data + t->tls_size);
    // The TLS data pointer itself belongs in the KTHREAD, where a thread looks for its own.
    memcpy(t->ethread + KTHREAD_TLS_DATA, &t->tls_data, sizeof t->tls_data);
}

void finish(XboxThread *t)
{
    pthread_mutex_lock(&t->gate);
    t->finished = true;
    t->running = false;
    pthread_cond_broadcast(&t->gate_cv);
    pthread_mutex_unlock(&t->gate);
    g_live--;
    logf("thread '%s' returned", t->name);
}

// Runs however the thread ends -- by returning from its entry routine or by calling PsTerminateSystemThread,
// which does not return at all. A cleanup handler is the only mechanism that covers both: without it, a
// thread that terminates itself would never mark itself finished, and the loader would wait for it forever.
void finish(XboxThread *t) __attribute__((unused));
void *thread_main(void *param)
{
    XboxThread *t = (XboxThread *)param;
    t_current = t;
    publish(t);
    pthread_cleanup_push((void (*)(void *))finish, t);

    // Where this thread's stack starts, which is what a crash inside the game points back into.
    uint32_t marker = 0;
    t->stack_top = (uint32_t)(uintptr_t)&marker;

    // A thread created suspended parks here until NtResumeThread, which is what the Xbox's suspend count
    // does. The record is published first either way, so a suspended thread already has a valid block.
    pthread_mutex_lock(&t->gate);
    while (t->suspend_count > 0) pthread_cond_wait(&t->gate_cv, &t->gate);
    t->running = true;
    pthread_mutex_unlock(&t->gate);

    if (t->system_routine) {
        // XPKSYSTEM_ROUTINE(start_routine, start_context): the XDK passes a wrapper that gets to run
        // before the thread's real entry point.
        ((void(__attribute__((stdcall)) *)(void *, void *))(uintptr_t)t->system_routine)(
            (void *)(uintptr_t)t->start_address, (void *)(uintptr_t)t->start_context);
    } else if (t->cdecl_entry) {
        ((void(*)())(uintptr_t)t->start_address)(); // the XBE entry point: cdecl, no arguments
    } else {
        ((void(__attribute__((stdcall)) *)(void *))(uintptr_t)t->start_address)(
            (void *)(uintptr_t)t->start_context);
    }

    pthread_cleanup_pop(1); // runs finish() here and on pthread_exit
    return nullptr;
}

void init_record(XboxThread *t, const char *name)
{
    // Zeroed where it matters, but not with memset: XboxThread holds a mutex and a condition variable, and
    // overwriting those is both wrong and warned about.
    t->prcb = XboxKprcb{};
    memset(t->ethread, 0, sizeof t->ethread);
    pthread_mutex_init(&t->gate, nullptr);
    pthread_cond_init(&t->gate_cv, nullptr);
    t->unique_thread = g_unique_thread++;
    t->tls_size = 0x1000; // a thread with no TLS asked for still needs somewhere to point
    t->tls_data = (uint8_t *)calloc(1, t->tls_size);
    if (name)
        snprintf(t->name, sizeof t->name, "%s", name);
    else
        snprintf(t->name, sizeof t->name, "xbox-%u", t->unique_thread);
}

void register_thread(XboxThread *t)
{
    std::lock_guard<std::mutex> g(g_registry);
    t->handle = xhandle_make(XHANDLE_THREAD, (uint32_t)g_threads.size());
    g_threads.push_back(t);
}

} // namespace

void *kthread_kthread(XboxThread *t) { return t ? t->ethread : nullptr; }

XboxThread *kthread_current()
{
    if (t_current) return t_current;

    // A thread the loader did not create: the main thread, or a host thread that has called into game
    // code. It gets a record so the block it reads has real values, but no handle -- nothing in the Xbox
    // would be able to name a thread the kernel did not start.
    XboxThread *t = new XboxThread();
    init_record(t, nullptr);
    snprintf(t->name, sizeof t->name, "host-%u", t->unique_thread);
    if (!t->tls_data) return nullptr;
    register_thread(t);
    t->running = true;
    g_live++;
    t_current = t;
    publish(t);
    logf("thread '%s' adopted: the loader did not create it, so it has no handle", t->name);
    return t;
}

XboxThread *kthread_current_or_null() { return t_current; }

XboxThread *kthread_from_kthread(const void *kthread)
{
    std::lock_guard<std::mutex> g(g_registry);
    for (XboxThread *t : g_threads)
        if (t->ethread == (const uint8_t *)kthread) return t;
    return nullptr;
}

XboxThread *kthread_from_handle(uint32_t handle)
{
    if (!xhandle_is(handle, XHANDLE_THREAD)) return nullptr;
    std::lock_guard<std::mutex> g(g_registry);
    const uint32_t i = xhandle_index(handle);
    return i < g_threads.size() ? g_threads[i] : nullptr;
}

void kthread_set_irql(uint8_t irql)
{
    XboxTeb *b = teb();
    b->irql = irql;
    if (b->prcb) ((XboxKprcb *)(uintptr_t)b->prcb)->irql = irql;
}

void kthread_set_dpc_active(bool active)
{
    XboxTeb *b = teb();
    b->dpc_active = active ? 1 : 0;
    if (b->prcb) ((XboxKprcb *)(uintptr_t)b->prcb)->dpc_routine_active = active ? 1 : 0;
}

uint32_t kthread_count()
{
    std::lock_guard<std::mutex> g(g_registry);
    return (uint32_t)g_threads.size();
}

uint32_t kthread_live_count() { return g_live.load(); }

void kthread_mark_finished(XboxThread *t)
{
    if (!t) return;
    pthread_mutex_lock(&t->gate);
    t->finished = true;
    t->running = false;
    pthread_cond_broadcast(&t->gate_cv);
    pthread_mutex_unlock(&t->gate);
    // g_live counts threads that have not finished; a thread marked from outside is still running, so this
    // deliberately does not touch it.
}

bool kthread_wait_finished(XboxThread *t, uint32_t slice_ms)
{
    if (!t) return true;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (long)slice_ms * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec++;
    }
    pthread_mutex_lock(&t->gate);
    bool done = true;
    while (!t->finished) {
        if (pthread_cond_timedwait(&t->gate_cv, &t->gate, &deadline) == ETIMEDOUT) {
            done = t->finished;
            break;
        }
    }
    pthread_mutex_unlock(&t->gate);
    return done;
}

// ----------------------------------------------------------------------------------------------------------

NTSTATUS kthread_create(uint32_t *out_handle, uint32_t *out_tid, uint32_t stack_size, uint32_t tls_size,
                        uint32_t start_routine, uint32_t start_context, uint32_t system_routine, bool suspended,
                        const char *name, bool cdecl_entry)
{
    XboxThread *t = new XboxThread();
    init_record(t, name);
    if (tls_size) {
        // The game asked for a specific amount; that is the block KTHREAD.TlsData points at and the one
        // XAPI walks from NtTib.StackBase.
        free(t->tls_data);
        t->tls_size = tls_size;
        t->tls_data = (uint8_t *)calloc(1, tls_size);
        if (!t->tls_data) {
            delete t;
            return KT_NO_MEMORY;
        }
    }
    t->start_address = start_routine;
    t->start_context = start_context;
    t->system_routine = system_routine;
    t->cdecl_entry = cdecl_entry;
    t->suspend_count = suspended ? 1 : 0;
    memcpy(t->ethread + ETHREAD_START_ADDRESS, &start_routine, 4);
    memcpy(t->ethread + ETHREAD_UNIQUE_THREAD, &t->unique_thread, 4);
    register_thread(t);

    const size_t stack = stack_size < MIN_HOST_STACK ? MIN_HOST_STACK : stack_size;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    const int rc = pthread_create(&t->host, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        logf("thread '%s': pthread_create failed with %d", t->name, rc);
        return KT_NO_MEMORY;
    }
    g_live++;
    if (out_handle) *out_handle = t->handle;
    if (out_tid) *out_tid = t->unique_thread;
    logf("thread '%s' created: routine 0x%08x context 0x%08x system 0x%08x tls %u stack 0x%zx%s -> handle 0x%08x "
         "id %u",
         t->name, start_routine, start_context, system_routine, t->tls_size, stack, suspended ? " suspended" : "",
         t->handle, t->unique_thread);
    return KT_SUCCESS;
}

// ----------------------------------------------------------------------------------------------------------
// exports
// ----------------------------------------------------------------------------------------------------------

extern "C" NTSTATUS __attribute__((stdcall)) PsCreateSystemThreadEx(uint32_t *ThreadHandle,
                                                                   uint32_t ThreadExtensionSize,
                                                                   uint32_t KernelStackSize,
                                                                   uint32_t TlsDataSize, uint32_t *ThreadId,
                                                                   void *StartRoutine, void *StartContext,
                                                                   uint8_t CreateSuspended, uint8_t DebuggerThread,
                                                                   void *SystemRoutine)
{
    (void)ThreadExtensionSize; // the console's per-thread extension; nothing here needs one
    (void)DebuggerThread;      // and nothing here is a debugger
    uint32_t tid = 0;
    const NTSTATUS s = kthread_create(ThreadHandle, &tid, KernelStackSize, TlsDataSize,
                                      (uint32_t)(uintptr_t)StartRoutine, (uint32_t)(uintptr_t)StartContext,
                                      (uint32_t)(uintptr_t)SystemRoutine, CreateSuspended != 0, nullptr, false);
    if (ThreadId) *ThreadId = tid;
    return s;
}

extern "C" void __attribute__((stdcall)) PsTerminateSystemThread(NTSTATUS ExitStatus)
{
    XboxThread *t = kthread_current();
    if (!t) {
        logf("thread: PsTerminateSystemThread(0x%08x) from a thread with no record", (uint32_t)ExitStatus);
        return;
    }
    logf("thread '%s' terminating with status 0x%08x", t->name, (uint32_t)ExitStatus);
    t->exit_status = ExitStatus;
    // A thread that exits holding the dispatch mutex would wedge every later raise. The Xbox needs no such
    // care because it has no locks -- the IRQL *is* the exclusion -- so getting this right here is the price
    // of using a mutex for it.
    if (xk_holds_dispatch_lock()) KfLowerIrql(IRQL_PASSIVE);
    pthread_exit((void *)(uintptr_t)(uint32_t)ExitStatus);
}

extern "C" NTSTATUS __attribute__((stdcall)) NtSuspendThread(uint32_t ThreadHandle, uint32_t *PreviousSuspendCount)
{
    XboxThread *t = kthread_from_handle(ThreadHandle);
    if (!t) return KT_INVALID_HANDLE;
    pthread_mutex_lock(&t->gate);
    const uint32_t prev = (uint32_t)t->suspend_count;
    t->suspend_count++;
    pthread_mutex_unlock(&t->gate);
    if (PreviousSuspendCount) *PreviousSuspendCount = prev;
    return KT_SUCCESS;
}

extern "C" NTSTATUS __attribute__((stdcall)) NtResumeThread(uint32_t ThreadHandle, uint32_t *PreviousSuspendCount)
{
    XboxThread *t = kthread_from_handle(ThreadHandle);
    if (!t) return KT_INVALID_HANDLE;
    pthread_mutex_lock(&t->gate);
    const uint32_t prev = (uint32_t)t->suspend_count;
    if (t->suspend_count > 0) t->suspend_count--;
    if (t->suspend_count == 0) pthread_cond_broadcast(&t->gate_cv);
    pthread_mutex_unlock(&t->gate);
    if (PreviousSuspendCount) *PreviousSuspendCount = prev;
    return KT_SUCCESS;
}

extern "C" int32_t __attribute__((stdcall)) KeQueryBasePriorityThread(void *Thread)
{
    XboxThread *t = kthread_from_kthread(Thread);
    return t ? t->base_priority : 0;
}

extern "C" int32_t __attribute__((stdcall)) KeSetBasePriorityThread(void *Thread, int32_t Increment)
{
    XboxThread *t = kthread_from_kthread(Thread);
    if (!t) {
        logf("thread: KeSetBasePriorityThread(%p): not a thread this port knows", Thread);
        return 0;
    }
    const int32_t old = t->base_priority;
    t->base_priority = Increment;
    // The console's priority increment does not map onto anything useful on a many-core Linux: raising a
    // thread's nice value would need privileges the loader does not have and would change the host's
    // scheduling. Recording it is what the game can observe, and that is what this does.
    return old;
}

extern "C" NTSTATUS __attribute__((stdcall)) ObReferenceObjectByHandle(uint32_t Handle, void *ObjectType,
                                                                      void **ReturnedObject)
{
    if (ReturnedObject) *ReturnedObject = nullptr;
    // A thread handle is the only kind this port can answer with an object pointer: the Xbox's object types
    // are opaque blobs, and the game passes the type it got from the import table to say what it expects.
    XboxThread *t = kthread_from_handle(Handle);
    if (t) {
        if (ReturnedObject) *ReturnedObject = kthread_kthread(t);
        return KT_SUCCESS;
    }
    if (ObjectType) {
        logf("thread: ObReferenceObjectByHandle(%08x, type %p): not a thread handle", Handle, ObjectType);
        return KT_INVALID_HANDLE;
    }
    logf("thread: ObReferenceObjectByHandle(%08x): object type not supported yet", Handle);
    return KT_OBJECT_TYPE_MISMATCH;
}

extern "C" void __attribute__((fastcall)) ObfDereferenceObject(void *Object) { (void)Object; }