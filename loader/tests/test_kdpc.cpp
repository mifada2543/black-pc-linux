// DPCs and kernel timers.
//
// The checks are about what the game can observe, not about the loader's bookkeeping:
//
//   - A DPC runs, at DISPATCH_LEVEL, with KPRCB.DpcRoutineActive set, on a thread with its own KPCR.
//   - A timer fires, relative or absolute, and the DPC it names runs with the 100ns time in its arguments.
//   - A cancelled timer does not fire.
//   - A DPC that is already queued is not queued twice, and KeInsertQueueDpc says so.
//
// The last one is the Xbox's rule rather than a convenience: one DPC object is in the queue or it is not, and
// the Windows port's k_dpc.cpp (the invoice for this) says the same thing.
//
// Every check that waits here waits on a *wall-clock* deadline -- two or three seconds, polled ten times a
// millisecond -- because a DPC that never runs cannot be told apart from a test that never finished. The
// consequence is that these checks can fail on a machine busy for reasons that have nothing to do with the
// DPC: one such failure happened while a full rebuild and the rest of the suite ran at the same time, and
// fifteen consecutive runs afterwards were clean. So if exactly one of these fails, run it on its own before
// believing it; if it passes there, the DPC was fine and the machine was not.
#include "kernel/k_dpc_posix.h"
#include "kernel/k_sync_posix.h"
#include "kernel/k_thread_posix.h"
#include "platform/posix/xsync.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
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
        printf("  FAIL %s (got 0x%x, want 0x%x)\n", what.c_str(), got, want);
        g_failures++;
    }
    fflush(stdout);
}

// ---- what the DPCs and timers report back ----------------------------------------------------------------

struct DpcReport {
    std::atomic<uint32_t> ran{0};
    std::atomic<uint32_t> irql{0xFF};
    std::atomic<uint32_t> dpc_active{0xFF};
    std::atomic<uint32_t> thread{0};
    std::atomic<uint32_t> arg1{0};
    std::atomic<uint32_t> context{0};
    std::atomic<uint32_t> calls{0};
};

static void __attribute__((stdcall)) dpc_routine(XKDPC *dpc, void *deferred_context, void *arg1, void *arg2)
{
    DpcReport *r = (DpcReport *)deferred_context;
    if (!r) return;
    r->calls.store(r->calls.load() + 1);
    r->irql.store(xk_current_irql());
    r->dpc_active.store(teb()->dpc_active);
    r->thread.store(teb()->thread);
    r->arg1.store((uint32_t)(uintptr_t)arg1);
    r->context.store((uint32_t)(uintptr_t)dpc->DeferredContext);
    r->ran.store(1);
}

// Wait for a flag with a deadline, so a broken DPC worker fails the test instead of hanging it.
static bool wait_for(const std::atomic<uint32_t> &flag, int timeout_ms)
{
    for (int i = 0; i < timeout_ms * 10; i++) {
        if (flag.load()) return true;
        usleep(100);
    }
    return flag.load() != 0;
}

// Wait for a *count* to reach a number, which is what a second run needs: the flag is already set by then.
static bool wait_for_count(const std::atomic<uint32_t> &counter, uint32_t at_least, int timeout_ms)
{
    for (int i = 0; i < timeout_ms * 10; i++) {
        if (counter.load() >= at_least) return true;
        usleep(100);
    }
    return counter.load() >= at_least;
}

// ---- the DPC object ---------------------------------------------------------------------------------------

