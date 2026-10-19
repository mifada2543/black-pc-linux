#include "kernel/k_dpc_posix.h"

#include "kernel/k_thread_posix.h" // kthread_create, kthread_set_dpc_active, kthread_set_irql
#include "platform/posix/log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <deque>
#include <vector>

namespace {

// A native pthread mutex, not a std::mutex: the waits below are pthread_cond_wait, which needs a
// pthread_mutex_t. Everything else in the loader uses std::mutex and this is the one place the two have to
// meet -- worth saying out loud, because "tidying" it into std::mutex breaks the timers silently.
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_wake;
bool g_wake_ready = false;
bool g_started = false;

bool g_atfork_registered = false;

std::deque<XKDPC *> g_queue;
std::vector<XKTIMER *> g_timers;

struct Guard {
    Guard() { pthread_mutex_lock(&g_lock); }
    ~Guard() { pthread_mutex_unlock(&g_lock); }
};

// Wake the worker. Called with the lock released, so the worker cannot miss it between checking the predicate
// and going to sleep -- which is the whole reason the predicate is checked under the lock on every pass.
void poke()
{
    if (g_wake_ready) pthread_cond_signal(&g_wake);
}

void remove_timer_locked(XKTIMER *t)
{
    g_timers.erase(std::remove(g_timers.begin(), g_timers.end(), t), g_timers.end());
    t->Header.Inserted = 0;
}

// Queue a DPC unless it is already queued, which is the Xbox's rule: one DPC object is in the queue or it is
// not, and KeInsertQueueDpc says so rather than queueing it twice.
bool insert_locked(XKDPC *dpc, void *arg1, void *arg2)
{
    if (dpc->Inserted) return false;
    dpc->Inserted = 1;
    dpc->SystemArgument1 = arg1;
    dpc->SystemArgument2 = arg2;
    g_queue.push_back(dpc);
    return true;
}

void __attribute__((stdcall)) dpc_worker(void *)
{
    // The worker's own Xbox thread: its fs:[0x20] is this thread's KPRCB, so a DPC that calls back into game
    // code finds the state it expects, and its IRQL field is ours to raise and lower around each DPC.
    for (;;) {
        std::vector<XKDPC *> run;
        int wait_ms = -1; // -1 means "nothing armed", so sleep until poked

        {
            Guard g;
            const int64_t now = kdpc_now_100ns();

            for (size_t i = 0; i < g_timers.size();) {
                XKTIMER *t = g_timers[i];
                if (t->DueTime > now) {
                    i++;
                    continue;
                }
                if (t->Period > 0) {
                    // A periodic timer re-arms itself. If the host was busy for several periods, the re-arm is
                    // pushed to now + one period rather than running a burst of catch-up DPCs -- which is what
                    // a game with a 16 ms timer would otherwise see after a stall.
                    t->DueTime += (int64_t)t->Period * 10000;
                    if (t->DueTime <= now) t->DueTime = now + (int64_t)t->Period * 10000;
                    i++;
                } else {
                    g_timers.erase(g_timers.begin() + i);
                    t->Header.Inserted = 0;
                }
                logf("dpc: timer expired (Dpc %p)", (void *)t->Dpc);
                xk_signal_object(&t->Header);
                if (t->Dpc)
                    insert_locked(t->Dpc, (void *)(uintptr_t)(uint32_t)now, (void *)(uintptr_t)(uint32_t)(now >> 32));
            }

            for (XKTIMER *t : g_timers) {
                int64_t ms = (t->DueTime - now + 9999) / 10000;
                if (ms < 0) ms = 0;
                if (wait_ms < 0 || ms < wait_ms) wait_ms = (int)ms;
            }
            while (!g_queue.empty()) {
                XKDPC *d = g_queue.front();
                g_queue.pop_front();
                d->Inserted = 0;
                run.push_back(d);
            }
        }

        if (!run.empty()) logf("dpc: running %u deferred procedure(s)", (uint32_t)run.size());
        for (XKDPC *d : run) {
            // DISPATCH_LEVEL around the call, and DpcRoutineActive set while it runs: the game reads that flag
            // at fs:[0x58], and a few of its routines check it before touching shared state.
            // The IRQL goes up in the thread's own block as well as in xsync's: game code reads KPCR.Irql at
            // fs:[0x24] from the block, and a DPC that found zero there -- while the kernel thought it was at
            // DISPATCH_LEVEL -- is exactly the sort of disagreement that costs an afternoon. xk_raise_irql
            // only knows the xsync side, so the publication to fs:[0x24] is made here.
            const uint8_t old = xk_raise_irql(IRQL_DISPATCH);
            kthread_set_irql(IRQL_DISPATCH);
            kthread_set_dpc_active(true);
            if (d->DeferredRoutine)
                d->DeferredRoutine(d, d->DeferredContext, d->SystemArgument1, d->SystemArgument2);
            kthread_set_dpc_active(false);
            xk_lower_irql(old);
            kthread_set_irql(old);
        }

        pthread_mutex_lock(&g_lock);
        if (g_queue.empty() && g_timers.empty()) {
            pthread_cond_wait(&g_wake, &g_lock); // nothing armed: sleep until there is
        } else if (wait_ms <= 0) {
            // Either a DPC just ran or a timer is due right now: go round again without sleeping, so neither
            // can be missed.
        } else {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += (long)wait_ms * 1000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_nsec -= 1000000000L;
                deadline.tv_sec++;
            }
            pthread_cond_timedwait(&g_wake, &g_lock, &deadline);
        }
        pthread_mutex_unlock(&g_lock);
    }
}

} // namespace

