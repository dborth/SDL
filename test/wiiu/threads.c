/*
  threads, mutex, semaphore, condition tests for the SDL3 Wii U port:
  TLS, rwlock, timers. All output goes through SDL_Log (OSReport on Wii U).
  Returns 0 if every check passed.

  Checks that depend on timing use wide margins so they also hold on Cemu.
  Lines starting with "INFO" are for the reader and are not pass/fail.
*/
#include <SDL3/SDL.h>

#include "console.h"

#include <coreinit/core.h>
#include <coreinit/thread.h>

static int failures;

static void check(const char *what, bool ok)
{
    SDL_Log("%s: %s", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static Uint64 elapsed_ms(Uint64 start_ns)
{
    return (SDL_GetTicksNS() - start_ns) / SDL_NS_PER_MS;
}

// ---------------------------------------------------------------- info

static void test_info(void)
{
    OSThread *self = OSGetCurrentThread();
    SDL_Log("INFO main thread: core %u, priority %d, cores %u",
            (unsigned)OSGetCoreId(), (int)OSGetThreadPriority(self), (unsigned)OSGetCoreCount());
    check("SDL_GetNumLogicalCPUCores() == 3", SDL_GetNumLogicalCPUCores() == 3);
}

// ------------------------------------------------- create, join, ID, name

static int status_thread(void *data)
{
    SDL_ThreadID *seen_id = (SDL_ThreadID *)data;
    *seen_id = SDL_GetCurrentThreadID();
    const char *os_name = OSGetThreadName(OSGetCurrentThread());
    SDL_Log("INFO thread says its OS name is \"%s\", priority %d",
            os_name ? os_name : "(null)", (int)OSGetThreadPriority(OSGetCurrentThread()));
    return os_name && SDL_strcmp(os_name, "p1-status") == 0 ? 42 : 7;
}

static void test_create_join(void)
{
    SDL_ThreadID seen_id = 0;
    SDL_Thread *t = SDL_CreateThread(status_thread, "p1-status", &seen_id);
    check("SDL_CreateThread", t != NULL);
    if (!t) {
        return;
    }
    const SDL_ThreadID creator_view = SDL_GetThreadID(t);
    check("SDL_GetThreadName", t && SDL_strcmp(SDL_GetThreadName(t), "p1-status") == 0);

    int status = -1;
    SDL_WaitThread(t, &status);
    check("thread return value is passed to SDL_WaitThread (42 means the OS thread name was set too)", status == 42);
    check("SDL_GetThreadID() matches SDL_GetCurrentThreadID() inside the thread",
          creator_view != 0 && creator_view == seen_id);
    check("thread ID differs from the main thread's", seen_id != SDL_GetCurrentThreadID());
}

// ------------------------------------------------- threads on several cores

#define NUM_CORE_THREADS 3

static int core_thread(void *data)
{
    unsigned *core_out = (unsigned *)data;
    unsigned seen = 0;
    const Uint64 start = SDL_GetTicksNS();
    // Stay busy for a while so the scheduler has to spread the threads out.
    while (elapsed_ms(start) < 300) {
        seen |= 1u << OSGetCoreId();
    }
    *core_out = seen;
    return 0;
}

static void test_cores(void)
{
    SDL_Thread *t[NUM_CORE_THREADS];
    unsigned seen[NUM_CORE_THREADS] = { 0 };
    unsigned all = 0;

    for (int i = 0; i < NUM_CORE_THREADS; i++) {
        t[i] = SDL_CreateThread(core_thread, "p1-core", &seen[i]);
    }
    for (int i = 0; i < NUM_CORE_THREADS; i++) {
        SDL_WaitThread(t[i], NULL);
        all |= seen[i];
    }
    SDL_Log("INFO core mask seen by %d busy threads: 0x%x", NUM_CORE_THREADS, all);
    check("busy threads ran on at least 2 different cores", (all & (all - 1)) != 0);
}

// ------------------------------------------------------------------ mutex

#define MUTEX_THREADS    4
#define MUTEX_ITERATIONS 10000

static SDL_Mutex *g_mutex;
static int g_counter;

static int mutex_thread(void *data)
{
    (void)data;
    for (int i = 0; i < MUTEX_ITERATIONS; i++) {
        SDL_LockMutex(g_mutex);
        g_counter++;
        SDL_UnlockMutex(g_mutex);
    }
    return 0;
}

static int trylock_thread(void *data)
{
    // Returns 1 if the mutex held by the creator could NOT be taken.
    (void)data;
    const bool got = SDL_TryLockMutex(g_mutex);
    if (got) {
        SDL_UnlockMutex(g_mutex);
    }
    return got ? 0 : 1;
}

static void test_mutex(void)
{
    SDL_Thread *t[MUTEX_THREADS];

    g_mutex = SDL_CreateMutex();
    check("SDL_CreateMutex", g_mutex != NULL);
    if (!g_mutex) {
        return;
    }

    g_counter = 0;
    for (int i = 0; i < MUTEX_THREADS; i++) {
        t[i] = SDL_CreateThread(mutex_thread, "p1-mutex", NULL);
    }
    for (int i = 0; i < MUTEX_THREADS; i++) {
        SDL_WaitThread(t[i], NULL);
    }
    check("mutex protects a shared counter across threads", g_counter == MUTEX_THREADS * MUTEX_ITERATIONS);

    SDL_LockMutex(g_mutex);
    check("mutex is recursive (SDL_TryLockMutex from the owner)", SDL_TryLockMutex(g_mutex));
    SDL_UnlockMutex(g_mutex);
    int not_taken = 0;
    SDL_Thread *tl = SDL_CreateThread(trylock_thread, "p1-trylock", NULL);
    SDL_WaitThread(tl, &not_taken);
    check("SDL_TryLockMutex fails in another thread while the mutex is held", not_taken == 1);
    SDL_UnlockMutex(g_mutex);

    SDL_DestroyMutex(g_mutex);
    g_mutex = NULL;
}

// -------------------------------------------------------------- semaphore

static SDL_Semaphore *g_sem;
static SDL_AtomicInt g_woken;

static int sem_waiter(void *data)
{
    (void)data;
    SDL_WaitSemaphore(g_sem);
    SDL_AddAtomicInt(&g_woken, 1);
    return 0;
}

static int sem_poster(void *data)
{
    SDL_Delay((Uint32)(uintptr_t)data);
    SDL_SignalSemaphore(g_sem);
    return 0;
}

static void test_semaphore(void)
{
    g_sem = SDL_CreateSemaphore(0);
    check("SDL_CreateSemaphore", g_sem != NULL);
    if (!g_sem) {
        return;
    }

    check("SDL_TryWaitSemaphore on an empty semaphore fails", !SDL_TryWaitSemaphore(g_sem));
    check("SDL_WaitSemaphoreTimeout(0) on an empty semaphore fails", !SDL_WaitSemaphoreTimeout(g_sem, 0));

    Uint64 start = SDL_GetTicksNS();
    bool ok = SDL_WaitSemaphoreTimeout(g_sem, 150);
    Uint64 ms = elapsed_ms(start);
    SDL_Log("INFO a 150 ms semaphore timeout took %llu ms", (unsigned long long)ms);
    check("timed wait times out", !ok);
    check("timed wait lasted at least 150 ms", ms >= 150);
    check("timed wait lasted under 400 ms", ms < 400);

    SDL_SignalSemaphore(g_sem);
    check("SDL_GetSemaphoreValue after a post", SDL_GetSemaphoreValue(g_sem) == 1);
    check("SDL_TryWaitSemaphore takes a posted count", SDL_TryWaitSemaphore(g_sem));
    check("SDL_GetSemaphoreValue after the take", SDL_GetSemaphoreValue(g_sem) == 0);

    // A post from another thread must cut a long timed wait short.
    SDL_Thread *poster = SDL_CreateThread(sem_poster, "p1-poster", (void *)(uintptr_t)100);
    start = SDL_GetTicksNS();
    ok = SDL_WaitSemaphoreTimeout(g_sem, 5000);
    ms = elapsed_ms(start);
    SDL_WaitThread(poster, NULL);
    SDL_Log("INFO a post from another thread woke a 5000 ms wait after %llu ms", (unsigned long long)ms);
    check("timed wait is woken by a post from another thread", ok && ms < 2000);

    // Three blocked waiters, three posts at once: all three must wake. This
    // is the case where one signalled event stands for several posts.
    SDL_SetAtomicInt(&g_woken, 0);
    SDL_Thread *w[3];
    for (int i = 0; i < 3; i++) {
        w[i] = SDL_CreateThread(sem_waiter, "p1-semwait", NULL);
    }
    SDL_Delay(200); // let them block
    check("waiters are blocked before the posts", SDL_GetAtomicInt(&g_woken) == 0);
    SDL_SignalSemaphore(g_sem);
    SDL_SignalSemaphore(g_sem);
    SDL_SignalSemaphore(g_sem);
    Uint64 deadline = SDL_GetTicksNS() + SDL_MS_TO_NS(3000);
    while (SDL_GetAtomicInt(&g_woken) < 3 && SDL_GetTicksNS() < deadline) {
        SDL_Delay(10);
    }
    check("3 posts wake 3 blocked waiters", SDL_GetAtomicInt(&g_woken) == 3);
    for (int i = 0; i < 3; i++) {
        if (SDL_GetAtomicInt(&g_woken) == 3) {
            SDL_WaitThread(w[i], NULL);
        } else {
            SDL_DetachThread(w[i]);
        }
    }
    check("count is 0 after the waiters took their posts", SDL_GetSemaphoreValue(g_sem) == 0);

    // Producer/consumer through a semaphore across threads.
    SDL_SetAtomicInt(&g_woken, 0);
    SDL_Thread *consumers[4];
    for (int i = 0; i < 4; i++) {
        consumers[i] = SDL_CreateThread(sem_waiter, "p1-semwait", NULL);
    }
    for (int i = 0; i < 4; i++) {
        SDL_SignalSemaphore(g_sem);
        SDL_Delay(5);
    }
    for (int i = 0; i < 4; i++) {
        SDL_WaitThread(consumers[i], NULL);
    }
    check("4 posts, 4 consumers, all joined", SDL_GetAtomicInt(&g_woken) == 4);

    SDL_DestroySemaphore(g_sem);
    g_sem = NULL;
}

// -------------------------------------------------------------- condition

static SDL_Mutex *g_cmutex;
static SDL_Condition *g_cond;
static bool g_flag;
static SDL_AtomicInt g_cwoken;

static int cond_waiter(void *data)
{
    (void)data;
    SDL_LockMutex(g_cmutex);
    while (!g_flag) {
        SDL_WaitCondition(g_cond, g_cmutex);
    }
    SDL_UnlockMutex(g_cmutex);
    SDL_AddAtomicInt(&g_cwoken, 1);
    return 0;
}

static void test_condition(void)
{
    g_cmutex = SDL_CreateMutex();
    g_cond = SDL_CreateCondition();
    check("SDL_CreateCondition", g_cmutex && g_cond);
    if (!g_cmutex || !g_cond) {
        return;
    }

    SDL_LockMutex(g_cmutex);
    Uint64 start = SDL_GetTicksNS();
    bool ok = SDL_WaitConditionTimeout(g_cond, g_cmutex, 150);
    Uint64 ms = elapsed_ms(start);
    SDL_UnlockMutex(g_cmutex);
    SDL_Log("INFO a 150 ms condition timeout took %llu ms", (unsigned long long)ms);
    check("SDL_WaitConditionTimeout times out", !ok && ms >= 150 && ms < 400);

    g_flag = false;
    SDL_SetAtomicInt(&g_cwoken, 0);
    SDL_Thread *w[3];
    for (int i = 0; i < 3; i++) {
        w[i] = SDL_CreateThread(cond_waiter, "p1-condwait", NULL);
    }
    SDL_Delay(200);
    check("condition waiters are blocked", SDL_GetAtomicInt(&g_cwoken) == 0);
    SDL_LockMutex(g_cmutex);
    g_flag = true;
    SDL_BroadcastCondition(g_cond);
    SDL_UnlockMutex(g_cmutex);
    for (int i = 0; i < 3; i++) {
        SDL_WaitThread(w[i], NULL);
    }
    check("SDL_BroadcastCondition wakes all waiters", SDL_GetAtomicInt(&g_cwoken) == 3);

    g_flag = false;
    SDL_SetAtomicInt(&g_cwoken, 0);
    SDL_Thread *one = SDL_CreateThread(cond_waiter, "p1-condwait", NULL);
    SDL_Delay(100);
    SDL_LockMutex(g_cmutex);
    g_flag = true;
    SDL_SignalCondition(g_cond);
    SDL_UnlockMutex(g_cmutex);
    SDL_WaitThread(one, NULL);
    check("SDL_SignalCondition wakes a waiter", SDL_GetAtomicInt(&g_cwoken) == 1);

    SDL_DestroyCondition(g_cond);
    SDL_DestroyMutex(g_cmutex);
}

// ---------------------------------------------------------------- rwlock

static SDL_RWLock *g_rw;
static int g_rw_value;
static SDL_AtomicInt g_rw_bad;

static int rw_reader(void *data)
{
    (void)data;
    for (int i = 0; i < 2000; i++) {
        SDL_LockRWLockForReading(g_rw);
        if ((g_rw_value & 1) != 0) { // writers keep it even outside the lock
            SDL_AddAtomicInt(&g_rw_bad, 1);
        }
        SDL_UnlockRWLock(g_rw);
    }
    return 0;
}

static int rw_writer(void *data)
{
    (void)data;
    for (int i = 0; i < 2000; i++) {
        SDL_LockRWLockForWriting(g_rw);
        g_rw_value++;
        g_rw_value++;
        SDL_UnlockRWLock(g_rw);
    }
    return 0;
}

static void test_rwlock(void)
{
    g_rw = SDL_CreateRWLock();
    check("SDL_CreateRWLock", g_rw != NULL);
    if (!g_rw) {
        return;
    }
    SDL_Thread *t[4];
    t[0] = SDL_CreateThread(rw_reader, "p1-rd", NULL);
    t[1] = SDL_CreateThread(rw_reader, "p1-rd", NULL);
    t[2] = SDL_CreateThread(rw_writer, "p1-wr", NULL);
    t[3] = SDL_CreateThread(rw_writer, "p1-wr", NULL);
    for (int i = 0; i < 4; i++) {
        SDL_WaitThread(t[i], NULL);
    }
    check("rwlock readers never see a half-done write", SDL_GetAtomicInt(&g_rw_bad) == 0);
    check("rwlock writers do not lose updates", g_rw_value == 2 * 2 * 2000);
    SDL_DestroyRWLock(g_rw);
}

// -------------------------------------------------------------------- TLS

static SDL_TLSID g_tls;
static SDL_AtomicInt g_tls_destroyed;

static void SDLCALL tls_destructor(void *value)
{
    (void)value;
    SDL_AddAtomicInt(&g_tls_destroyed, 1);
}

static int tls_thread(void *data)
{
    int mine = (int)(uintptr_t)data;
    int bad = 0;

    check("SDL_GetTLS is NULL in a new thread", SDL_GetTLS(&g_tls) == NULL);
    SDL_SetTLS(&g_tls, (void *)(uintptr_t)mine, tls_destructor);
    for (int i = 0; i < 50; i++) {
        SDL_Delay(2);
        if ((int)(uintptr_t)SDL_GetTLS(&g_tls) != mine) {
            bad++;
        }
    }
    return bad;
}

static void test_tls(void)
{
    SDL_Thread *t[4];
    int bad_total = 0;

    SDL_SetTLS(&g_tls, (void *)(uintptr_t)1000, NULL);
    for (int i = 0; i < 4; i++) {
        t[i] = SDL_CreateThread(tls_thread, "p1-tls", (void *)(uintptr_t)(i + 1));
    }
    for (int i = 0; i < 4; i++) {
        int bad = 0;
        SDL_WaitThread(t[i], &bad);
        bad_total += bad;
    }
    check("TLS values stay per-thread", bad_total == 0);
    check("main thread's TLS value is untouched", (int)(uintptr_t)SDL_GetTLS(&g_tls) == 1000);
    check("TLS destructors ran as the 4 threads exited", SDL_GetAtomicInt(&g_tls_destroyed) == 4);
}

// -------------------------------------------------------------- priority

static int priority_thread(void *data)
{
    int *out = (int *)data;
    OSThread *self = OSGetCurrentThread();
    out[0] = (int)OSGetThreadPriority(self);
    bool ok = SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    out[1] = ok ? (int)OSGetThreadPriority(self) : -1;
    ok = SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    out[2] = ok ? (int)OSGetThreadPriority(self) : -1;
    ok = SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
    out[3] = ok ? (int)OSGetThreadPriority(self) : -1;
    ok = SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_NORMAL);
    out[4] = ok ? (int)OSGetThreadPriority(self) : -1;
    return 0;
}

static void test_priority(void)
{
    int p[5] = { -1, -1, -1, -1, -1 };
    const int main_priority = (int)OSGetThreadPriority(OSGetCurrentThread());

    SDL_Thread *t = SDL_CreateThread(priority_thread, "p1-prio", p);
    SDL_WaitThread(t, NULL);
    SDL_Log("INFO priorities: inherited %d, low %d, high %d, time-critical %d, normal %d", p[0], p[1], p[2], p[3], p[4]);
    check("a new thread inherits the creator's priority", p[0] == main_priority);
    check("LOW < NORMAL < HIGH < TIME_CRITICAL (coreinit: lower number is higher priority)",
          p[1] > p[4] && p[4] > p[2] && p[2] > p[3] && p[3] >= 0);
}

// ---------------------------------------------------------------- detach

static SDL_Semaphore *g_detach_done;

static int detach_thread(void *data)
{
    (void)data;
    SDL_SignalSemaphore(g_detach_done);
    return 0;
}

static void test_detach(void)
{
    // Large stacks so that a leak of detached threads would run the heap out
    // quickly (300 x 1 MiB). Hardware and Cemu are the only places this means anything.
    enum { COUNT = 300 };
    int created = 0;

    g_detach_done = SDL_CreateSemaphore(0);
    for (int i = 0; i < COUNT; i++) {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)detach_thread);
        SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "p1-detach");
        SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 1024 * 1024);
        SDL_Thread *t = SDL_CreateThreadWithProperties(props);
        SDL_DestroyProperties(props);
        if (!t) {
            SDL_Log("INFO SDL_CreateThread failed at %d: %s", i, SDL_GetError());
            break;
        }
        created++;
        // Alternate: detach before and after the thread has had time to finish.
        if (i & 1) {
            SDL_Delay(2);
        }
        SDL_DetachThread(t);
        if (!SDL_WaitSemaphoreTimeout(g_detach_done, 5000)) {
            break;
        }
    }
    SDL_Delay(100);
    check("created and detached 300 threads with 1 MiB stacks (a leak would show here)", created == COUNT);
    SDL_DestroySemaphore(g_detach_done);
}

