#include "platform/posix/xsync.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace {

// One wake primitive per object, holding everything that object owns: the lock, the condition
// variable its waiters sleep on, and the generation counter.
//
// The per-object mutex is the *only* lock an object has. There is deliberately no global state lock.
// A global one is what k_sync.cpp uses on Windows and it looks tidier, but it is the wrong shape for
// POSIX: a signal took the global lock and notified, while the waiter held a different lock (the
// condition variable's) and slept -- so a signal landing between the waiter's predicate check and its
// wait was simply lost, and the waiter only came back on its own timeout. When the lock and the
// condition variable are the same lock, "check the state" and "sleep on the state" cannot come apart.
//
// A struct rather than std::pair<mutex, condition_variable>: pair cannot be built from non-movable
// members, and try_emplace value-initialises this in place.
struct Wake {
    std::mutex m;
    std::condition_variable cv;
    uint64_t generation = 0; // signals this object has seen; read and written under m
};

// The table, guarded separately and only for its own structure. Entries are never erased and
// std::map nodes do not move, so a pointer handed out here stays valid for the life of the process --
// which is what lets a waiter go on holding w.m after this lock has been dropped.
std::map<const void *, Wake> g_wake;
std::mutex g_wake_mutex;

// Look up or create the wake primitive. The signal path creates too, rather than assuming a waiter got
// there first: an event can be set before anyone ever waits on it, and that signal has to be visible
// to a waiter that arrives later. The table lock is released before returning, so w.m is never taken
// while g_wake_mutex is held.
Wake &wake_of(const void *obj)
{
    std::lock_guard<std::mutex> g(g_wake_mutex);
    return g_wake.try_emplace(obj).first->second;
}

// Caller holds w.m. KeSetEvent: set the state and record that a signal happened. Every signal path
// funnels through here, so "the state changed" and "the waiters were told" cannot come apart.
void signal_locked(XDispatcherHeader *h, Wake &w)
{
    h->SignalState = 1;
    w.generation++;
    w.cv.notify_all(); // a condition variable cannot hand a signal to one waiter alone, and it does
                       // not need to: try_consume() below is what limits it to one.
}

// Caller holds w.m. KeReleaseSemaphore on an object living in Xbox memory: *add* tokens rather than
// setting the state, because releasing three at once is not the same thing as signalling.
void release_locked(XDispatcherHeader *h, Wake &w, int32_t count)
{
    h->SignalState += count;
    w.generation++;
    w.cv.notify_all();
}

// Handle-based objects live in their own small arena. A real OS would have its own table; a vector
// with stable indices is enough and cannot leak a pthread object.
struct HandleObject {
    enum Kind { Event, Semaphore } kind = Event;
    bool manual_reset = false; // NotificationEvent is manual reset, SynchronizationEvent auto
    int32_t count = 0;
    int32_t max = 0;
    std::mutex m;
    std::condition_variable cv;
    bool live = false;
};
std::deque<HandleObject> g_handles;
std::mutex g_handle_mutex; // the arena table. It guards the slots' structure only -- each object's own
                           // state has its own lock, so a handle lock is never held across a wait.

// IRQL emulation. The dispatch lock is held across the whole time any thread is at DISPATCH_LEVEL or
// above, which is what gives the game the exclusion it expects from a single-CPU machine.
std::mutex g_dispatch_mutex;
std::atomic<bool> g_holds_dispatch{false};
thread_local uint8_t t_irql = IRQL_PASSIVE;

// ---- IRQL ----

uint64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

// ---- try_consume, the heart of the object model ----

// Caller holds the object's own wake mutex. Returns true if this waiter may proceed.
bool try_consume(XDispatcherHeader *h)
{
    switch (h->Type) {
    case XOBJ_EVENT_NOTIFICATION:
    case XOBJ_TIMER_NOTIFICATION:
        // Persistent: the object stays signalled, so every waiter passes without consuming it.
        return h->SignalState > 0;
    case XOBJ_EVENT_SYNCHRONIZATION:
    case XOBJ_TIMER_SYNCHRONIZATION:
        if (h->SignalState > 0) {
            h->SignalState = 0; // auto-reset: exactly one waiter gets it
            return true;
        }
        return false;
    case XOBJ_SEMAPHORE:
    case XOBJ_MUTANT:
        if (h->SignalState > 0) {
            h->SignalState--;
            return true;
        }
        return false;
    default:
        return h->SignalState > 0;
    }
}

