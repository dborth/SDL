/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

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

#include <errno.h>
#include <ogc/mutex.h>

#include "SDL_sysmutex_c.h"

/* Create a mutex */
SDL_Mutex *SDL_CreateMutex(void)
{
    SDL_Mutex *mutex;

    mutex = (SDL_Mutex *)SDL_calloc(1, sizeof(*mutex));
    if (mutex) {
        if (LWP_MutexInit(&mutex->id, true) != 0) {
            SDL_SetError("LWP_MutexInit() failed");
            SDL_free(mutex);
            mutex = NULL;
        }
    }
    return mutex;
}

/* Destroy a mutex */
void SDL_DestroyMutex(SDL_Mutex *mutex)
{
    if (mutex) {
        LWP_MutexDestroy(mutex->id);
        SDL_free(mutex);
    }
}

/* Lock the mutex */
void SDL_LockMutex(SDL_Mutex *mutex) SDL_NO_THREAD_SAFETY_ANALYSIS /* clang doesn't know about NULL mutexes */
{
    if (mutex) {
        const int rc = LWP_MutexLock(mutex->id);
        SDL_assert(rc == 0);  /* assume we're in a lot of trouble if this assert fails. */
        (void)rc;
    }
}

/* Try to lock the mutex */
bool SDL_TryLockMutex(SDL_Mutex *mutex)
{
    int retval = true;

    if (mutex) {
        const int rc = LWP_MutexTryLock(mutex->id);
        if (rc != 0) {
            retval = (rc == EBUSY) ? false : false;
        }
    }
    return retval;
}

/* Unlock the mutex */
void SDL_UnlockMutex(SDL_Mutex *mutex) SDL_NO_THREAD_SAFETY_ANALYSIS /* clang doesn't know about NULL mutexes */
{
    if (mutex) {
        const int rc = LWP_MutexUnlock(mutex->id);
        SDL_assert(rc == 0);  /* assume we're in a lot of trouble if this assert fails. */
        (void)rc;
    }
}

/* vi: set ts=4 sw=4 expandtab: */