int64_t kdpc_now_100ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 10000000LL + ts.tv_nsec / 100;
}

uint32_t kdpc_timer_count()
{
    Guard g;
    return (uint32_t)g_timers.size();
}

uint32_t kdpc_queue_count()
{
    Guard g;
    return (uint32_t)g_queue.size();
}

// A child of fork() has this process's memory but none of its threads, and a flag saying the worker is
// running is one of the things it inherits. That is how a forked child ends up waiting for a timer that
// nothing will ever fire: the test forks, the child arms a timer from game code, and the boot thread spins
// until the run times out with nothing in the log. So the child handler puts the worker's state back to
// "not started" -- including re-initialising the mutex and the condition variable, which may have been held
// by a thread that does not exist in the child any more.
static void dpc_after_fork_in_child()
{
    g_started = false;
    g_wake_ready = false;
    pthread_mutex_init(&g_lock, nullptr);
    pthread_cond_init(&g_wake, nullptr);
    // Armed timers and queued DPCs are left alone: they are ordinary data, their targets are still mapped in
    // the child, and the worker started by the next kdpc_init() will pick them up.
}

bool kdpc_init()
{
    {
        Guard g;
        if (!g_atfork_registered) {
            g_atfork_registered = true;
            pthread_atfork(nullptr, nullptr, dpc_after_fork_in_child);
        }
        if (g_started) return true;
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_cond_init(&g_wake, &attr);
        pthread_condattr_destroy(&attr);
        g_wake_ready = true;
    }
    // Created through the thread layer, with a high base priority: a DPC that runs late is a dropped frame, and
    // on this machine that is the difference between the game running and not. The console had a real clock
    // interrupt for it; the closest thing here is a thread the host scheduler prefers.
    uint32_t handle = 0, tid = 0;
    const NTSTATUS s = kthread_create(&handle, &tid, 0, 0, (uint32_t)(uintptr_t)&dpc_worker, 0, 0, false,
                                      "kernel-dpc", false, /*game_thread=*/false);
    if (s != KT_SUCCESS) {
        logc("dpc: the DPC thread could not start (%08x) -- timers will never fire", (uint32_t)s);
        return false;
    }
    XboxThread *t = kthread_from_handle(handle);
    if (t) t->base_priority = 15;
    Guard g;
    g_started = true;
    logc("dpc: worker thread %u started (handle 0x%08x)", tid, handle);
    return true;
}

