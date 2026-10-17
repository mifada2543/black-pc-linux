// Xbox dispatcher objects and IRQL emulation, on POSIX.
//
// The model, unchanged from loader/src/kernel/k_sync.cpp
// ---------------------------------------------------
// Game code owns these objects: a KEVENT or KSEMAPHORE is memory in the Xbox image, and the game
// reads and writes its fields directly. So the Xbox structure is the truth -- SignalState above all --
// and the host side only provides something to sleep on. That is what makes this work on a machine
// with many cores, where the Xbox's single CPU gave away ordering for free.
//
// Two kinds of object, deliberately handled differently:
//   * handle-based (NtCreateEvent, NtCreateSemaphore) get a host object, because the game only ever
//     holds the handle
//   * dispatcher objects living in Xbox memory get a wake primitive keyed by their address, so the
//     game can read SignalState at any time and the loader never has to mirror it
//
// The generation counter is what makes notification-type waits correct: a notification event stays
// signalled, so a waiter must not be released by a signal that happened before it started waiting.
// Recording how many signals an object has seen, and comparing before and after, separates "signalled
// while I waited" from "was already signalled".
#pragma once

#include <stdint.h>

// ---- Xbox kernel ABI ----

using NTSTATUS = int32_t;

#define XK_SUCCESS ((NTSTATUS)0x00000000)
#define XK_TIMEOUT ((NTSTATUS)0x00000102)
#define XK_PENDING ((NTSTATUS)0x00000103)
#define XK_USER_APC ((NTSTATUS)0x000000C0)
#define XK_NO_MORE_FILES ((NTSTATUS)0x80000006)
#define XK_ALERTED ((NTSTATUS)0x00000101)
#define XK_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define XK_INVALID_HANDLE ((NTSTATUS)0xC0000008)
#define XK_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define XK_ACCESS_DENIED ((NTSTATUS)0xC0000022)
#define XK_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034)
#define XK_NOT_SUPPORTED ((NTSTATUS)0xC00000BB)
#define XK_INHERITED_WAIT ((NTSTATUS)0x80000004)
#define XK_ABANDONED_WAIT ((NTSTATUS)0x00000080)
#define XK_OBJECT_NAME_EXISTS ((NTSTATUS)0x40000000)

// A timeout is a LARGE_INTEGER in 100 ns units. Negative means relative, -1 is infinite. That
// matches every Xbox timeout the kernel takes, and the conversion lives in xk_timeout_ms().
struct XLargeInt {
    union {
        struct {
            uint32_t LowPart;
            int32_t HighPart;
        };
        int64_t QuadPart;
    };
};

struct XListEntry {
    XListEntry *Flink;
    XListEntry *Blink;
};

struct XDispatcherHeader {
    uint8_t Type;
    uint8_t Absolute;
    uint8_t Size;
    uint8_t Inserted;
    int32_t SignalState;
    XListEntry WaitListHead;
};
static_assert(sizeof(XDispatcherHeader) == 16, "DISPATCHER_HEADER is 16 bytes");

enum : uint8_t {
    XOBJ_EVENT_NOTIFICATION = 0,
    XOBJ_EVENT_SYNCHRONIZATION = 1,
    XOBJ_MUTANT = 2,
    XOBJ_PROCESS = 3,
    XOBJ_QUEUE = 4,
    XOBJ_SEMAPHORE = 5,
    XOBJ_THREAD = 6,
    XOBJ_TIMER_NOTIFICATION = 8,
    XOBJ_TIMER_SYNCHRONIZATION = 9,
    XOBJ_DPC = 19,
};

struct XKeEvent {
    XDispatcherHeader Header;
};

struct XKSemaphore {
    XDispatcherHeader Header;
    int32_t Limit;
};

// Rtl critical section: 28 bytes, LockCount starts at -1, OwningThread is a KTHREAD*.
struct XRtlCriticalSection {
    XDispatcherHeader Synchronization;
    int32_t LockCount;
    int32_t RecursionCount;
    void *OwningThread;
};
static_assert(sizeof(XRtlCriticalSection) == 28, "RTL_CRITICAL_SECTION is 28 bytes");

