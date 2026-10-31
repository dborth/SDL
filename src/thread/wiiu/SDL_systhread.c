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

// Thread management routines for SDL, built on coreinit's OSThread.

#include "../SDL_systhread.h"

#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>

/* Default stack size when SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER is 0. This
   is what wut's own pthread layer uses. */
#define WIIU_THREAD_STACK_SIZE_DEFAULT (128 * 1024)

// Smallest stack we will create, whatever the caller asks for.
#define WIIU_THREAD_STACK_SIZE_MIN (16 * 1024)

/* Run quantum, in microseconds. Without one, threads of equal priority on the
   same core are never time-sliced. Same value as wut's pthread layer. */
#define WIIU_THREAD_RUN_QUANTUM_US 1000

/* coreinit priorities: 0 is the highest, 31 the lowest. Only the thread
   itself can set its priority (SDL_SetCurrentThreadPriority), and new threads
   inherit the creating thread's base priority, as with pthreads. */
#define WIIU_THREAD_PRIORITY_LOW           24
#define WIIU_THREAD_PRIORITY_NORMAL        16
#define WIIU_THREAD_PRIORITY_HIGH          10
#define WIIU_THREAD_PRIORITY_TIME_CRITICAL 3

#define WIIU_ALIGN32(x) (((x) + 31u) & ~(size_t)31u)

/* One allocation from the default heap per thread: this header, then the
   stack. The OSThread must stay first, because the OS hands that address to
   the deallocator. The name is kept here because OSSetThreadName stores only
   the pointer, and SDL frees its own copy of the name when the SDL_Thread goes. */
typedef struct WIIU_Thread
{
    OSThread os;
    char name[32];
} WIIU_Thread;

/* Called by the OS after a detached thread has terminated. This runs while the
   thread is being torn down, so it must only call coreinit. */
static void WIIU_ThreadDeallocator(OSThread *thread, void *stack)
{
    (void)stack;
    MEMFreeToDefaultHeap(thread);
}

static int WIIU_ThreadEntry(int argc, const char **argv)
{
    // The SDL_Thread pointer travels in argc, as in wut's pthread layer.
    (void)argv;
    SDL_RunThread((SDL_Thread *)(uintptr_t)(uint32_t)argc);
    return 0;
}

bool SDL_SYS_CreateThread(SDL_Thread *thread,
                          SDL_FunctionPointer pfnBeginThread,
                          SDL_FunctionPointer pfnEndThread)
{
    size_t stack_size = thread->stacksize ? thread->stacksize : WIIU_THREAD_STACK_SIZE_DEFAULT;
    const size_t header_size = WIIU_ALIGN32(sizeof(WIIU_Thread));

    if (stack_size < WIIU_THREAD_STACK_SIZE_MIN) {
        stack_size = WIIU_THREAD_STACK_SIZE_MIN;
    }
    if (stack_size > (size_t)0x7FFFFFFF) {
        return SDL_SetError("Thread stack size is too large");
    }
    stack_size = WIIU_ALIGN32(stack_size);

    const size_t total_size = header_size + stack_size;
    WIIU_Thread *handle = (WIIU_Thread *)MEMAllocFromDefaultHeapEx((uint32_t)total_size, 32);
    if (!handle) {
        return SDL_OutOfMemory();
    }
    SDL_memset(handle, 0, sizeof(*handle));

    // OSCreateThread takes the address of the top of the stack.
    void *stack_top = (Uint8 *)handle + total_size;
    const int32_t priority = OSGetThreadPriority(OSGetCurrentThread());

    if (!OSCreateThread(&handle->os, WIIU_ThreadEntry, (int32_t)(uintptr_t)thread, NULL,
                        stack_top, (uint32_t)stack_size, priority, OS_THREAD_ATTRIB_AFFINITY_ANY)) {
        MEMFreeToDefaultHeap(handle);
        return SDL_SetError("OSCreateThread() failed");
    }

    OSSetThreadRunQuantum(&handle->os, WIIU_THREAD_RUN_QUANTUM_US);

    // Threads are created suspended. Publish the handle and ID before the new thread can run.
    thread->handle = &handle->os;
    thread->threadid = (SDL_ThreadID)(uintptr_t)&handle->os;
    OSResumeThread(&handle->os);

    return true;
}

void SDL_SYS_SetupThread(const char *name)
{
    // Only called from SDL_RunThread(), so this is always a thread made above.
    OSThread *os = OSGetCurrentThread();
    WIIU_Thread *handle = (WIIU_Thread *)os;

    if (name) {
        SDL_strlcpy(handle->name, name, sizeof(handle->name));
        OSSetThreadName(os, handle->name);
    }

    /* Do not touch the thread's cleanup callback: wut's newlib layer installs
       its own the first time the thread uses libc, to free the per-thread reent. */
}

SDL_ThreadID SDL_GetCurrentThreadID(void)
{
    return (SDL_ThreadID)(uintptr_t)OSGetCurrentThread();
}

bool SDL_SYS_SetThreadPriority(SDL_ThreadPriority sdl_priority)
{
    int32_t priority;

    switch (sdl_priority) {
    case SDL_THREAD_PRIORITY_LOW:
        priority = WIIU_THREAD_PRIORITY_LOW;
        break;
    case SDL_THREAD_PRIORITY_HIGH:
        priority = WIIU_THREAD_PRIORITY_HIGH;
        break;
    case SDL_THREAD_PRIORITY_TIME_CRITICAL:
        priority = WIIU_THREAD_PRIORITY_TIME_CRITICAL;
        break;
    case SDL_THREAD_PRIORITY_NORMAL:
    default:
        priority = WIIU_THREAD_PRIORITY_NORMAL;
        break;
    }

    if (!OSSetThreadPriority(OSGetCurrentThread(), priority)) {
        return SDL_SetError("OSSetThreadPriority() failed");
    }
    return true;
}

void SDL_SYS_WaitThread(SDL_Thread *thread)
{
    WIIU_Thread *handle = (WIIU_Thread *)thread->handle;
    int result = 0;

    // The control block and stack can only go once the join has completed.
    if (OSJoinThread(&handle->os, &result)) {
        MEMFreeToDefaultHeap(handle);
    }
}

void SDL_SYS_DetachThread(SDL_Thread *thread)
{
    /* Read the handle first: SDL_RunThread() may free the SDL_Thread as soon
       as it sees the DETACHED state, which has already been set by now. */
    WIIU_Thread *handle = (WIIU_Thread *)thread->handle;

    /* The deallocator is only set on this path. Joined threads are freed by
       SDL_SYS_WaitThread() instead, so there is no chance of a double free. */
    OSSetThreadDeallocator(&handle->os, WIIU_ThreadDeallocator);
    OSDetachThread(&handle->os);
}

#endif // SDL_THREAD_WIIU