// ---------------------------------------------------------------- stacks

static int stack_thread(void *data)
{
    // Touch a good part of a custom stack.
    volatile char buf[48 * 1024];
    for (size_t i = 0; i < sizeof(buf); i += 64) {
        buf[i] = (char)i;
    }
    (void)data;
    return (int)buf[64];
}

static void test_stack_size(void)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)stack_thread);
    SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "p1-stack");
    SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 128 * 1024);
    SDL_Thread *t = SDL_CreateThreadWithProperties(props);
    SDL_DestroyProperties(props);
    int status = -1;
    SDL_WaitThread(t, &status);
    check("thread with a custom 128 KiB stack uses 48 KiB of it", t != NULL && status == 64);
}

// ------------------------------------------------------- timers and time

static SDL_AtomicInt g_timer_fired;

static Uint32 SDLCALL timer_cb(void *userdata, SDL_TimerID id, Uint32 interval)
{
    (void)userdata;
    (void)id;
    (void)interval;
    SDL_AddAtomicInt(&g_timer_fired, 1);
    return 0; // one shot
}

static void test_timers(void)
{
    // Monotonic
    bool monotonic = true;
    Uint64 last = SDL_GetTicksNS();
    Uint64 last_pc = SDL_GetPerformanceCounter();
    for (int i = 0; i < 100000; i++) {
        const Uint64 now = SDL_GetTicksNS();
        const Uint64 pc = SDL_GetPerformanceCounter();
        if (now < last || pc < last_pc) {
            monotonic = false;
            break;
        }
        last = now;
        last_pc = pc;
    }
    check("SDL_GetTicksNS and SDL_GetPerformanceCounter are monotonic over 100000 reads", monotonic);
    SDL_Log("INFO performance frequency: %llu Hz", (unsigned long long)SDL_GetPerformanceFrequency());

    // Delays
    Uint64 start = SDL_GetTicksNS();
    SDL_DelayNS(1500000);
    Uint64 ns = SDL_GetTicksNS() - start;
    check("SDL_DelayNS(1.5 ms) lasts at least 1.5 ms", ns >= 1500000);

    start = SDL_GetTicksNS();
    for (int i = 0; i < 1000; i++) {
        SDL_Delay(0);
    }
    ns = SDL_GetTicksNS() - start;
    SDL_Log("INFO 1000 x SDL_Delay(0) took %llu us", (unsigned long long)(ns / 1000));
    check("1000 x SDL_Delay(0) returns in under 2 s", ns < 2000ull * SDL_NS_PER_MS);

    // SDL_AddTimer: runs on SDL's timer thread and waits on a semaphore with a timeout.
    SDL_SetAtomicInt(&g_timer_fired, 0);
    SDL_TimerID id = SDL_AddTimer(100, timer_cb, NULL);
    check("SDL_AddTimer", id != 0);
    start = SDL_GetTicksNS();
    while (SDL_GetAtomicInt(&g_timer_fired) == 0 && elapsed_ms(start) < 2000) {
        SDL_Delay(10);
    }
    const Uint64 fired_after = elapsed_ms(start);
    SDL_Log("INFO a 100 ms timer fired after %llu ms", (unsigned long long)fired_after);
    check("a 100 ms SDL timer fires, and not early", SDL_GetAtomicInt(&g_timer_fired) == 1 && fired_after >= 90);

    // Wall clock: Unix epoch, sub-second resolution
    SDL_Time t1, t2;
    SDL_DateTime dt;
    bool subsecond = false;
    check("SDL_GetCurrentTime", SDL_GetCurrentTime(&t1));
    for (int i = 0; i < 20; i++) {
        SDL_GetCurrentTime(&t2);
        if (t2 % SDL_NS_PER_SECOND != 0) {
            subsecond = true;
        }
        SDL_Delay(3);
    }
    check("SDL_GetCurrentTime has sub-second resolution", subsecond);
    check("SDL_GetCurrentTime does not go backwards", t2 >= t1);
    if (SDL_TimeToDateTime(t2, &dt, false)) {
        SDL_Log("INFO UTC now: %04d-%02d-%02d %02d:%02d:%02d.%09d", dt.year, dt.month, dt.day,
                dt.hour, dt.minute, dt.second, dt.nanosecond);
        check("wall clock is in the Unix epoch (year >= 2024)", dt.year >= 2024 && dt.year < 2100);
    } else {
        check("SDL_TimeToDateTime", false);
    }
    check("SDL_TimeToDateTime handles a time before 1970 (1969-12-31 23:59:59.5)",
          SDL_TimeToDateTime(-500000000, &dt, false) && dt.year == 1969 && dt.month == 12 &&
              dt.day == 31 && dt.hour == 23 && dt.minute == 59 && dt.second == 59 &&
              dt.nanosecond == 500000000);
}