static void test_dpc_object()
{
    printf("\nthe DPC object\n");

    XKDPC dpc;
    memset(&dpc, 0, sizeof dpc);
    DpcReport report;
    KeInitializeDpc(&dpc, dpc_routine, &report);

    check_u32((uint32_t)dpc.Type, XOBJ_DPC, "KeInitializeDpc gave it the DPC object type");
    check((void *)dpc.DeferredRoutine == (void *)dpc_routine, "and kept the routine");
    check(dpc.DeferredContext == &report, "and the context");
    check_u32(dpc.Inserted, 0, "and it is not queued");
    check(dpc.DpcListEntry.Flink == nullptr && dpc.DpcListEntry.Blink == nullptr,
          "with an empty list entry, as an unqueued DPC has");
    check_u32((uint32_t)sizeof(XKDPC), 28, "the KDPC is 28 bytes, the Xbox's size");
    check_u32((uint32_t)sizeof(XKTIMER), 40, "and the KTIMER is 40 bytes");
}

static void test_queue()
{
    printf("\nqueueing a DPC\n");

    XKDPC dpc;
    memset(&dpc, 0, sizeof dpc);
    DpcReport report;
    KeInitializeDpc(&dpc, dpc_routine, &report);

    check_u32(KeInsertQueueDpc(&dpc, (void *)0x1234, (void *)0x5678), 1, "KeInsertQueueDpc queued it");
    check_u32(dpc.Inserted, 1, "and the DPC knows it is queued");
    check_u32(KeInsertQueueDpc(&dpc, (void *)0x9, (void *)0x9), 0,
              "queueing it again is refused, as on the Xbox");
    check_u32(dpc.SystemArgument1 == (void *)0x1234, 1, "the arguments stayed as the first call set them");
    check_u32(dpc.SystemArgument2 == (void *)0x5678, 1, "both of them");

    check(wait_for(report.ran, 2000), "the DPC ran");
    check_u32(report.calls.load(), 1, "exactly once");
    check_u32(report.irql.load(), IRQL_DISPATCH, "at DISPATCH_LEVEL");
    check_u32(report.dpc_active.load(), 1, "with KPRCB.DpcRoutineActive set while it ran");
    check(report.thread.load() != 0, "on a thread with a KTHREAD of its own");
    check(report.thread.load() != (uint32_t)(uintptr_t)teb()->thread, "and not the calling thread's");
    check_u32(report.context.load(), (uint32_t)(uintptr_t)&report, "the DPC got its context back");
    check_u32(report.arg1.load(), 0x1234, "and SystemArgument1 as it was queued with");

    // A DPC that was queued and has run can be queued again.
    check_u32(KeInsertQueueDpc(&dpc, (void *)0x2222, (void *)0x3333), 1, "after it ran, it can be queued again");
    check(wait_for_count(report.calls, 2, 2000), "and it runs again");
}

static void test_remove()
{
    printf("\nremoving a queued DPC\n");

    XKDPC dpc;
    memset(&dpc, 0, sizeof dpc);
    DpcReport report;
    KeInitializeDpc(&dpc, dpc_routine, &report);

    check_u32(KeRemoveQueueDpc(&dpc), 0, "removing a DPC that is not queued says so");
    KeInsertQueueDpc(&dpc, nullptr, nullptr);
    check_u32(KeRemoveQueueDpc(&dpc), 1, "removing a queued DPC reports that it removed it");
    check_u32(dpc.Inserted, 0, "and the DPC knows it is no longer queued");
    usleep(50 * 1000);
    check_u32(report.calls.load(), 0, "and it never ran");
}