bool is_notification(XDispatcherHeader *h)
{
    return h->Type == XOBJ_EVENT_NOTIFICATION || h->Type == XOBJ_TIMER_NOTIFICATION;
}

HandleObject *handle_of(XHandle h)
{
    if (h >= g_handles.size()) return nullptr;
    return g_handles[h].live ? &g_handles[h] : nullptr;
}

} // namespace

uint32_t xk_timeout_ms(const XLargeInt *timeout)
{
    if (!timeout) return 0xFFFFFFFFu; // INFINITE
    int64_t t = timeout->QuadPart;
    if (t < 0) {
        // -1 (all bits set) is the Xbox's INFINITE; anything else negative is a relative wait.
        if (t == -1) return 0xFFFFFFFFu;
        int64_t ms = (-t + 9999) / 10000;
        return ms > (int64_t)0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)ms;
    }
    // Positive: absolute, in 100 ns units since boot. Callers that mean "now" pass 0.
    uint64_t ms = (uint64_t)t / 10000u;
    return ms > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)ms;
}

uint64_t xk_tick_ms() { return now_ms(); }

// ---------------------------------------------------------------------------
// IRQL
// ---------------------------------------------------------------------------

void xk_init()
{
    g_holds_dispatch = false;
}

uint8_t xk_current_irql() { return t_irql; }

bool xk_holds_dispatch_lock() { return g_holds_dispatch.load(std::memory_order_acquire); }

uint8_t xk_raise_irql(uint8_t new_irql)
{
    uint8_t old = t_irql;
    if (old < IRQL_DISPATCH && new_irql >= IRQL_DISPATCH) {
        g_dispatch_mutex.lock();
        g_holds_dispatch.store(true, std::memory_order_release);
    }
    t_irql = new_irql > old ? new_irql : old; // raising only: a raise never lowers
    return old;
}

void xk_lower_irql(uint8_t new_irql)
{
    uint8_t old = t_irql;
    t_irql = new_irql;
    if (old >= IRQL_DISPATCH && new_irql < IRQL_DISPATCH) {
        g_holds_dispatch.store(false, std::memory_order_release);
        g_dispatch_mutex.unlock();
    }
}

void xk_stall_us(uint32_t us)
{
    struct timespec ts;
    ts.tv_sec = us / 1000000u;
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    // A stall on the Xbox spins; nanosleep is close enough and does not burn a core the loader needs.
    nanosleep(&ts, nullptr);
}

void xk_synchronize_execution(void (*fn)(void *), void *arg)
{
    if (!fn) return;
    uint8_t old = xk_raise_irql(IRQL_DISPATCH);
    fn(arg);
    xk_lower_irql(old);
}

// ---------------------------------------------------------------------------
// dispatcher objects in Xbox memory
// ---------------------------------------------------------------------------

int32_t xk_set_event(XKeEvent *event, int32_t increment, uint8_t wait)
{
    (void)increment;
    (void)wait;
    Wake &w = wake_of(event);
    std::lock_guard<std::mutex> g(w.m);
    int32_t prev = event->Header.SignalState;
    signal_locked(&event->Header, w);
    return prev;
}

void xk_signal_object(XDispatcherHeader *object)
{
    Wake &w = wake_of(object);
    std::lock_guard<std::mutex> g(w.m);
    signal_locked(object, w);
}

void xk_release_object(XDispatcherHeader *object, int32_t count)
{
    Wake &w = wake_of(object);
    std::lock_guard<std::mutex> g(w.m);
    release_locked(object, w, count);
}

