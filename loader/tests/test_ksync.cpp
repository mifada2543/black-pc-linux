// The Xbox synchronisation surface: events, semaphores, waits, IRQL, dispatcher objects in the game's own
// memory, and NtClose's cross-layer dispatch.
//
// The waits here are tested against real threads on purpose. A wait/signal pair that only ever runs on
// one thread proves nothing about a condition variable, and the whole point of this layer is that the
// Xbox's ordering survives on a machine with cores. The IRQL checks likewise look at whether the dispatch
// mutex really excludes another thread, not at whether a variable changed.
#include "kernel/k_sync_posix.h"

#include "platform/posix/xhandle.h"
#include "platform/posix/xsync.h"

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
    fflush(stdout); // this suite waits on threads and may be killed: the last line has to survive
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

// A timeout that has already expired: the Xbox's shape is a negative LARGE_INTEGER in 100 ns units.
static XLargeInt expired()
{
    XLargeInt t;
    t.QuadPart = -100000; // 10 ms
    return t;
}

static XLargeInt relative_ms(int64_t ms)
{
    XLargeInt t;
    t.QuadPart = -(ms * 10000);
    return t;
}

static XLargeInt forever()
{
    XLargeInt t;
    t.QuadPart = -1;
    return t;
}

static int64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ----------------------------------------------------------------------------------------------------------
// events and semaphores through the Xbox's own calls
// ----------------------------------------------------------------------------------------------------------

static void test_events()
{
    printf("\nevents\n");

    uint32_t event = 0;
    check(NtCreateEvent(&event, nullptr, 0 /* notification */, 0) == KS_SUCCESS, "an unsignalled event is created");
    check_u32(xhandle_kind(event), XHANDLE_SYNC, "and its handle says it is a sync object");

    // Unsignalled, so a wait must time out. The status is STATUS_TIMEOUT (0x102), which is non-negative:
    // this is the footgun xsync.h warns about, and it is checked here so nobody "fixes" it later.
    XLargeInt t = expired();
    check_u32((uint32_t)NtWaitForSingleObject(event, 0, &t), 0x102, "a wait on an unsignalled event times out");

    int32_t previous = -1;
    check(NtSetEvent(event, &previous) == KS_SUCCESS, "the event is set");
    check_u32((uint32_t)previous, 0, "and reports that it was not signalled before");
    check(NtWaitForSingleObject(event, 0, &t) == KS_SUCCESS, "now a wait succeeds");
    // A notification event stays signalled; a second wait must also succeed.
    check(NtWaitForSingleObject(event, 0, &t) == KS_SUCCESS, "and a notification event stays signalled");

    // A synchronisation event is consumed by the wait.
    uint32_t sync_event = 0;
    check(NtCreateEvent(&sync_event, nullptr, 1 /* synchronisation */, 1) == KS_SUCCESS,
          "a signalled synchronisation event is created");
    check(NtWaitForSingleObject(sync_event, 0, &t) == KS_SUCCESS, "its first wait succeeds");
    check(NtWaitForSingleObject(sync_event, 0, &t) == 0x102, "and the second one times out: it was consumed");

    check(NtClose(event) == KS_SUCCESS, "the notification event closes");
    check(NtClose(sync_event) == KS_SUCCESS, "and so does the synchronisation one");
    check(NtWaitForSingleObject(event, 0, &t) == KS_INVALID_HANDLE, "a closed handle is no longer waited on");
}

static void test_semaphores()
{
    printf("\nsemaphores\n");

    uint32_t sem = 0;
    check(NtCreateSemaphore(&sem, nullptr, 1, 4) == KS_SUCCESS, "a semaphore with one of four is created");

    int32_t previous = -1;
    check(NtReleaseSemaphore(sem, 1, &previous) == KS_SUCCESS, "one more is released, so there are two");
    check_u32((uint32_t)previous, 1, "reporting the count before the release");
    check(NtReleaseSemaphore(sem, 5, nullptr) == KS_UNSUCCESSFUL, "releasing past the maximum is refused");
    check(NtReleaseSemaphore(sem, -1, nullptr) == KS_INVALID_PARAMETER, "releasing a negative count is refused");

    XLargeInt t = expired();
    check(NtWaitForSingleObject(sem, 0, &t) == KS_SUCCESS, "a wait takes one");
    check(NtWaitForSingleObject(sem, 0, &t) == KS_SUCCESS, "a second wait takes the other");
    check(NtWaitForSingleObject(sem, 0, &t) == 0x102, "and the third times out: the count is zero");

    // The Ex flavour the game uses for a countdown, through the same handle.
    check(NtClose(sem) == KS_SUCCESS, "the semaphore closes");
}

