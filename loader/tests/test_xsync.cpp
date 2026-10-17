// Xbox dispatcher objects, critical sections and IRQL emulation.
//
// These are the parts of the kernel HLE where a subtle mistake shows up as a rare hang rather than a
// crash, so the tests here are about ordering and timing as much as return values: many waiters on one
// object, signals that land before a waiter starts, timeouts that must actually expire, and an
// auto-reset event that must release exactly one waiter.
#include "platform/posix/xsync.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

static int g_failures, g_checks;
static void check(bool cond, const std::string &what)
{
    g_checks++;
    if (cond) printf("  ok   %s\n", what.c_str());
    else { g_failures++; printf("  FAIL %s\n", what.c_str()); }
}

// For the counts that a concurrency test produces: reporting the number turns "it failed" into
// "it was 2, not 3", which is the difference between a guess and a diagnosis.
static void check_eq(int got, int want, const std::string &what)
{
    g_checks++;
    if (got == want) printf("  ok   %s\n", what.c_str());
    else { g_failures++; printf("  FAIL %s (got %d, want %d)\n", what.c_str(), got, want); }
}

static XLargeInt rel_ms(int64_t ms)
{
    XLargeInt t;
    t.QuadPart = -(ms * 10000); // negative = relative, in 100 ns units
    return t;
}
static XLargeInt forever()
{
    XLargeInt t;
    t.QuadPart = -1;
    return t;
}
// Waiting takes a pointer, and these are locals that must outlive the call.
static XLargeInt *keep(XLargeInt *t) { return t; }

static XKeEvent make_event(uint8_t type, int32_t state = 0)
{
    XKeEvent e{};
    e.Header.Type = type;
    e.Header.SignalState = state;
    e.Header.Size = (uint8_t)(sizeof(XKeEvent) / 4);
    return e;
}

static void spin_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---------------------------------------------------------------------------

static void test_timeouts()
{
    printf("\ntimeout conversion\n");
    check(xk_timeout_ms(nullptr) == 0xFFFFFFFFu, "a null timeout means infinite");
    XLargeInt inf0 = forever();
    check(xk_timeout_ms(&inf0) == 0xFFFFFFFFu, "-1 (100 ns) means infinite");
    XLargeInt r = rel_ms(500);
    check(xk_timeout_ms(&r) == 500, "a relative timeout converts 100 ns units to milliseconds");
    XLargeInt r2 = rel_ms(1);
    check(xk_timeout_ms(&r2) == 1, "1 ms rounds up to 1, not 0");
}

static void test_events()
{
    printf("\nevents in Xbox memory\n");

    // A synchronization (auto-reset) event that is already signalled is consumed by one waiter and
    // then goes back to unsignalled, which is the whole difference from a notification event.
    XKeEvent auto_ev = make_event(XOBJ_EVENT_SYNCHRONIZATION, 1);
    XLargeInt inf = forever();
    check(XK_NT_OK(xk_wait_for_object(&auto_ev.Header, 0, keep(&inf))), "wait on an already-signalled auto-reset event");
    check(auto_ev.Header.SignalState == 0, "the signal was consumed");

    XLargeInt short_t = rel_ms(30);
    check(xk_wait_for_object(&auto_ev.Header, 0, &short_t) == XK_TIMEOUT, "waiting on it again times out");

    // Exactly one waiter out of many may take a single auto-reset signal. The waiters must all be
    // inside the wait before the signal lands, otherwise a late thread legitimately consumes it and
    // the test measures thread startup instead of the auto-reset rule.
    XKeEvent race = make_event(XOBJ_EVENT_SYNCHRONIZATION, 0);
    std::atomic<int> passed{0};
    std::atomic<int> ready{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; i++)
        ts.emplace_back([&] {
            ready++;
            XLargeInt t = rel_ms(800);
            // == XK_SUCCESS, not XK_NT_OK: STATUS_TIMEOUT is 0x102, non-negative, so it passes the
            // NT_SUCCESS test. Counting timed-out waiters as released ones made this check unable to
            // pass either way, and hid a real lost-wakeup bug behind a permanent FAIL.
            if (xk_wait_for_object(&race.Header, 0, &t) == XK_SUCCESS) passed++;
        });
    while (ready.load() < 8) std::this_thread::yield();
    spin_ms(60); // let every waiter park on the condition variable
    xk_set_event(&race, 1, 0);
    for (auto &t : ts) t.join();
    check_eq(passed.load(), 1, "one signal released exactly one of eight waiters");

    // A notification event stays signalled: every waiter that arrives afterwards passes.
    XKeEvent note = make_event(XOBJ_EVENT_NOTIFICATION, 1);
    int note_passed = 0;
    for (int i = 0; i < 5; i++) {
        XLargeInt t = rel_ms(100);
        if (xk_wait_for_object(&note.Header, 0, &t) == XK_SUCCESS) note_passed++; // see above
    }
    check(note_passed == 5, "a notification event releases every waiter and stays signalled");
    check(note.Header.SignalState == 1, "and it is still signalled afterwards");

    // A signal that arrived before the waiter started must not release it. This is what the
    // generation counter is for: without it a timed wait would return immediately and wrongly.
    XKeEvent before = make_event(XOBJ_EVENT_NOTIFICATION, 0);
    xk_set_event(&before, 1, 0);
    spin_ms(20);
    XLargeInt t2 = rel_ms(40);
    check(xk_wait_for_object(&before.Header, 0, &t2) == XK_SUCCESS,
          "a notification event signalled before the wait is still seen (the object stays signalled)");

    XKeEvent sync_before = make_event(XOBJ_EVENT_SYNCHRONIZATION, 0);
    xk_set_event(&sync_before, 1, 0);
    spin_ms(20);
    XLargeInt t3 = rel_ms(40);
    check(xk_wait_for_object(&sync_before.Header, 0, &t3) == XK_SUCCESS,
          "an auto-reset event signalled before the wait is still seen (nothing consumed it)");
}