NTSTATUS xk_wait_for_object(XDispatcherHeader *object, uint8_t alertable, const XLargeInt *timeout)
{
    if (!object) return XK_INVALID_HANDLE;
    uint32_t ms = xk_timeout_ms(timeout);
    uint64_t deadline = (ms == 0xFFFFFFFFu) ? 0 : now_ms() + ms;
    bool notification = is_notification(object);

    // The object's own mutex is taken before the state is read and held across every sleep, so a
    // signal cannot slip between the predicate check and the wait. gen0 is read here too, under the
    // same lock, for the same reason.
    Wake &w = wake_of(object);
    std::unique_lock<std::mutex> lk(w.m);
    uint64_t gen0 = w.generation;

    for (;;) {
        if (try_consume(object)) return XK_SUCCESS;
        // A notification object is usually already satisfied by the check above, since it stays
        // signalled. This is for the other case: a signal that landed after gen0 was taken but is
        // not visible in the state yet, i.e. "signalled while I waited" rather than "was already
        // signalled".
        if (notification && w.generation != gen0) return XK_SUCCESS;
        if (ms == 0xFFFFFFFFu) {
            w.cv.wait(lk);
        } else {
            uint64_t now = now_ms();
            if (now >= deadline) return XK_TIMEOUT;
            w.cv.wait_for(lk, std::chrono::milliseconds(deadline - now));
        }
        (void)alertable; // no APC delivery on Linux; the game never uses alertable waits
    }
}

// ---------------------------------------------------------------------------
// critical sections
// ---------------------------------------------------------------------------
//
// The Xbox layout is kept exactly (28 bytes, LockCount from -1, OwningThread a KTHREAD pointer),
// because game code reads these fields. Ownership is tracked through a host wake primitive keyed by
// the section's address, and the atomic on LockCount is what decides who owns it.

void xk_initialize_critical_section(XRtlCriticalSection *cs)
{
    if (!cs) return;
    cs->Synchronization.Type = XOBJ_EVENT_SYNCHRONIZATION;
    cs->Synchronization.Absolute = 0;
    cs->Synchronization.Size = (uint8_t)(sizeof(XKeEvent) / 4);
    cs->Synchronization.Inserted = 0;
    cs->Synchronization.SignalState = 0;
    cs->Synchronization.WaitListHead.Flink = cs->Synchronization.WaitListHead.Blink =
        &cs->Synchronization.WaitListHead;
    // -1 so that the first InterlockedIncrement reaches 0 and means "uncontended acquire".
    __atomic_store_n(&cs->LockCount, -1, __ATOMIC_RELEASE);
    cs->RecursionCount = 0;
    cs->OwningThread = nullptr;
}

void xk_enter_critical_section(XRtlCriticalSection *cs)
{
    if (!cs) return;
    void *me = (void *)(uintptr_t)pthread_self();
    // Recursion: the same thread may take it again, and LockCount must not move.
    if (cs->OwningThread == me && cs->RecursionCount > 0) {
        cs->RecursionCount++;
        return;
    }
    Wake &w = wake_of(cs);
    w.m.lock();
    cs->OwningThread = me;
    cs->RecursionCount = 1;
    // The Xbox keeps LockCount at -1 while free and 0 while held uncontended. Only the game reads it
    // (and only to test contention), so that is all it has to be here; mutual exclusion comes from the
    // host mutex, which is what actually works on a many-core machine.
    __atomic_store_n(&cs->LockCount, 0, __ATOMIC_RELEASE);
}

void xk_leave_critical_section(XRtlCriticalSection *cs)
{
    if (!cs) return;
    if (--cs->RecursionCount != 0) return; // still held recursively
    cs->OwningThread = nullptr;
    __atomic_store_n(&cs->LockCount, -1, __ATOMIC_RELEASE);
    wake_of(cs).m.unlock();
}
// ---------------------------------------------------------------------------
// handle-based objects
// ---------------------------------------------------------------------------
//
// A deque, not a vector: HandleObject holds a std::mutex, which is neither copyable nor movable, so a
// vector could never reallocate one. The game only ever holds the handle, so nothing here needs the
// object to live at a stable address.

NTSTATUS xk_create_event(XHandle *out, uint32_t event_type, uint8_t initial_state)
{
    if (!out) return XK_INVALID_PARAMETER;
    std::lock_guard<std::mutex> g(g_handle_mutex);
    for (size_t i = 0; i < g_handles.size(); i++) {
        if (g_handles[i].live) continue;
        HandleObject &o = g_handles[i];
        o.kind = HandleObject::Event;
        o.manual_reset = (event_type == 0); // NotificationEvent is manual reset
        o.count = initial_state ? 1 : 0;
        o.live = true;
        *out = (XHandle)i;
        return XK_SUCCESS;
    }
    g_handles.emplace_back();
    HandleObject &o = g_handles.back();
    o.kind = HandleObject::Event;
    o.manual_reset = (event_type == 0);
    o.count = initial_state ? 1 : 0;
    o.live = true;
    *out = (XHandle)(g_handles.size() - 1);
    return XK_SUCCESS;
}

