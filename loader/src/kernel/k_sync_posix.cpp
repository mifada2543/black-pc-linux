#include "kernel/k_sync_posix.h"

#include "kernel/k_thread_posix.h" // kthread_set_irql: the level the game reads at fs:[0x24]
#include "platform/posix/log.h"
#include "platform/posix/xfile.h"   // xf_close / xf_duplicate, for NtClose's cross-layer dispatch
#include "platform/posix/xhandle.h" // which arena a handle belongs to

#include <errno.h>
#include <sched.h>
#include <string.h>
#include <time.h>

#include <string>

namespace {

// The name in an OBJECT_ATTRIBUTES, for the log line. The game names some objects (XAPI names events
// after the subsystem that made them) and leaves the rest null.
std::string object_name(void *oa)
{
    if (!oa) return {};
    const XObjectAttributes *a = (const XObjectAttributes *)oa;
    return a->ObjectName ? xstr(a->ObjectName) : std::string();
}

// xsync takes a C function pointer, KeSynchronizeExecution takes an Xbox one, and the call has to
// happen while the dispatch mutex is held -- so the pair the game passed has to be carried in the
// thread that is making the call. Thread-local, not global: two threads may synchronize at once.
__thread uint8_t (*t_sync_fn)(void *) __attribute__((stdcall)) = nullptr;
__thread void *t_sync_arg = nullptr;

void sync_trampoline(void *) { t_sync_fn(t_sync_arg); }

} // namespace

// ----------------------------------------------------------------------------------------------------------
// IRQL

extern "C" uint8_t __attribute__((stdcall)) KeRaiseIrqlToDpcLevel()
{
    // Returns the previous IRQL, which the game uses to restore it. DISPATCH_LEVEL is the level DPCs and
    // interrupts run at, and xsync's dispatch mutex is what that means here. The new level is published
    // into this thread's block as well, because the game reads its IRQL at fs:[0x24] and expects to see
    // the level it just raised to.
    const uint8_t old = xk_raise_irql(IRQL_DISPATCH);
    kthread_set_irql(xk_current_irql());
    return old;
}

extern "C" uint8_t __attribute__((fastcall)) KfRaiseIrql(uint8_t NewIrql)
{
    const uint8_t old = xk_raise_irql(NewIrql);
    kthread_set_irql(xk_current_irql());
    return old;
}

extern "C" void __attribute__((fastcall)) KfLowerIrql(uint8_t NewIrql)
{
    xk_lower_irql(NewIrql);
    kthread_set_irql(xk_current_irql());
}

extern "C" void __attribute__((stdcall)) KeStallExecutionProcessor(uint32_t MicroSeconds)
{
    xk_stall_us(MicroSeconds);
}

extern "C" NTSTATUS __attribute__((stdcall)) KeDelayExecutionThread(uint8_t WaitMode, uint8_t Alertable,
                                                                    XLargeInt *Interval)
{
    (void)WaitMode;
    (void)Alertable;
    const uint32_t ms = xk_timeout_ms(Interval);
    if (ms == 0xFFFFFFFFu) {
        // A zero relative interval is a yield on the Xbox; an infinite one is a wait that never ends,
        // which a boot that has to be interruptible cannot honour.
        if (Interval && Interval->QuadPart < 0 && Interval->QuadPart != -1) {
            xk_stall_us(0);
            return KS_SUCCESS;
        }
        logf("sync: KeDelayExecutionThread asked to wait forever; returning instead of hanging the boot");
        return KS_SUCCESS;
    }
    xk_stall_us(ms * 1000u);
    return KS_SUCCESS;
}

extern "C" NTSTATUS __attribute__((stdcall)) NtYieldExecution()
{
    sched_yield();
    return KS_SUCCESS;
}

// ----------------------------------------------------------------------------------------------------------
// handle-based objects

extern "C" NTSTATUS __attribute__((stdcall)) NtCreateEvent(uint32_t *EventHandle, void *ObjectAttributes,
                                                           uint32_t EventType, uint8_t InitialState)
{
    const std::string name = object_name(ObjectAttributes);
    XHandle h = XINVALID_HANDLE;
    const NTSTATUS s = xk_create_event(&h, EventType, InitialState);
    if (s != KS_SUCCESS) return s;
    if (EventHandle) *EventHandle = h;
    logf("sync: NtCreateEvent('%s', type %u, initial %u) -> 0x%08x", name.c_str(), EventType, InitialState, h);
    return KS_SUCCESS;
}

extern "C" NTSTATUS __attribute__((stdcall)) NtCreateSemaphore(uint32_t *SemaphoreHandle, void *ObjectAttributes,
                                                               int32_t InitialCount, int32_t MaximumCount)
{
    const std::string name = object_name(ObjectAttributes);
    XHandle h = XINVALID_HANDLE;
    const NTSTATUS s = xk_create_semaphore(&h, InitialCount, MaximumCount);
    if (s != KS_SUCCESS) return s;
    if (SemaphoreHandle) *SemaphoreHandle = h;
    logf("sync: NtCreateSemaphore('%s', %d of %d) -> 0x%08x", name.c_str(), InitialCount, MaximumCount, h);
    return KS_SUCCESS;
}