// NT_SUCCESS, as Windows defines it: bit 31 clear. Note the footgun this leaves -- STATUS_TIMEOUT
// (0x102), STATUS_ALERTED (0x101), STATUS_USER_APC (0xC0) and STATUS_PENDING (0x103) are all
// non-negative, and so all "succeed". A wait that is *meant* to succeed has to be compared against
// XK_SUCCESS directly, never against this.
#define XK_NT_OK(s) ((s) >= 0)

using XHandle = uint32_t;
constexpr XHandle XINVALID_HANDLE = 0xFFFFFFFFu;

// ---- timeouts ----
// 100 ns units to milliseconds. Negative values other than -1 are relative waits in the Xbox, and the
// kernel takes them as absolute in some places, so this only has to be right for the shapes the game
// actually passes.
uint32_t xk_timeout_ms(const XLargeInt *timeout);

// Monotonic milliseconds, for deadline arithmetic. The Xbox TSC emulation in timing.cpp scales this to
// 733.33 MHz for the game's own timestamps.
uint64_t xk_tick_ms();

// ---- IRQL ----
//
// Raising to DISPATCH_LEVEL (2) or above takes one process-wide lock. Code written for the single-CPU
// Xbox uses IRQL to exclude other threads; on a many-core machine the same exclusion has to come from
// somewhere real, and a mutex held for the duration of the raised IRQL is the honest translation.
// Passive (0), APC (1) and DISPATCH (2) are the values the game uses; DISPATCH is the cut-off.

enum : uint8_t {
    IRQL_PASSIVE = 0,
    IRQL_APC = 1,
    IRQL_DISPATCH = 2,
    IRQL_DEVICE = 3,
    IRQL_TIMER = 12,
    IRQL_HIGH = 15,
};

void xk_init();
// Raise to new_irql, returning the previous one. Takes the dispatch lock when crossing DISPATCH_LEVEL.
uint8_t xk_raise_irql(uint8_t new_irql);
// Lower to new_irql. Releases the dispatch lock when dropping below DISPATCH_LEVEL.
void xk_lower_irql(uint8_t new_irql);
uint8_t xk_current_irql();
// True while any thread holds the dispatch lock. A thread that exits holding it must not strand it.
bool xk_holds_dispatch_lock();
// KeStallExecutionProcessor: spin for the given microseconds. Used by short hardware-ish delays.
void xk_stall_us(uint32_t us);
// KeSynchronizeExecution: run fn(arg) with IRQL raised to DISPATCH_LEVEL, on whichever thread wins
// the dispatch lock.
void xk_synchronize_execution(void (*fn)(void *), void *arg);

// ---- dispatcher objects in Xbox memory ----

int32_t xk_set_event(XKeEvent *event, int32_t increment, uint8_t wait);
// Set the object signalled (SignalState = 1) and wake every waiter. This is what KeSetEvent does, and
// it is wrong for a semaphore releasing several tokens at once -- use xk_release_object() there.
void xk_signal_object(XDispatcherHeader *object);
// KeReleaseSemaphore on an object living in Xbox memory: add count to SignalState and wake waiters,
// which then take them one at a time through try_consume().
void xk_release_object(XDispatcherHeader *object, int32_t count);
NTSTATUS xk_wait_for_object(XDispatcherHeader *object, uint8_t alertable, const XLargeInt *timeout);

// ---- critical sections ----

void xk_initialize_critical_section(XRtlCriticalSection *cs);
void xk_enter_critical_section(XRtlCriticalSection *cs);
void xk_leave_critical_section(XRtlCriticalSection *cs);

// ---- handle-based objects (NtCreateEvent / NtCreateSemaphore) ----

NTSTATUS xk_create_event(XHandle *out, uint32_t event_type, uint8_t initial_state);
NTSTATUS xk_set_event_handle(XHandle h, int32_t *previous_state);
NTSTATUS xk_create_semaphore(XHandle *out, int32_t initial, int32_t maximum);
NTSTATUS xk_release_semaphore(XHandle h, int32_t count, int32_t *previous_count);
NTSTATUS xk_wait_handle(XHandle h, uint8_t alertable, const XLargeInt *timeout);
NTSTATUS xk_close(XHandle h);

// Diagnostics: how many wake primitives and handle objects are live.
uint32_t xk_live_wake_count();
uint32_t xk_live_handle_count();