// ------------------------------------------------------------- spinlock

static SDL_SpinLock g_spin;
static int g_spin_counter;

static int spin_thread(void *data)
{
    (void)data;
    for (int i = 0; i < 20000; i++) {
        SDL_LockSpinlock(&g_spin);
        g_spin_counter++;
        SDL_UnlockSpinlock(&g_spin);
    }
    return 0;
}

static void test_spinlock(void)
{
    SDL_Thread *t[3];
    for (int i = 0; i < 3; i++) {
        t[i] = SDL_CreateThread(spin_thread, "p1-spin", NULL);
    }
    for (int i = 0; i < 3; i++) {
        SDL_WaitThread(t[i], NULL);
    }
    check("contended spinlock across 3 threads", g_spin_counter == 3 * 20000);
}

int tool_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    SDL_Log("SDL %d.%d.%d on %s: P1 thread tests", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
            SDL_MICRO_VERSION, SDL_GetPlatform());
    check("SDL_Init(0)", SDL_Init(0));

    test_info();
    test_create_join();
    test_cores();
    test_mutex();
    test_semaphore();
    test_condition();
    test_rwlock();
    test_tls();
    test_priority();
    test_stack_size();
    test_spinlock();
    test_timers();
    test_detach();

    SDL_Quit();
    SDL_Log("Done: %d failure(s)", failures);
    return failures ? 1 : 0;
}