static void test_semaphore_objects()
{
    printf("\nsemaphores in Xbox memory\n");
    XKSemaphore sem{};
    sem.Header.Type = XOBJ_SEMAPHORE;
    sem.Header.SignalState = 2;
    sem.Limit = 5;
    XLargeInt inf = forever();

    check(XK_NT_OK(xk_wait_for_object(&sem.Header, 0, &inf)), "first wait takes a token");
    check(sem.Header.SignalState == 1, "the count went down by one");
    check(XK_NT_OK(xk_wait_for_object(&sem.Header, 0, &inf)), "second wait takes a token");
    XLargeInt t = rel_ms(30);
    check(xk_wait_for_object(&sem.Header, 0, &t) == XK_TIMEOUT, "a third wait finds it empty and times out");

    // Four waiters, three tokens released: exactly three pass, the fourth must time out. The waiters
    // are parked before the release, so the count they see is the one that was released.
    XKSemaphore s2{};
    s2.Header.Type = XOBJ_SEMAPHORE;
    s2.Limit = 10;
    std::atomic<int> got{0};
    std::atomic<int> ready{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; i++)
        ts.emplace_back([&] {
            ready++;
            XLargeInt tt = rel_ms(600);
            if (xk_wait_for_object(&s2.Header, 0, &tt) == XK_SUCCESS) got++; // see above
        });
    while (ready.load() < 4) std::this_thread::yield();
    spin_ms(60);
    xk_release_object(&s2.Header, 3); // KeReleaseSemaphore, not KeSetEvent
    for (auto &x : ts) x.join();
    check_eq(got.load(), 3, "three tokens released three of four waiters, the fourth timed out");
    check_eq(s2.Header.SignalState, 0, "and the semaphore ended empty");
    XLargeInt t2 = rel_ms(20);
    check(xk_wait_for_object(&s2.Header, 0, &t2) == XK_TIMEOUT, "a further wait still times out");
}

static void test_critical_sections()
{
    printf("\ncritical sections\n");
    XRtlCriticalSection cs;
    xk_initialize_critical_section(&cs);
    check(cs.LockCount == -1, "LockCount starts at -1, as the Xbox layout requires");
    check(cs.Synchronization.Type == XOBJ_EVENT_SYNCHRONIZATION, "the header is a synchronization event");

    xk_enter_critical_section(&cs);
    check(cs.OwningThread != nullptr && cs.RecursionCount == 1 && cs.LockCount == 0, "first enter owns it");
    xk_enter_critical_section(&cs);
    check(cs.RecursionCount == 2, "re-entering the same section recurses");
    xk_leave_critical_section(&cs);
    check(cs.RecursionCount == 1, "one leave drops the recursion but keeps ownership");
    xk_leave_critical_section(&cs);
    check(cs.OwningThread == nullptr && cs.LockCount == -1, "the last leave releases it back to -1");

    // Mutual exclusion across threads, with a counter that would race if the lock were not working.
    XRtlCriticalSection shared;
    xk_initialize_critical_section(&shared);
    int counter = 0;
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; i++)
        ts.emplace_back([&] {
            for (int k = 0; k < 2000; k++) {
                xk_enter_critical_section(&shared);
                int v = counter;
                // A yield here widens any window the lock failed to close.
                if ((k & 255) == 0) std::this_thread::yield();
                counter = v + 1;
                xk_leave_critical_section(&shared);
            }
        });
    for (auto &t : ts) t.join();
    check(counter == 8 * 2000, "8 threads x 2000 increments under the section, with no lost updates");
    check(shared.LockCount == -1, "the section is back to its initial state afterwards");
}