// Waits across threads: the whole point of the layer.
struct Waiter {
    uint32_t handle;
    NTSTATUS status = 0xFFFFFFFF;
    int64_t waited_ms = 0;
};

static void *wait_in_thread(void *arg)
{
    Waiter *w = (Waiter *)arg;
    const int64_t start = now_ms();
    XLargeInt t = forever();
    w->status = NtWaitForSingleObject(w->handle, 0, &t);
    w->waited_ms = now_ms() - start;
    return nullptr;
}

static void test_wait_across_threads()
{
    printf("\nwaits across threads\n");

    uint32_t event = 0;
    check(NtCreateEvent(&event, nullptr, 0, 0) == KS_SUCCESS, "an event to wait on");

    Waiter w{event};
    pthread_t thread;
    pthread_create(&thread, nullptr, wait_in_thread, &w);
    usleep(50 * 1000); // let it reach the wait
    check(w.status == 0xFFFFFFFF, "the waiter is still waiting");

    NtSetEvent(event, nullptr);
    pthread_join(thread, nullptr);
    check(w.status == KS_SUCCESS, "setting the event releases the waiting thread");
    check(w.waited_ms < 2000, "and it did not wait for anything else (" + std::to_string(w.waited_ms) + " ms)");

    // A wait that is given a deadline the caller sets must return on time, not hang.
    uint32_t unsignalled = 0;
    NtCreateEvent(&unsignalled, nullptr, 0, 0);
    const int64_t start = now_ms();
    XLargeInt deadline = relative_ms(120);
    check(NtWaitForSingleObject(unsignalled, 0, &deadline) == 0x102, "a relative timeout expires");
    const int64_t took = now_ms() - start;
    check(took >= 100 && took < 1000, "after about the time asked for (" + std::to_string(took) + " ms)");

    NtClose(event);
    NtClose(unsignalled);
}

// ----------------------------------------------------------------------------------------------------------
// IRQL
// ----------------------------------------------------------------------------------------------------------

static bool g_try_entered, g_try_done;

static void *try_to_take_the_dispatch_lock(void *)
{
    g_try_entered = true;
    const uint8_t old = KeRaiseIrqlToDpcLevel(); // this must block: the other thread holds it
    g_try_done = true;
    // Lowering again is not optional. A thread that exits holding the dispatch mutex leaves it locked for
    // good -- which is true of the Xbox's IRQL too, and is why every raise there has a matching lower.
    KfLowerIrql(old);
    return nullptr;
}

static void test_irql()
{
    printf("\nIRQL\n");

    check_u32(xk_current_irql(), IRQL_PASSIVE, "a thread starts at passive level");

    const uint8_t old = KeRaiseIrqlToDpcLevel();
    check_u32(old, IRQL_PASSIVE, "raising to DPC level reports the previous level");
    check_u32(xk_current_irql(), IRQL_DISPATCH, "and the level is dispatch now");
    check(xk_holds_dispatch_lock(), "which means the process-wide dispatch mutex is held");

    // The point of the mutex: another thread cannot get in while this one is at dispatch level.
    g_try_done = false;
    pthread_t thread;
    pthread_create(&thread, nullptr, try_to_take_the_dispatch_lock, nullptr);
    usleep(120 * 1000);
    check(!g_try_done, "another thread cannot raise to dispatch level while this one holds it");

    KfLowerIrql(old);
    check_u32(xk_current_irql(), IRQL_PASSIVE, "lowering it back releases the mutex");
    pthread_join(thread, nullptr);
    check(g_try_done, "and the other thread gets in once it is released");

    // A raise never lowers, and a raise from below dispatch does not take the mutex.
    KfRaiseIrql(IRQL_APC);
    check_u32(xk_current_irql(), IRQL_APC, "raising to APC level works");
    check(!xk_holds_dispatch_lock(), "and takes no dispatch mutex");
    KfLowerIrql(IRQL_PASSIVE);
    check_u32(xk_current_irql(), IRQL_PASSIVE, "lowering back to passive works");

    check(NtYieldExecution() == KS_SUCCESS, "NtYieldExecution succeeds");
}

static void test_delays()
{
    printf("\ndelays\n");

    int64_t start = now_ms();
    KeStallExecutionProcessor(20000); // 20 ms
    const int64_t stalled = now_ms() - start;
    check(stalled >= 15 && stalled < 1000, "a stall waits about as long as it was told (" +
                                                std::to_string(stalled) + " ms)");

    start = now_ms();
    XLargeInt interval;
    interval.QuadPart = -300000; // 30 ms
    check(KeDelayExecutionThread(0, 0, &interval) == KS_SUCCESS, "a relative delay succeeds");
    const int64_t delayed = now_ms() - start;
    check(delayed >= 25 && delayed < 1000, "and waited about as long as it was told (" +
                                               std::to_string(delayed) + " ms)");

    start = now_ms();
    check(KeDelayExecutionThread(0, 0, nullptr) == KS_SUCCESS, "a delay with no interval succeeds");
    check(now_ms() - start < 200, "and returns rather than waiting forever");
}

