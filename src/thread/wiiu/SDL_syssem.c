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

#ifdef SDL_THREAD_WIIU

// An implementation of semaphores using coreinit's OSMutex and OSEvent.
//
// coreinit's OSSemaphore has no timed wait, and timing out with an OSAlarm
// plus an OSCondition can lose the wakeup if the alarm fires just before the
// waiter goes to sleep. OSWaitEventWithTimeout is the OS's own timed wait, so
// the count lives under a mutex and an auto-reset OSEvent is used as the
// "something was posted" token.

#include <coreinit/event.h>
#include <coreinit/mutex.h>
#include <coreinit/time.h>

struct SDL_Semaphore
{
    OSMutex lock;
    OSEvent event; // auto-reset: OSSignalEvent wakes one waiter, or stays set if there are none
    Uint32 count;
};

// Longest single OSWaitEventWithTimeout; longer waits are done in slices.
#define WIIU_SEM_MAX_WAIT_NS (SDL_NS_PER_SECOND * 60 * 60)

// Converts nanoseconds to coreinit ticks, rounding up (never wake early) and
// never returning 0 (a zero timeout would just poll).
static OSTime NSToTicks(Uint64 ns)
{
    const Uint64 freq = (Uint64)OSTimerClockSpeed;
    const Uint64 secs = ns / SDL_NS_PER_SECOND;
    const Uint64 rem_ns = ns % SDL_NS_PER_SECOND;
    Uint64 ticks = secs * freq + (rem_ns * freq) / SDL_NS_PER_SECOND;

    if ((rem_ns * freq) % SDL_NS_PER_SECOND != 0) {
        ticks++;
    }
    if (ticks == 0) {
        ticks = 1;
    }
    return (OSTime)ticks;
}

static bool TakeIfAvailable(SDL_Semaphore *sem)
{
    bool taken = false;

    OSLockMutex(&sem->lock);
    if (sem->count > 0) {
        sem->count--;
        taken = true;
        if (sem->count > 0) {
            // Several posts can coalesce into one signalled event, so pass the
            // wakeup on to the next waiter.
            OSSignalEvent(&sem->event);
        }
    }
    OSUnlockMutex(&sem->lock);

    return taken;
}

SDL_Semaphore *SDL_CreateSemaphore(Uint32 initial_value)
{
    SDL_Semaphore *sem = (SDL_Semaphore *)SDL_malloc(sizeof(*sem));
    if (!sem) {
        return NULL;
    }

    OSInitMutex(&sem->lock);
    OSInitEvent(&sem->event, FALSE, OS_EVENT_MODE_AUTO);
    sem->count = initial_value;
    return sem;
}

// WARNING: you cannot call this function when another thread is using the semaphore.
void SDL_DestroySemaphore(SDL_Semaphore *sem)
{
    SDL_free(sem);
}

bool SDL_WaitSemaphoreTimeoutNS(SDL_Semaphore *sem, Sint64 timeoutNS)
{
    if (!sem) {
        return true;
    }

    if (timeoutNS == 0) {
        return TakeIfAvailable(sem);
    }

    const bool forever = (timeoutNS < 0);
    const Uint64 deadline = forever ? 0 : (SDL_GetTicksNS() + (Uint64)timeoutNS);

    for (;;) {
        // Always try to take first, so a wakeup that raced with a timeout is not lost.
        if (TakeIfAvailable(sem)) {
            return true;
        }

        if (forever) {
            OSWaitEvent(&sem->event);
        } else {
            const Uint64 now = SDL_GetTicksNS();
            if (now >= deadline) {
                return false;
            }
            const Uint64 remaining = SDL_min(deadline - now, (Uint64)WIIU_SEM_MAX_WAIT_NS);
            // The result is ignored: whether we were signalled or timed out,
            // the loop re-checks the count and the deadline.
            OSWaitEventWithTimeout(&sem->event, NSToTicks(remaining));
        }
    }
}

Uint32 SDL_GetSemaphoreValue(SDL_Semaphore *sem)
{
    Uint32 value = 0;

    if (sem) {
        OSLockMutex(&sem->lock);
        value = sem->count;
        OSUnlockMutex(&sem->lock);
    }
    return value;
}

void SDL_SignalSemaphore(SDL_Semaphore *sem)
{
    if (!sem) {
        return;
    }

    OSLockMutex(&sem->lock);
    sem->count++;
    OSUnlockMutex(&sem->lock);

    OSSignalEvent(&sem->event);
}

#endif // SDL_THREAD_WIIU
