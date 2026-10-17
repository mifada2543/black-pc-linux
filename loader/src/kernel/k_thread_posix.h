// Threads: the Xbox's PsCreateSystemThreadEx and everything that goes with it.
//
// What the game can observe
// -------------------------
// A thread it creates is a real thread with the Xbox's own per-thread state in reach through the
// instructions the loader has already rewritten:
//
//   fs:[0x20]  KPCR.Prcb          -> this thread's fake KPRCB
//   fs:[0x24]  KPCR.Irql          -> kept in step with xsync's IRQL, so the game reading it sees the level
//                                   it just raised with KfRaiseIrql
//   fs:[0x28]  KPRCB.CurrentThread -> the KTHREAD this handle refers to, which is what
//                                   ObReferenceObjectByHandle hands back
//   fs:[0x58]  KPRCB.DpcRoutineActive -> 1 while a DPC runs (k_dpc will set it)
//   fs:[0xE10 + 4*slot]           -> the TLS slot array, per thread
//   fs:[0x04]  NtTib.StackBase    -> where XAPI expects the TLS array, i.e. TlsData + TlsDataSize
//
// Every one of those lives in the thread's own block (teb.h), so two threads never see each other's
// values, which is the property the whole fs->gs rewrite exists to provide.
//
// The parts that cannot be the Xbox's
// ------------------------------------
// IRQL is xsync's: raising to DISPATCH_LEVEL or above takes its process-wide dispatch mutex, and this
// module only mirrors the level into the block the game reads. Suspend and resume are a counter and a
// condition variable per thread, which gives the same observable behaviour as the Xbox's suspend count:
// the creator asks for a suspended thread, NtResumeThread lets it run.
#pragma once

#include <pthread.h>
#include <stdint.h>

#include "platform/posix/teb.h"    // XboxTeb, XboxKprcb
#include "platform/posix/xhandle.h" // XHANDLE_THREAD
#include "platform/posix/xsync.h"   // NTSTATUS

// Per-thread state the thread layer owns. The KPRCB is the Xbox's fake KPRCB; the ETHREAD block is the
// structure the kernel hands out as the KTHREAD pointer, with KTHREAD as its first member on the Xbox.
struct XboxThread {
    XboxKprcb prcb;
    uint8_t ethread[0x200];

    uint8_t *tls_data = nullptr; // KTHREAD.TlsData
    uint32_t tls_size = 0;
    uint32_t handle = 0;       // this thread's Xbox handle
    uint32_t unique_thread = 0;// ETHREAD.UniqueThread: what XAPI's GetCurrentThreadId answers
    uint32_t start_address = 0;// ETHREAD.StartAddress
    int32_t base_priority = 0; // Xbox priority increment, -15..15
    int32_t exit_status = 0;   // what PsTerminateSystemThread was given
    uint32_t stack_top = 0;    // this thread's stack pointer at entry, for a crash report

    uint32_t start_context = 0;   // the entry routine's argument
    uint32_t system_routine = 0; // XPKSYSTEM_ROUTINE, called with the start routine and context
    bool cdecl_entry = false;    // the entry routine is cdecl with no arguments (the XBE entry point)

    pthread_t host;
    pthread_mutex_t gate;      // guards suspend_count and exit flags
    pthread_cond_t gate_cv;
    int32_t suspend_count = 0; // > 0 means parked; starts at 1 for a thread created suspended
    bool running = false;
    bool finished = false;
    bool detached = false;
    char name[32] = {};
};

#define KT_SUCCESS ((NTSTATUS)0x00000000)
#define KT_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define KT_NO_MEMORY ((NTSTATUS)0xC0000017)
#define KT_INVALID_HANDLE ((NTSTATUS)0xC0000008)
#define KT_OBJECT_TYPE_MISMATCH ((NTSTATUS)0xC0000024)
#define KT_NOT_SUPPORTED ((NTSTATUS)0xC00000BB)

// Create a thread. `start_routine` is called with `start_context` as its only argument (stdcall, the Xbox
// convention), or with no arguments when `cdecl_entry` is set -- which is how the XBE entry point is
// entered. `system_routine` is called the way the Xbox's XPKSYSTEM_ROUTINE is, with the start routine and
// context as its two arguments; the XDK passes it so a thread can run a wrapper before its real entry.
// `suspended` leaves the thread parked until NtResumeThread.
NTSTATUS kthread_create(uint32_t *out_handle, uint32_t *out_tid, uint32_t stack_size, uint32_t tls_size,
                        uint32_t start_routine, uint32_t start_context, uint32_t system_routine, bool suspended,
                        const char *name, bool cdecl_entry);

// This thread's state, creating one for a thread the loader did not start (the main thread, or a host
// thread that ends up calling into the game's code). Never null.
XboxThread *kthread_current();
XboxThread *kthread_current_or_null();
XboxThread *kthread_from_kthread(const void *kthread); // from the pointer the game holds
XboxThread *kthread_from_handle(uint32_t handle);

// The KTHREAD pointer, which is what the kernel hands out and what ObReferenceObjectByHandle returns.
void *kthread_kthread(XboxThread *t);

// Publish the current IRQL into the block the game reads. Called by the IRQL wrappers in k_sync_posix.
void kthread_set_irql(uint8_t irql);
// Whether a DPC is running, for fs:[0x58].
void kthread_set_dpc_active(bool active);

// How many threads the loader knows about, and how many are still running. For the boot report and tests.
uint32_t kthread_count();
uint32_t kthread_live_count();

// Mark a thread finished without waiting for it. The loader calls this when a kernel call asks to end the
// run: the thread that asked is parked and will never finish on its own, so its record has to say so, and
// saying it under the thread's own mutex is what lets the waiter see the exit request and the finished flag
// together instead of racing between two reads.
void kthread_mark_finished(XboxThread *t);

// Wait up to `slice_ms` for a thread to finish. Returns true when it has. One slice at a time, because the
// caller has other things to watch -- an exit request, for instance -- and a thread that never finishes must
// not hold the caller in here.
bool kthread_wait_finished(XboxThread *t, uint32_t slice_ms);

// ---- exports ------------------------------------------------------------------------------------------

extern "C" NTSTATUS __attribute__((stdcall)) PsCreateSystemThreadEx(uint32_t *ThreadHandle,
                                                                   uint32_t ThreadExtensionSize,
                                                                   uint32_t KernelStackSize,
                                                                   uint32_t TlsDataSize, uint32_t *ThreadId,
                                                                   void *StartRoutine, void *StartContext,
                                                                   uint8_t CreateSuspended, uint8_t DebuggerThread,
                                                                   void *SystemRoutine);
extern "C" void __attribute__((stdcall)) PsTerminateSystemThread(NTSTATUS ExitStatus);
extern "C" NTSTATUS __attribute__((stdcall)) NtSuspendThread(uint32_t ThreadHandle, uint32_t *PreviousSuspendCount);
extern "C" NTSTATUS __attribute__((stdcall)) NtResumeThread(uint32_t ThreadHandle, uint32_t *PreviousSuspendCount);
extern "C" int32_t __attribute__((stdcall)) KeQueryBasePriorityThread(void *Thread);
extern "C" int32_t __attribute__((stdcall)) KeSetBasePriorityThread(void *Thread, int32_t Increment);
extern "C" NTSTATUS __attribute__((stdcall)) ObReferenceObjectByHandle(uint32_t Handle, void *ObjectType,
                                                                      void **ReturnedObject);
extern "C" void __attribute__((fastcall)) ObfDereferenceObject(void *Object);