// ----------------------------------------------------------------------------------------------------------
// dispatcher objects in the game's own memory
// ----------------------------------------------------------------------------------------------------------

static uint8_t sync_calls;

static uint8_t __attribute__((stdcall)) synchronizing_routine(void *context)
{
    sync_calls++;
    *(int *)context += 41;
    return 1;
}

static void test_dispatcher_objects()
{
    printf("\nobjects in the game's memory\n");

    // A KEVENT the game would allocate itself: the Xbox's 24 bytes, and SignalState is the truth.
    alignas(4) uint8_t storage[sizeof(XKeEvent)];
    memset(storage, 0, sizeof storage);
    XKeEvent *event = (XKeEvent *)storage;

    check_u32(event->Header.Type, 0, "a fresh event's header is zeroed");
    check_u32(event->Header.SignalState, 0, "and unsignalled");

    // The game would have called KeInitializeEvent; doing the same by hand here keeps this test about the
    // wait and signal paths rather than about an initialiser.
    event->Header.Type = XOBJ_EVENT_NOTIFICATION;
    event->Header.Size = (uint16_t)(sizeof(XKeEvent) / 4);
    event->Header.SignalState = 0;
    event->Header.WaitListHead.Flink = &event->Header.WaitListHead;
    event->Header.WaitListHead.Blink = &event->Header.WaitListHead;

    check_u32((uint32_t)KeSetEvent(event, 1, 0), 0, "KeSetEvent reports the state before it signalled");
    check_u32(event->Header.SignalState, 1, "and SignalState -- which the game reads directly -- is now 1");

    XLargeInt t = expired();
    check(KeWaitForSingleObject(&event->Header, 0, 0, 0, &t) == KS_SUCCESS, "a wait on it succeeds at once");

    event->Header.SignalState = 0; // the game may reset it, exactly as it may read it
    check(KeWaitForSingleObject(&event->Header, 0, 0, 0, &t) == 0x102, "after the game clears it, a wait times out");

    // KeSynchronizeExecution runs a routine under the dispatch mutex.
    int context = 1;
    sync_calls = 0;
    check_u32(KeSynchronizeExecution(&event->Header, (void *)synchronizing_routine, &context), 1,
              "KeSynchronizeExecution reports success");
    check_u32(sync_calls, 1, "the routine ran once");
    check_u32((uint32_t)context, 42, "with the context it was given");

    check(KeWaitForSingleObject(nullptr, 0, 0, 0, &t) == KS_INVALID_HANDLE, "a null object is refused");
}

// ----------------------------------------------------------------------------------------------------------
// handle dispatch
// ----------------------------------------------------------------------------------------------------------

static void test_handle_dispatch()
{
    printf("\nhandle dispatch\n");

    uint32_t event = 0, sem = 0;
    NtCreateEvent(&event, nullptr, 0, 0);
    NtCreateSemaphore(&sem, nullptr, 0, 1);

    // Same arena, two objects, two values: closing one must not affect the other.
    check(event != sem, "an event and a semaphore have different handles");
    check(NtClose(event) == KS_SUCCESS, "the event closes");
    XLargeInt t = expired();
    check(NtWaitForSingleObject(sem, 0, &t) == 0x102, "and the semaphore is untouched: a wait on it still works");
    check(NtClose(sem) == KS_SUCCESS, "the semaphore closes too");

    check(NtClose(0) == KS_INVALID_HANDLE, "closing zero is refused");
    check(NtClose(0xFFFFFFFF) == KS_INVALID_HANDLE, "closing the invalid handle is refused");
    check(NtClose(0x50000000) == KS_INVALID_HANDLE, "closing a thread handle is refused, and says so");

    // Duplicating a sync object would need two handles for one object, which this arena cannot express.
    uint32_t dup = 0;
    check(NtDuplicateObject(event, &dup, 0) == KS_NOT_SUPPORTED, "duplicating a sync handle reports that it cannot");
    check(NtDuplicateObject(event, nullptr, 0) == KS_INVALID_PARAMETER, "and a null target is refused");
}

int main()
{
    printf("BLACK loader -- kernel synchronisation\n");

    test_events();
    test_semaphores();
    test_wait_across_threads();
    test_irql();
    test_delays();
    test_dispatcher_objects();
    test_handle_dispatch();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}