extern "C" NTSTATUS __attribute__((stdcall)) NtSetEvent(uint32_t EventHandle, int32_t *PreviousState)
{
    return xk_set_event_handle(EventHandle, PreviousState);
}

extern "C" NTSTATUS __attribute__((stdcall)) NtReleaseSemaphore(uint32_t SemaphoreHandle, int32_t ReleaseCount,
                                                               int32_t *PreviousCount)
{
    return xk_release_semaphore(SemaphoreHandle, ReleaseCount, PreviousCount);
}

extern "C" NTSTATUS __attribute__((stdcall)) NtWaitForSingleObject(uint32_t Handle, uint8_t Alertable,
                                                                  XLargeInt *Timeout)
{
    return xk_wait_handle(Handle, Alertable, Timeout);
}

extern "C" NTSTATUS __attribute__((stdcall)) NtWaitForSingleObjectEx(uint32_t Handle, uint8_t WaitMode,
                                                                     uint8_t Alertable, XLargeInt *Timeout)
{
    // WaitMode selects UserMode versus KernelMode waits; there is one mode here and it is the one the
    // game asks for from user code.
    (void)WaitMode;
    return xk_wait_handle(Handle, Alertable, Timeout);
}

extern "C" NTSTATUS __attribute__((stdcall)) NtClose(uint32_t Handle)
{
    // The Xbox's handles come from one table, so the game closes a file, an event or a thread with the
    // same call and does not say which. The kind tag in the handle says, which is why it exists.
    switch (xhandle_kind(Handle)) {
    case XHANDLE_SYNC:
        return xk_close(Handle);
    case XHANDLE_FILE:
    case XHANDLE_LINK:
        return xf_close(Handle);
    case XHANDLE_THREAD: {
        // Closing a thread handle does not kill the thread on the Xbox, and neither does it here: the
        // handle is forgotten and the thread runs on. Closing the *current* thread's own handle is legal
        // too, which is why nothing is torn down here.
        XboxThread *t = kthread_from_handle(Handle);
        if (!t) return KS_INVALID_HANDLE;
        logf("sync: NtClose(0x%08x): thread '%s' keeps running, the handle is gone", Handle, t->name);
        return KS_SUCCESS;
    }
    default:
        logf("sync: NtClose(0x%08x): not a handle of this port", Handle);
        return KS_INVALID_HANDLE;
    }
}

extern "C" NTSTATUS __attribute__((stdcall)) NtDuplicateObject(uint32_t SourceHandle, uint32_t *TargetHandle,
                                                               uint32_t Options)
{
    if (!TargetHandle) return KS_INVALID_PARAMETER;
    // Duplicating a file needs its own arena slot (the duplicate shares the mount, not the descriptor),
    // which xfile does. Duplicating an event would mean two handles for one object, and xsync's arena
    // holds objects by value, so that is reported rather than faked.
    if (xhandle_kind(SourceHandle) == XHANDLE_FILE) {
        XfHandle dup = XF_INVALID_HANDLE_VALUE;
        const NTSTATUS s = xf_duplicate(SourceHandle, &dup, Options);
        if (s != KS_SUCCESS) return s;
        *TargetHandle = dup;
        return KS_SUCCESS;
    }
    if (xhandle_kind(SourceHandle) == XHANDLE_LINK) return KS_NOT_SUPPORTED;
    logf("sync: NtDuplicateObject(0x%08x): only file handles can be duplicated so far", SourceHandle);
    return KS_NOT_SUPPORTED;
}

// ----------------------------------------------------------------------------------------------------------
// dispatcher objects in the game's own memory

extern "C" int32_t __attribute__((stdcall)) KeSetEvent(XKeEvent *Event, int32_t Increment, uint8_t Wait)
{
    return xk_set_event(Event, Increment, Wait);
}

extern "C" NTSTATUS __attribute__((stdcall)) KeWaitForSingleObject(XDispatcherHeader *Object, uint32_t WaitReason,
                                                                   uint8_t WaitMode, uint8_t Alertable,
                                                                   XLargeInt *Timeout)
{
    (void)WaitReason;
    (void)WaitMode;
    return xk_wait_for_object(Object, Alertable, Timeout);
}

extern "C" uint8_t __attribute__((stdcall)) KeSynchronizeExecution(XDispatcherHeader *Interrupt,
                                                                   void *SynchronizeRoutine, void *SynchronizeContext)
{
    (void)Interrupt;
    if (!SynchronizeRoutine) return 0;
    t_sync_fn = (uint8_t(__attribute__((stdcall)) *)(void *))SynchronizeRoutine;
    t_sync_arg = SynchronizeContext;
    xk_synchronize_execution(sync_trampoline, nullptr);
    t_sync_fn = nullptr;
    t_sync_arg = nullptr;
    return 1; // the Xbox returns the routine's result; the game only branches on zero
}