NTSTATUS xk_set_event_handle(XHandle h, int32_t *previous_state)
{
    HandleObject *o = handle_of(h);
    if (!o) return XK_INVALID_HANDLE;
    std::lock_guard<std::mutex> lk(o->m);
    int32_t prev = o->count;
    o->count = 1;
    if (o->manual_reset) o->cv.notify_all();
    else o->cv.notify_one();
    if (previous_state) *previous_state = prev;
    return XK_SUCCESS;
}

NTSTATUS xk_create_semaphore(XHandle *out, int32_t initial, int32_t maximum)
{
    if (!out || maximum < 0 || initial < 0 || initial > maximum) return XK_INVALID_PARAMETER;
    std::lock_guard<std::mutex> g(g_handle_mutex);
    for (size_t i = 0; i < g_handles.size(); i++) {
        if (g_handles[i].live) continue;
        HandleObject &o = g_handles[i];
        o.kind = HandleObject::Semaphore;
        o.count = initial;
        o.max = maximum;
        o.live = true;
        *out = (XHandle)i;
        return XK_SUCCESS;
    }
    g_handles.emplace_back();
    HandleObject &o = g_handles.back();
    o.kind = HandleObject::Semaphore;
    o.count = initial;
    o.max = maximum;
    o.live = true;
    *out = (XHandle)(g_handles.size() - 1);
    return XK_SUCCESS;
}

NTSTATUS xk_release_semaphore(XHandle h, int32_t count, int32_t *previous_count)
{
    if (count < 0) return XK_INVALID_PARAMETER;
    HandleObject *o = handle_of(h);
    if (!o || o->kind != HandleObject::Semaphore) return XK_INVALID_HANDLE;
    std::lock_guard<std::mutex> lk(o->m);
    int32_t prev = o->count;
    int64_t next = (int64_t)o->count + count;
    if (next > o->max) return XK_UNSUCCESSFUL; // the Xbox kernel refuses to exceed MaximumCount
    o->count = (int32_t)next;
    for (int32_t i = 0; i < count; i++) o->cv.notify_one();
    if (previous_count) *previous_count = prev;
    return XK_SUCCESS;
}

NTSTATUS xk_wait_handle(XHandle h, uint8_t alertable, const XLargeInt *timeout)
{
    HandleObject *o = handle_of(h);
    if (!o) return XK_INVALID_HANDLE;
    uint32_t ms = xk_timeout_ms(timeout);
    uint64_t deadline = (ms == 0xFFFFFFFFu) ? 0 : now_ms() + ms;
    std::unique_lock<std::mutex> lk(o->m);
    for (;;) {
        bool satisfied = o->count > 0;
        if (satisfied && (o->kind == HandleObject::Semaphore || !o->manual_reset)) o->count--;
        if (satisfied) return XK_SUCCESS;
        if (ms == 0xFFFFFFFFu) {
            o->cv.wait(lk);
        } else {
            uint64_t now = now_ms();
            if (now >= deadline) return XK_TIMEOUT;
            o->cv.wait_for(lk, std::chrono::milliseconds(deadline - now));
        }
        (void)alertable; // no APC delivery on Linux; the game never uses alertable waits
    }
}

NTSTATUS xk_close(XHandle h)
{
    std::lock_guard<std::mutex> g(g_handle_mutex);
    if (h >= g_handles.size() || !g_handles[h].live) return XK_INVALID_HANDLE;
    g_handles[h].live = false;
    g_handles[h].count = 0;
    g_handles[h].cv.notify_all();
    return XK_SUCCESS;
}

// ---------------------------------------------------------------------------

uint32_t xk_live_wake_count()
{
    std::lock_guard<std::mutex> g(g_wake_mutex);
    return (uint32_t)g_wake.size();
}

uint32_t xk_live_handle_count()
{
    std::lock_guard<std::mutex> g(g_handle_mutex);
    uint32_t n = 0;
    for (auto &o : g_handles)
        if (o.live) n++;
    return n;
}