static void test_timer()
{
    printf("\na kernel timer\n");

    XKTIMER timer;
    memset(&timer, 0, sizeof timer);
    XKDPC dpc;
    memset(&dpc, 0, sizeof dpc);
    DpcReport report;
    KeInitializeDpc(&dpc, dpc_routine, &report);

    KeInitializeTimerEx(&timer, 1); // 1 = the 10ms resolution
    check_u32(timer.Header.Type, XOBJ_TIMER_NOTIFICATION + 1,
              "KeInitializeTimerEx gave it the timer notification type");
    check_u32(timer.Header.Size, (uint32_t)(sizeof(XKTIMER) / 4), "and the size the console's would have");
    check_u32(timer.Header.Inserted, 0, "and it is not armed");
    check(timer.Header.WaitListHead.Flink == &timer.Header.WaitListHead &&
              timer.Header.WaitListHead.Blink == &timer.Header.WaitListHead,
          "with an empty wait list, as an armed-timer object has before anyone waits on it");

    check_u32(KeSetTimer(&timer, -200000, &dpc), 0, "arming a timer reports that none was armed before");
    check_u32(timer.Header.Inserted, 1, "and the timer knows it is armed");
    check_u32(kdpc_timer_count(), 1, "one timer is armed");
    check(timer.Dpc == &dpc, "with the DPC it was given");

    check(wait_for(report.ran, 3000), "the timer fired and the DPC ran");
    check_u32(report.calls.load(), 1, "once");
    check(report.arg1.load() != 0, "the DPC got the 100ns time in SystemArgument1");
    check_u32(kdpc_timer_count(), 0, "and the one-shot timer is no longer armed");
}

static void test_timer_cancel()
{
    printf("\ncancelling a timer\n");

    XKTIMER timer;
    memset(&timer, 0, sizeof timer);
    XKDPC dpc;
    memset(&dpc, 0, sizeof dpc);
    DpcReport report;
    KeInitializeDpc(&dpc, dpc_routine, &report);
    KeInitializeTimerEx(&timer, 2);

    KeSetTimer(&timer, -500000, &dpc); // half a second: long enough to cancel
    check_u32(KeSetTimer(&timer, -500000, &dpc), 1, "arming an armed timer reports the previous state");
    check_u32(KeCancelTimer(&timer), 1, "cancelling it reports that it was armed");
    check_u32(KeCancelTimer(&timer), 0, "cancelling it again reports that it was not");
    check_u32(kdpc_timer_count(), 0, "no timer is armed");

    usleep(700 * 1000); // past the time the first arming would have fired
    check_u32(report.calls.load(), 0, "and the DPC never ran");
}

static void test_absolute_timer()
{
    printf("\nan absolute timer\n");

    XKTIMER timer;
    memset(&timer, 0, sizeof timer);
    XKDPC dpc;
    memset(&dpc, 0, sizeof dpc);
    DpcReport report;
    KeInitializeDpc(&dpc, dpc_routine, &report);
    KeInitializeTimerEx(&timer, 3);

    // An absolute due time in the past fires at once: the NT convention is that a positive DueTime is
    // absolute, and the console's clock is the 1601 epoch, which is behind any host clock.
    const int64_t past = kdpc_now_100ns() - 1000000LL;
    KeSetTimer(&timer, past, &dpc);
    check(wait_for(report.ran, 3000), "a timer due in the past fires straight away");
    check_u32(report.calls.load(), 1, "once");
}

static void test_timer_signals_the_object()
{
    printf("\nwaiting on a timer\n");

    // A timer is a notification object: when it expires the object is signalled, so a wait on it returns.
    // That is what makes NtWaitForSingleObject on a timer work, and it is the reason the expiry path calls
    // xk_signal_object at all.
    XKTIMER timer;
    memset(&timer, 0, sizeof timer);
    KeInitializeTimerEx(&timer, 1);
    KeSetTimer(&timer, -100000, nullptr); // 10 ms, no DPC

    XLargeInt timeout;
    timeout.QuadPart = -30000000LL; // 3 seconds
    const NTSTATUS s = xk_wait_for_object(&timer.Header, 0, &timeout);
    check(s == 0, "waiting on the timer returned success (it was signalled)");
}

int main()
{
    printf("BLACK loader -- DPCs and kernel timers\n");

    if (!kdpc_init()) {
        check(false, "the DPC worker started");
        printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    check(true, "the DPC worker started");

    test_dpc_object();
    test_queue();
    test_remove();
    test_timer();
    test_timer_cancel();
    test_absolute_timer();
    test_timer_signals_the_object();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