static void test_irql()
{
    printf("\nIRQL\n");
    xk_init();
    check(xk_current_irql() == IRQL_PASSIVE, "a fresh thread starts at PASSIVE");

    uint8_t old = xk_raise_irql(IRQL_APC);
    check(old == IRQL_PASSIVE && xk_current_irql() == IRQL_APC, "raising to APC does not take the dispatch lock");
    check(!xk_holds_dispatch_lock(), "still no dispatch lock at APC");
    xk_lower_irql(old);
    check(xk_current_irql() == IRQL_PASSIVE, "lowering back restores it");

    xk_raise_irql(IRQL_DISPATCH);
    check(xk_current_irql() == IRQL_DISPATCH && xk_holds_dispatch_lock(), "DISPATCH takes the dispatch lock");

    // KeSynchronizeExecution must not deadlock when called from a thread that is already at DISPATCH.
    struct Ctx { int ran; } ctx{0};
    xk_synchronize_execution([](void *p) { ((Ctx *)p)->ran = 1; }, &ctx);
    check(ctx.ran == 1, "KeSynchronizeExecution runs the callback");
    check(xk_current_irql() == IRQL_DISPATCH, "and leaves the IRQL as it found it");

    xk_lower_irql(IRQL_PASSIVE);
    check(!xk_holds_dispatch_lock(), "lowering below DISPATCH releases the lock");

    // Only one thread may be at DISPATCH at a time. Count how many ever are, concurrently.
    std::atomic<int> inside{0};
    std::atomic<int> max_inside{0};
    std::atomic<bool> overlap{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < 6; i++)
        ts.emplace_back([&] {
            for (int k = 0; k < 300; k++) {
                xk_raise_irql(IRQL_DISPATCH);
                int n = ++inside;
                int prev = max_inside.load();
                while (n > prev && !max_inside.compare_exchange_weak(prev, n)) {
                }
                if (n > 1) overlap = true;
                --inside;
                std::this_thread::yield();
                xk_lower_irql(IRQL_PASSIVE);
            }
        });
    for (auto &t : ts) t.join();
    check(!overlap, "no two threads were ever at DISPATCH_LEVEL at the same time");
    check(max_inside == 1, "the dispatch lock serialised all six threads");
    check(!xk_holds_dispatch_lock(), "the dispatch lock was not stranded");

    xk_stall_us(2000);
    check(true, "KeStallExecutionProcessor returns");
}

static void test_handle_objects()
{
    printf("\nhandle-based objects\n");
    XHandle ev = XINVALID_HANDLE;
    check(XK_NT_OK(xk_create_event(&ev, 1, 0)), "created a synchronization event");
    XLargeInt t = rel_ms(30);
    check(xk_wait_handle(ev, 0, &t) == XK_TIMEOUT, "waiting on it times out");

    int32_t prev = -1;
    std::thread setter([&] {
        spin_ms(30);
        xk_set_event_handle(ev, &prev);
    });
    XLargeInt inf = forever();
    check(XK_NT_OK(xk_wait_handle(ev, 0, &inf)), "a thread's set released the wait");
    setter.join();
    check(prev == 0, "the previous state was reported");

    XHandle sem = XINVALID_HANDLE;
    check(XK_NT_OK(xk_create_semaphore(&sem, 0, 3)), "created a semaphore, max 3");
    int32_t pc = -1;
    check(XK_NT_OK(xk_release_semaphore(sem, 2, &pc)) && pc == 0, "released 2, previous count 0");
    XLargeInt t2 = rel_ms(30);
    check(XK_NT_OK(xk_wait_handle(sem, 0, &t2)), "took one");
    check(XK_NT_OK(xk_wait_handle(sem, 0, &t2)), "took the other");
    check(xk_wait_handle(sem, 0, &t2) == XK_TIMEOUT, "the semaphore is empty again");
    check(xk_release_semaphore(sem, 5, nullptr) == XK_UNSUCCESSFUL, "releasing past MaximumCount is refused");

    check(XK_NT_OK(xk_close(ev)) && XK_NT_OK(xk_close(sem)), "closed both");
    check(xk_close(ev) == XK_INVALID_HANDLE, "closing twice is INVALID_HANDLE");
    check(xk_wait_handle(sem, 0, nullptr) == XK_INVALID_HANDLE, "waiting on a closed handle is INVALID_HANDLE");
}

static void test_handle_reuse()
{
    printf("\nhandle reuse\n");
    XHandle a = XINVALID_HANDLE, b = XINVALID_HANDLE;
    xk_create_event(&a, 1, 0);
    xk_close(a);
    xk_create_event(&b, 1, 1); // created already signalled
    check(a == b, "a freed handle slot is reused");
    XLargeInt inf = forever();
    check(XK_NT_OK(xk_wait_handle(b, 0, &inf)), "and the new object works");
    xk_close(b);
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("BLACK loader -- sync, critical sections and IRQL tests\n");
    test_timeouts();
    test_events();
    test_semaphore_objects();
    test_critical_sections();
    test_irql();
    test_handle_objects();
    test_handle_reuse();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}