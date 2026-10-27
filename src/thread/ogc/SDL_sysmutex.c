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

/* Kernel-object-free recursive mutex for libogc2 (see SDL_sysmutex_c.h for why).
 *
 * Contended threads yield briefly and then sleep, instead of blocking in the kernel; the Wii has one
 * core and SDL's critical sections are short. The sleep matters: LWP priorities are strict, so a
 * high-priority thread (the audio thread runs at 80) that merely yields would never let a
 * lower-priority holder run.
 *
 * NOT usable from interrupt context: an ISR that spins on a lock held by the thread it interrupted
 * never returns. Share data with ISRs using an interrupt-disable section instead (see SDL_ogcaudio.c).
 */

/* Create a mutex */
SDL_Mutex *SDL_CreateMutex(void)
{
    return (SDL_Mutex *)SDL_calloc(1, sizeof(SDL_Mutex));
}

/* Destroy a mutex */
void SDL_DestroyMutex(SDL_Mutex *mutex)
{
    SDL_free(mutex);
}

/* Lock the mutex */
void SDL_LockMutex(SDL_Mutex *mutex) SDL_NO_THREAD_SAFETY_ANALYSIS /* clang doesn't know about NULL mutexes */
{
    if (!mutex) {
        return;
    }

    const SDL_ThreadID this_thread = SDL_GetCurrentThreadID();
    if (mutex->owner == this_thread && SDL_GetAtomicInt(&mutex->locked)) {
        ++mutex->recursive;
        return;
    }

    int spins = 0;
    while (!SDL_CompareAndSwapAtomicInt(&mutex->locked, 0, 1)) {
        if (spins < 4) {
            ++spins;
            LWP_YieldThread();          /* lets an equal-priority holder run */
        } else {
            SDL_DelayNS(100 * 1000);    /* holder is blocked or lower priority: really give up the CPU */
        }
    }
    mutex->owner = this_thread;
    mutex->recursive = 0;
}

/* Try to lock the mutex */
bool SDL_TryLockMutex(SDL_Mutex *mutex)
{
    if (!mutex) {
        return true;
    }

    const SDL_ThreadID this_thread = SDL_GetCurrentThreadID();
    if (mutex->owner == this_thread && SDL_GetAtomicInt(&mutex->locked)) {
        ++mutex->recursive;
        return true;
    }
    if (!SDL_CompareAndSwapAtomicInt(&mutex->locked, 0, 1)) {
        return false;
    }
    mutex->owner = this_thread;
    mutex->recursive = 0;
    return true;
}

/* Unlock the mutex */
void SDL_UnlockMutex(SDL_Mutex *mutex) SDL_NO_THREAD_SAFETY_ANALYSIS /* clang doesn't know about NULL mutexes */
{
    if (!mutex) {
        return;
    }

    /* We can only unlock the mutex if we own it */
    if (mutex->owner != SDL_GetCurrentThreadID() || !SDL_GetAtomicInt(&mutex->locked)) {
        SDL_SetError("mutex not owned by this thread");
        return;
    }

    if (mutex->recursive) {
        --mutex->recursive;
        return;
    }

    mutex->owner = 0;                    /* clear the owner before releasing, so a new owner is never overwritten */
    SDL_SetAtomicInt(&mutex->locked, 0); /* (SDL's atomics include the needed barriers) */
}

/* vi: set ts=4 sw=4 expandtab: */
