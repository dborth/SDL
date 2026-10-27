/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#include <ogc/lwp.h>

#include "SDL_sysmutex_c.h"

/* Kernel-object-free condition variable for libogc2, to go with SDL_sysmutex.c (see SDL_sysmutex_c.h).
 * Waiters poll: they yield briefly, then sleep.
 *
 * Semantics: a counter of waiters and a counter of pending wakeups. SDL_SignalCondition() grants one
 * wakeup if a waiter exists; SDL_BroadcastCondition() grants one per current waiter. A wakeup goes to
 * whichever waiter polls first, not necessarily the longest-waiting one, which is fine for the usual
 * "wait in a loop on a predicate" use that SDL (and its generic rwlock) rely on.
 * As with any condition variable, the mutex must be held (once) when calling the wait function.
 */

struct SDL_Condition
{
    SDL_AtomicInt waiters;
    SDL_AtomicInt signals;   /* wakeups granted but not yet consumed (clamped to `waiters`) */
};

/* Create a condition variable */
SDL_Condition *SDL_CreateCondition(void)
{
    return (SDL_Condition *)SDL_calloc(1, sizeof(SDL_Condition));
}

/* Destroy a condition variable */
void SDL_DestroyCondition(SDL_Condition *cond)
{
    SDL_free(cond);
}

/* Restart one of the threads that are waiting on the condition variable */
void SDL_SignalCondition(SDL_Condition *cond)
{
    if (!cond) {
        return;
    }
    for (;;) {
        const int waiters = SDL_GetAtomicInt(&cond->waiters);
        const int signals = SDL_GetAtomicInt(&cond->signals);
        if (signals >= waiters) {
            return; /* nobody (left) to wake */
        }
        if (SDL_CompareAndSwapAtomicInt(&cond->signals, signals, signals + 1)) {
            return;
        }
    }
}

/* Restart all threads that are waiting on the condition variable */
void SDL_BroadcastCondition(SDL_Condition *cond)
{
    if (!cond) {
        return;
    }
    for (;;) {
        const int waiters = SDL_GetAtomicInt(&cond->waiters);
        const int signals = SDL_GetAtomicInt(&cond->signals);
        if (signals >= waiters || SDL_CompareAndSwapAtomicInt(&cond->signals, signals, waiters)) {
            return;
        }
    }
}

/* Wait on the condition variable for at most 'timeoutNS' nanoseconds (negative = forever).
   The mutex must be locked before entering this function!
   The mutex is unlocked during the wait, and locked again after the wait.

Typical use:

Thread A:
    SDL_LockMutex(lock);
    while ( ! condition ) {
        SDL_WaitCondition(cond, lock);
    }
    SDL_UnlockMutex(lock);

Thread B:
    SDL_LockMutex(lock);
    ...
    condition = true;
    ...
    SDL_SignalCondition(cond);
    SDL_UnlockMutex(lock);
 */
bool SDL_WaitConditionTimeoutNS(SDL_Condition *cond, SDL_Mutex *mutex, Sint64 timeoutNS)
{
    if (!cond) {
        return SDL_InvalidParamError("cond");
    }
    if (!mutex) {
        return SDL_InvalidParamError("mutex");
    }

    const Uint64 deadline = (timeoutNS < 0) ? 0 : SDL_GetTicksNS() + (Uint64)timeoutNS;
    bool signaled = false;
    int spins = 0;

    SDL_AddAtomicInt(&cond->waiters, 1);   /* registered while still holding the mutex, so no signal is missed */
    SDL_UnlockMutex(mutex);

    for (;;) {
        const int signals = SDL_GetAtomicInt(&cond->signals);
        if (signals > 0 && SDL_CompareAndSwapAtomicInt(&cond->signals, signals, signals - 1)) {
            signaled = true;
            break;
        }
        if (timeoutNS >= 0 && SDL_GetTicksNS() >= deadline) {
            break;
        }
        if (spins < 4) {
            ++spins;
            LWP_YieldThread();
        } else {
            SDL_DelayNS(200 * 1000);
        }
    }

    SDL_AddAtomicInt(&cond->waiters, -1);
    for (;;) { /* a timed-out waiter must not leave a wakeup behind for a waiter that isn't there */
        const int waiters = SDL_GetAtomicInt(&cond->waiters);
        const int signals = SDL_GetAtomicInt(&cond->signals);
        if (signals <= waiters || SDL_CompareAndSwapAtomicInt(&cond->signals, signals, waiters)) {
            break;
        }
    }

    SDL_LockMutex(mutex);
    return signaled;
}

/* vi: set ts=4 sw=4 expandtab: */