// ----------------------------------------------------------------------------------------------------------
// exports
// ----------------------------------------------------------------------------------------------------------

extern "C" void __attribute__((stdcall)) KeInitializeDpc(
    XKDPC *Dpc, void (__attribute__((stdcall)) *DeferredRoutine)(XKDPC *, void *, void *, void *),
    void *DeferredContext)
{
    if (!Dpc) return;
    Dpc->Type = (int16_t)XOBJ_DPC;
    Dpc->Inserted = 0;
    Dpc->Padding = 0;
    Dpc->DpcListEntry.Flink = nullptr;
    Dpc->DpcListEntry.Blink = nullptr;
    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
    Dpc->SystemArgument1 = nullptr;
    Dpc->SystemArgument2 = nullptr;
}

extern "C" uint8_t __attribute__((stdcall)) KeInsertQueueDpc(XKDPC *Dpc, void *SystemArgument1,
                                                             void *SystemArgument2)
{
    if (!Dpc) return 0;
    bool ok;
    {
        Guard g;
        ok = insert_locked(Dpc, SystemArgument1, SystemArgument2);
    }
    poke();
    return ok ? 1 : 0;
}

extern "C" uint8_t __attribute__((stdcall)) KeRemoveQueueDpc(XKDPC *Dpc)
{
    if (!Dpc) return 0;
    Guard g;
    if (!Dpc->Inserted) return 0;
    g_queue.erase(std::remove(g_queue.begin(), g_queue.end(), Dpc), g_queue.end());
    Dpc->Inserted = 0;
    return 1;
}

extern "C" void __attribute__((stdcall)) KeInitializeTimerEx(XKTIMER *Timer, uint32_t Type)
{
    if (!Timer) return;
    // The notification type is what makes NtWaitForSingleObject on a timer work: xsync dispatches on it, and a
    // timer given the wrong type would be waited on as though it were something else.
    Timer->Header.Type = (uint8_t)(XOBJ_TIMER_NOTIFICATION + Type);
    Timer->Header.Absolute = 0;
    Timer->Header.Size = (uint8_t)(sizeof(XKTIMER) / 4);
    Timer->Header.Inserted = 0;
    Timer->Header.SignalState = 0;
    Timer->Header.WaitListHead.Flink = &Timer->Header.WaitListHead;
    Timer->Header.WaitListHead.Blink = &Timer->Header.WaitListHead;
    Timer->DueTime = 0;
    Timer->TimerListEntry.Flink = nullptr;
    Timer->TimerListEntry.Blink = nullptr;
    Timer->Dpc = nullptr;
    Timer->Period = 0;
}

extern "C" uint8_t __attribute__((stdcall)) KeSetTimer(XKTIMER *Timer, int64_t DueTime, XKDPC *Dpc)
{
    if (!Timer) return 0;
    const int64_t now = kdpc_now_100ns();
    bool was;
    {
        Guard g;
        was = Timer->Header.Inserted != 0;
        if (was) remove_timer_locked(Timer);
        Timer->Header.SignalState = 0;
        Timer->Dpc = Dpc;
        Timer->Period = 0;
        // Negative is relative, which is the NT convention and the one the game uses: KeSetTimer is called both
        // ways and the sign is the only thing that says which.
        Timer->DueTime = DueTime < 0 ? now - DueTime : DueTime;
        Timer->Header.Inserted = 1;
        g_timers.push_back(Timer);
    }
    poke();
    return was ? 1 : 0;
}

extern "C" uint8_t __attribute__((stdcall)) KeCancelTimer(XKTIMER *Timer)
{
    if (!Timer) return 0;
    bool was;
    {
        Guard g;
        was = Timer->Header.Inserted != 0;
        if (was) remove_timer_locked(Timer);
    }
    poke();
    return was ? 1 : 0;
}
