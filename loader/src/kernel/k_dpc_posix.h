// DPCs and kernel timers: the last six kernel ordinals.
//
// The invoice for this is src/kernel/k_dpc.cpp on the Windows side, and it is worth reading before changing
// anything here, because the shape of it is the shape of the Xbox's:
//
//   - One worker thread, created *through the thread layer* so that the game code a DPC calls sees a real
//     KPCR: its fs:[0x20], fs:[0x24], fs:[0x28] and TLS slots all have to be this thread's. A DPC that ran on
//     a plain pthread would find the loader's block, not the game's.
//   - A timer's DueTime is absolute when positive and relative when negative, and relative is `now - DueTime`.
//     That is the NT convention and games rely on it: KeSetTimer is called both ways and the sign is the only
//     thing that says which.
//   - On expiry the timer object is *signalled* (so NtWaitForSingleObject on a timer returns) and the DPC is
//     queued if it is not already queued, with SystemArgument1/2 carrying the 100ns time split into two 32-bit
//     words. That split is the Xbox's shape, and a DPC routine that does arithmetic on it expects it.
//   - A DPC runs with the IRQL raised to DISPATCH_LEVEL and KPRCB.DpcRoutineActive set, which the game reads
//     at fs:[0x58].
//   - KeInsertQueueDpc refuses (FALSE) when the DPC is already queued, and KeSetTimer cancels an existing
//     timer before arming a new one, returning whether one was armed.
//
// What this port does not do: it does not pretend to be the console's clock. Xbox timer resolution is whatever
// KeInitializeTimerEx was told (clock tick, 10ms, 1ms, 100ns); here every timer runs on the host's monotonic
// clock in 100ns units, and the requested type is logged rather than honoured, because a timer that fired on a
// 1ms tick while the game measured with KeQuerySystemTime would be wrong in a way it could detect.
#pragma once

#include <pthread.h>
#include <stdint.h>

#include "platform/posix/xsync.h" // XDispatcherHeader, XListEntry, the XOBJ_* types

// Xbox KDPC: the object the game allocates and hands to KeInitializeDpc. The layout is the Xbox's, so the
// list entry the kernel would normally link is there, and the loader's bookkeeping sits in the fields beside
// it -- the same arrangement the Windows port uses.
struct XKDPC {
    int16_t Type;
    uint8_t Inserted;
    uint8_t Padding;
    XListEntry DpcListEntry;
    // stdcall, because the game compiled the routine to pop its own four arguments: its machine code ends
    // in `ret 16`. Calling it through a plain cdecl pointer would have the caller pop them as well, and the
    // stack would come back sixteen bytes short. GCC implements stdcall on i386 -- the callee pops -- so
    // this attribute is what keeps the game's stack intact.
    void (__attribute__((stdcall)) *DeferredRoutine)(XKDPC *dpc, void *deferred_context, void *system_argument1,
                                                    void *system_argument2);
    void *DeferredContext;
    void *SystemArgument1;
    void *SystemArgument2;
};
static_assert(sizeof(struct XKDPC) == 28, "KDPC is 28 bytes on the Xbox");

// Xbox KTIMER. Period is in milliseconds, as on the console: a periodic timer re-arms itself by that much.
// There is no room for a "resolution" field, and there should not be: the game's own code knows what it asked
// for, and the size of this structure is part of the ABI it reads.
struct XKTIMER {
    XDispatcherHeader Header;
    int64_t DueTime; // 100ns units, absolute (or relative when it was set from a negative value)
    XListEntry TimerListEntry;
    XKDPC *Dpc;
    int32_t Period;
};
static_assert(sizeof(struct XKTIMER) == 40, "KTIMER is 40 bytes on the Xbox");

// Start the DPC thread. Called once, from the boot sequence; false means the thread could not start, and the
// console would be a machine whose timers never fire.
bool kdpc_init();

// How many timers are armed and how many DPCs are queued, for the boot report and the tests.
uint32_t kdpc_timer_count();
uint32_t kdpc_queue_count();

// The 100ns monotonic clock the timers run on. CLOCK_MONOTONIC, not CLOCK_REALTIME: a timer is a duration,
// and a duration that jumps when NTP corrects the clock would fire early or late. KeQuerySystemTime is the
// one that wants wall time, and it reads CLOCK_REALTIME in k_misc.
int64_t kdpc_now_100ns();

// ---- exports ------------------------------------------------------------------------------------------

extern "C" void __attribute__((stdcall)) KeInitializeDpc(
    XKDPC *Dpc, void (__attribute__((stdcall)) *DeferredRoutine)(XKDPC *, void *, void *, void *),
    void *DeferredContext);
extern "C" uint8_t __attribute__((stdcall)) KeInsertQueueDpc(XKDPC *Dpc, void *SystemArgument1,
                                                             void *SystemArgument2);
extern "C" uint8_t __attribute__((stdcall)) KeRemoveQueueDpc(XKDPC *Dpc);
extern "C" void __attribute__((stdcall)) KeInitializeTimerEx(XKTIMER *Timer, uint32_t Type);
extern "C" uint8_t __attribute__((stdcall)) KeSetTimer(XKTIMER *Timer, int64_t DueTime, XKDPC *Dpc);
extern "C" uint8_t __attribute__((stdcall)) KeCancelTimer(XKTIMER *Timer);
