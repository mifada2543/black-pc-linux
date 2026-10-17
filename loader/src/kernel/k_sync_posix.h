// Synchronization, as the game sees it: the Xbox's events, semaphores, waits, IRQL and critical
// sections, wired to the POSIX layer in xsync.{h,cpp}.
//
// There is no new synchronisation logic in this file. What it adds is the shape the game calls:
// Xbox status codes, the OBJECT_ATTRIBUTES the game passes (which xsync never sees), and one thing the
// POSIX layer cannot know by itself -- that a handle in NtClose may belong to any layer, because the
// Xbox's handles all come from one table. That dispatch is here too.
//
// What the game can observe, and therefore what has to be right:
//
//   * wait results. STATUS_TIMEOUT is 0x102, which is *non-negative*, so any game code testing a wait
//     with NT_SUCCESS() reads a timeout as success. xsync.h says this too; nothing here changes it.
//   * IRQL. Raising to DISPATCH_LEVEL or above takes xsync's process-wide dispatch mutex, which is what
//     excludes other threads on a machine with cores. KfRaiseIrql/KfLowerIrql are fastcall, so their
//     argument arrives in ecx and the call pushes nothing.
//   * critical sections, which the game allocates in its own image with the Xbox's 28-byte layout and
//     reads LockCount and OwningThread back.
#pragma once

#include <stdint.h>

#include "platform/posix/xsync.h" // XHandle, XKeEvent, XDispatcherHeader, XLargeInt, XRtlCriticalSection

// Status codes, as macros so they read the way the rest of the port writes them.
#define KS_SUCCESS ((NTSTATUS)0x00000000)
#define KS_TIMEOUT ((NTSTATUS)0x00000102)
#define KS_PENDING ((NTSTATUS)0x00000103)
#define KS_USER_APC ((NTSTATUS)0x000000C0)
#define KS_NO_MORE_FILES ((NTSTATUS)0x80000006)
#define KS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define KS_INVALID_HANDLE ((NTSTATUS)0xC0000008)
#define KS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define KS_ACCESS_DENIED ((NTSTATUS)0xC0000022)
#define KS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034)
#define KS_NOT_SUPPORTED ((NTSTATUS)0xC00000BB)
#define KS_OBJECT_NAME_EXISTS ((NTSTATUS)0x40000000)

// ---- IRQL ------------------------------------------------------------------------------------------------

extern "C" uint8_t __attribute__((stdcall)) KeRaiseIrqlToDpcLevel();
extern "C" uint8_t __attribute__((fastcall)) KfRaiseIrql(uint8_t NewIrql);
extern "C" void __attribute__((fastcall)) KfLowerIrql(uint8_t NewIrql);
extern "C" void __attribute__((stdcall)) KeStallExecutionProcessor(uint32_t MicroSeconds);
extern "C" NTSTATUS __attribute__((stdcall)) KeDelayExecutionThread(uint8_t WaitMode, uint8_t Alertable,
                                                                    XLargeInt *Interval);
extern "C" NTSTATUS __attribute__((stdcall)) NtYieldExecution();

// ---- handle-based objects ---------------------------------------------------------------------------------

extern "C" NTSTATUS __attribute__((stdcall)) NtCreateEvent(uint32_t *EventHandle, void *ObjectAttributes,
                                                           uint32_t EventType, uint8_t InitialState);
extern "C" NTSTATUS __attribute__((stdcall)) NtCreateSemaphore(uint32_t *SemaphoreHandle, void *ObjectAttributes,
                                                               int32_t InitialCount, int32_t MaximumCount);
extern "C" NTSTATUS __attribute__((stdcall)) NtSetEvent(uint32_t EventHandle, int32_t *PreviousState);
extern "C" NTSTATUS __attribute__((stdcall)) NtReleaseSemaphore(uint32_t SemaphoreHandle, int32_t ReleaseCount,
                                                               int32_t *PreviousCount);
extern "C" NTSTATUS __attribute__((stdcall)) NtWaitForSingleObject(uint32_t Handle, uint8_t Alertable,
                                                                  XLargeInt *Timeout);
extern "C" NTSTATUS __attribute__((stdcall)) NtWaitForSingleObjectEx(uint32_t Handle, uint8_t WaitMode,
                                                                     uint8_t Alertable, XLargeInt *Timeout);
extern "C" NTSTATUS __attribute__((stdcall)) NtClose(uint32_t Handle);
extern "C" NTSTATUS __attribute__((stdcall)) NtDuplicateObject(uint32_t SourceHandle, uint32_t *TargetHandle,
                                                               uint32_t Options);

// ---- dispatcher objects in the game's own memory -----------------------------------------------------------

extern "C" int32_t __attribute__((stdcall)) KeSetEvent(XKeEvent *Event, int32_t Increment, uint8_t Wait);
extern "C" NTSTATUS __attribute__((stdcall)) KeWaitForSingleObject(XDispatcherHeader *Object, uint32_t WaitReason,
                                                                   uint8_t WaitMode, uint8_t Alertable,
                                                                   XLargeInt *Timeout);
extern "C" uint8_t __attribute__((stdcall)) KeSynchronizeExecution(XDispatcherHeader *Interrupt,
                                                                   void *SynchronizeRoutine, void *SynchronizeContext);